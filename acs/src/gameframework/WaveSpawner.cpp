// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L/R — FWaveSpawner 実装
//
// state machine + 複数 rule 並列 spawn + intermission timer の合成。
// FSpawnRule.spawn_interval_sec が小さい / dt が大きいケースでは 1 Tick 内で
// 複数 spawn が連鎖発火する (= carry 方式)。state 遷移も連鎖し得るので、
// Tick() は state ループとして書く。
#include "gameframework/WaveSpawner.h"
#include "foundation/Log.h"
#include "foundation/Move.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// construction / lifecycle
// ----------------------------------------------------------------------------

FWaveSpawner::FWaveSpawner() noexcept {
    _waves.Reserve(kWaveReserveHint);
}

void FWaveSpawner::Init() noexcept {
    _state              = EWaveState::Idle;
    _current_wave       = 0u;
    _wave_timer         = 0.0f;
    _intermission_timer = 0.0f;
    _alive_count        = 0u;
    _paused             = false;

    // 各 wave の spawned_per_rule を全部 0 にリセット (queue 自身は保持)。
    const usize n = _waves.Size();
    for (usize i = 0; i < n; ++i) {
        TArray<u32>& s = _waves[i].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) {
            s[j] = 0u;
        }
    }
    // callback は保持 (Init は scene 再 enter 用)。
}

void FWaveSpawner::Reset() noexcept {
    Init();
}

void FWaveSpawner::ClearAll() noexcept {
    _state              = EWaveState::Idle;
    _current_wave       = 0u;
    _wave_timer         = 0.0f;
    _intermission_timer = 0.0f;
    _alive_count        = 0u;
    _paused             = false;
    _waves.Clear();
    _spawn_cb       = nullptr;
    _spawn_cb_user  = nullptr;
    _state_cb       = nullptr;
    _state_cb_user  = nullptr;
}

// ----------------------------------------------------------------------------
// wave 構築
// ----------------------------------------------------------------------------

void FWaveSpawner::AddWave(const FWaveDef& def) noexcept {
    FWaveEntry entry;
    entry.def = def;
    // spawned_per_rule を rule_count 個の 0 で初期化。
    // TArray は trivially constructible な u32 については MemSet 0 で初期化される
    // ので、Resize だけで OK (= 中身は全部 0)。
    entry.spawned_per_rule.Resize(static_cast<usize>(def.rule_count));
    // Resize で 0 埋めされた前提だが、defense-in-depth で明示的に 0 を入れる。
    for (u32 i = 0; i < def.rule_count; ++i) {
        entry.spawned_per_rule[static_cast<usize>(i)] = 0u;
    }
    // TArray<TArray<u32>> は move-only なので Move で挿入。
    _waves.PushBack(Move(entry));
}

u32 FWaveSpawner::TotalWaves() const noexcept {
    return static_cast<u32>(_waves.Size());
}

// ----------------------------------------------------------------------------
// state transition
// ----------------------------------------------------------------------------

void FWaveSpawner::TransitionTo(EWaveState next) noexcept {
    if (next == _state) return;  // no-op
    const EWaveState prev = _state;
    _state = next;

    // 遷移時の補助タイマ初期化:
    //   Spawning に入る   → wave_timer = 0、alive_count は AdvanceToNextWave で
    //                       0 に戻す (= 旧 wave の生存者は持ち越さない契約)
    //   WaitingClear     → 何もしない (wave_timer は加算継続)
    //   Cleared          → intermission_timer = 0
    //   AllComplete/Idle → タイマは保持 (caller が問い合わせる用)
    if (next == EWaveState::Spawning) {
        _wave_timer = 0.0f;
    } else if (next == EWaveState::Cleared) {
        _intermission_timer = 0.0f;
    }

    // callback: state 更新後に呼ぶ (= callback 内で CurrentState() を見て OK)。
    // wave_index は遷移時点の current wave index を渡す (AllComplete でも
    // _current_wave は最後の wave index のまま、ここでは TotalWaves() ではなく
    // 「最後に処理していた wave」の index を渡す方が caller の解釈が容易)。
    if (_state_cb != nullptr) {
        _state_cb(_state_cb_user, _current_wave, prev, next);
    }
}

// ----------------------------------------------------------------------------
// 駆動
// ----------------------------------------------------------------------------

void FWaveSpawner::StartWaves() noexcept {
    // 進行中の呼び出しは誤用なので警告 + no-op (= 意図しない state 巻き戻しを防ぐ)。
    if (_state == EWaveState::Spawning || _state == EWaveState::WaitingClear) {
        ACS_LOG_WARN("FWaveSpawner::StartWaves: already running (state=%u)",
                     static_cast<u32>(_state));
        return;
    }

    // 空 queue で開始 → 即 AllComplete (state callback が必要なら発火する)。
    if (_waves.IsEmpty()) {
        _current_wave = 0u;
        TransitionTo(EWaveState::AllComplete);
        return;
    }

    // 進行カウンタを完全リセットしてから 0 番目の wave を Spawning にする。
    _current_wave       = 0u;
    _wave_timer         = 0.0f;
    _intermission_timer = 0.0f;
    _alive_count        = 0u;
    _paused             = false;
    // 0 番 wave の spawned_per_rule を 0 に揃え直す (= 再 Start での再湧き対応)。
    {
        TArray<u32>& s = _waves[0].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) s[j] = 0u;
    }
    TransitionTo(EWaveState::Spawning);
}

void FWaveSpawner::StopWaves() noexcept {
    _paused = true;
}

void FWaveSpawner::ResumeWaves() noexcept {
    _paused = false;
}

void FWaveSpawner::NotifyEnemyKilled(const char* enemy_id) noexcept {
    (void)enemy_id;  // 識別子マッチングは行わない (上位責務)、ログ目的のみ予約。

    // 進行中の wave がない / 既に全滅済みなら no-op (過剰 kill 防御)。
    if (_state != EWaveState::Spawning && _state != EWaveState::WaitingClear) {
        return;
    }
    if (_alive_count == 0u) {
        // 既に 0 なら下回らせない (defense-in-depth)。
        return;
    }
    --_alive_count;

    // WaitingClear かつ alive==0 になった瞬間に Cleared へ遷移。
    // Spawning 中 + alive==0 はまだ全 rule が発火しきっていない可能性があり、
    // ここでは Cleared にせず Spawning 継続する (= TickSpawning が rule 完走 →
    // WaitingClear → 即 Cleared を保証する)。
    if (_state == EWaveState::WaitingClear && _alive_count == 0u) {
        TransitionTo(EWaveState::Cleared);
    }
}

// ----------------------------------------------------------------------------
// internal: per-wave spawn driver
// ----------------------------------------------------------------------------

void FWaveSpawner::TickSpawning(f32 dt) noexcept {
    if (_current_wave >= _waves.Size()) return;  // defense
    FWaveEntry&     entry = _waves[_current_wave];
    const FWaveDef& def   = entry.def;

    _wave_timer += dt;

    // 空 wave (rule_count == 0): 即 WaitingClear へ。alive_count は 0 のまま。
    if (def.rule_count == 0u || def.rules == nullptr) {
        TransitionTo(EWaveState::WaitingClear);
        return;
    }

    // 各 rule を独立に評価し、発火条件を満たすたびに spawn callback を呼ぶ。
    // 同 Tick 内で複数発火する場合は while ループで carry する。
    u32 total_completed_rules = 0u;
    for (u32 i = 0; i < def.rule_count; ++i) {
        const FSpawnRule& rule = def.rules[i];
        u32& spawned = entry.spawned_per_rule[static_cast<usize>(i)];

        // 既に rule の count を満たした → 完了 rule としてマークだけして次へ。
        if (spawned >= rule.count) {
            ++total_completed_rules;
            continue;
        }

        // count == 0 の rule は事実上完了扱い (誤用 / no-op rule)。
        if (rule.count == 0u || rule.enemy_id == nullptr) {
            spawned = rule.count;  // count==0 のときは 0 のまま
            ++total_completed_rules;
            continue;
        }

        // 発火タイミング: spawn_time(n) = initial_delay + n * interval (n は 0-based)。
        // interval <= 0 のときは「initial_delay 後にまとめて count 個」扱いとする
        // (= 連続発火、_wave_timer >= initial_delay を満たした瞬間に残数を全部
        // 同フレームで発火させる)。
        if (rule.spawn_interval_sec <= 0.0f) {
            if (_wave_timer >= rule.initial_delay_sec) {
                while (spawned < rule.count) {
                    if (_spawn_cb != nullptr) {
                        _spawn_cb(_spawn_cb_user, rule.enemy_id, rule.spawn_position);
                    }
                    ++spawned;
                    ++_alive_count;
                }
            }
        } else {
            // 正常な周期 spawn: 大 dt 対策で while で carry。
            // 次に発火すべき時刻 = initial_delay + spawned * interval。
            while (spawned < rule.count) {
                const f32 next_fire_time = rule.initial_delay_sec
                                         + static_cast<f32>(spawned) * rule.spawn_interval_sec;
                if (_wave_timer < next_fire_time) break;
                if (_spawn_cb != nullptr) {
                    _spawn_cb(_spawn_cb_user, rule.enemy_id, rule.spawn_position);
                }
                ++spawned;
                ++_alive_count;
            }
        }

        if (spawned >= rule.count) ++total_completed_rules;
    }

    // 全 rule が count を満たしたら WaitingClear へ。
    // alive_count が既に 0 なら (= spawn と同 Tick 内で全部キル通知が来た等の
    // 病的ケース or count==0 の wave) WaitingClear を経由して即 Cleared へ
    // 遷移する。これは Tick() のメインループ側で再判定するので、ここでは
    // WaitingClear に上げるだけ。
    if (total_completed_rules >= def.rule_count) {
        TransitionTo(EWaveState::WaitingClear);
    }
}

// ----------------------------------------------------------------------------
// internal: advance to next wave (or AllComplete)
// ----------------------------------------------------------------------------

void FWaveSpawner::AdvanceToNextWave() noexcept {
    const u32 next_index = _current_wave + 1u;
    if (next_index >= _waves.Size()) {
        // 最後の wave を終えた → AllComplete。_current_wave はそのまま (最後の
        // 有効 wave index を保持) で callback に渡す。
        TransitionTo(EWaveState::AllComplete);
        return;
    }

    _current_wave = next_index;
    _alive_count  = 0u;
    // 次 wave の spawned_per_rule を 0 に揃え直す (= 再起動時の再湧きにも対応)。
    {
        TArray<u32>& s = _waves[_current_wave].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) s[j] = 0u;
    }
    TransitionTo(EWaveState::Spawning);
}

// ----------------------------------------------------------------------------
// callback
// ----------------------------------------------------------------------------

void FWaveSpawner::SetOnSpawnCallback(SpawnCallback cb, void* user) noexcept {
    _spawn_cb      = cb;
    _spawn_cb_user = user;
}

void FWaveSpawner::SetOnWaveStateChangeCallback(WaveStateChangeCallback cb, void* user) noexcept {
    _state_cb      = cb;
    _state_cb_user = user;
}

// ----------------------------------------------------------------------------
// query helpers
// ----------------------------------------------------------------------------

u32 FWaveSpawner::EnemiesSpawnedInWave() const noexcept {
    if (_state == EWaveState::Idle || _state == EWaveState::AllComplete) return 0u;
    if (_current_wave >= _waves.Size()) return 0u;
    const TArray<u32>& s = _waves[_current_wave].spawned_per_rule;
    u32 total = 0u;
    const usize m = s.Size();
    for (usize i = 0; i < m; ++i) total += s[i];
    return total;
}

// ----------------------------------------------------------------------------
// driver (top-level)
// ----------------------------------------------------------------------------

void FWaveSpawner::Tick(f32 dt) noexcept {
    if (_paused) return;
    if (dt < 0.0f) dt = 0.0f;

    // state 連鎖を許容するためのループ。1 Tick 内に
    //   Spawning → WaitingClear → Cleared → (intermission 終わり) → Spawning
    // と複数遷移し得る (= 大 dt or 短い intermission / 短い wave)。安全弁として
    // 最大ループ回数を 32 にキャップ (= 同 dt で 32 wave 跨ぐのは病的ケースのみ)。
    for (u32 iter = 0; iter < 32u; ++iter) {
        const EWaveState prev_state = _state;

        switch (_state) {
            case EWaveState::Idle:
            case EWaveState::AllComplete:
                return;  // 何もしない

            case EWaveState::Spawning:
                TickSpawning(dt);
                // TickSpawning 内で WaitingClear に遷移し得る。さらに alive==0 なら
                // この iter 末で WaitingClear → Cleared に上げる (下の処理に流す)。
                if (_state == EWaveState::WaitingClear && _alive_count == 0u) {
                    TransitionTo(EWaveState::Cleared);
                }
                break;

            case EWaveState::WaitingClear:
                // ここでは spawn は終わっているが alive>0 のはず。何もしないで
                // NotifyEnemyKilled を待つ。dt はカウンタに加算しておく (= 演出/
                // 統計用、HUD に wave 経過秒として出せる)。
                _wave_timer += dt;
                // 防衛的に alive==0 を再チェック (= NotifyEnemyKilled が外から
                // 来て前 iter で alive を減らしていた場合に対応)。
                if (_alive_count == 0u) {
                    TransitionTo(EWaveState::Cleared);
                }
                break;

            case EWaveState::Cleared: {
                // intermission を進める。閾値到達で次 wave へ。
                // _wave_timer も加算継続する (= 「wave 開始からの経過秒」契約)。
                _wave_timer         += dt;
                _intermission_timer += dt;
                // 現 wave の intermission 設定を引く。
                f32 threshold = 0.0f;
                if (_current_wave < _waves.Size()) {
                    threshold = _waves[_current_wave].def.wave_intermission_sec;
                    if (threshold < 0.0f) threshold = 0.0f;
                }
                if (_intermission_timer >= threshold) {
                    AdvanceToNextWave();
                }
                break;
            }
        }

        // 状態が変化していなければループ終了 (= 進捗なし)。
        if (_state == prev_state) break;
    }
}

} // namespace acs::game
