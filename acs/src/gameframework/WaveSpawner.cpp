// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar L/R — CWaveSpawner 実装
//
// state machine + 複数 rule 並列 spawn + intermission timer の合成。
// FSpawnRule.spawn_interval_sec が小さい / dt が大きいケースでは 1 Tick 内で
// 複数 spawn が連鎖発火する (= carry 方式)。state 遷移も連鎖し得るので、
// Tick() は state ループとして書く。
#include "gameframework/WaveSpawner.h"
#include "foundation/Log.h"
#include "foundation/Move.h"

namespace acs::game {

/** wave 配列を事前確保して構築する。 */
CWaveSpawner::CWaveSpawner() noexcept {
    m_Waves.Reserve(kWaveReserveHint);
}

/** 進行状態をリセットし各 wave の spawned_per_rule を 0 に戻す (callback は保持)。 */
void CWaveSpawner::Init() noexcept {
    _state              = EWaveState::Idle;
    m_CurrentWave       = 0u;
    m_WaveTimer         = 0.0f;
    m_IntermissionTimer = 0.0f;
    m_AliveCount        = 0u;
    m_Paused             = false;

    // 各 wave の spawned_per_rule を全部 0 にリセット (queue 自身は保持)。
    const usize n = m_Waves.Size();
    for (usize i = 0; i < n; ++i) {
        TArray<u32>& s = m_Waves[i].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) {
            s[j] = 0u;
        }
    }
    // callback は保持 (Init は scene 再 enter 用)。
}

/** 進行状態を Init() と同じくリセットする。 */
void CWaveSpawner::Reset() noexcept {
    Init();
}

/** 全 wave と callback を含めて完全に空状態へ戻す。 */
void CWaveSpawner::ClearAll() noexcept {
    _state              = EWaveState::Idle;
    m_CurrentWave       = 0u;
    m_WaveTimer         = 0.0f;
    m_IntermissionTimer = 0.0f;
    m_AliveCount        = 0u;
    m_Paused             = false;
    m_Waves.Clear();
    m_SpawnCb       = nullptr;
    m_SpawnCbUser  = nullptr;
    _state_cb       = nullptr;
    _state_cb_user  = nullptr;
}

/** def をコピーして wave を 1 つ末尾に追加し spawned_per_rule を 0 初期化する。 */
void CWaveSpawner::AddWave(const FWaveDef& def) noexcept {
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
    m_Waves.PushBack(Move(entry));
}

/** 登録済み wave の総数を返す。 */
u32 CWaveSpawner::TotalWaves() const noexcept {
    return static_cast<u32>(m_Waves.Size());
}

/** state を遷移させ、補助タイマを初期化して state 変更 callback を発火する。 */
void CWaveSpawner::TransitionTo(EWaveState next) noexcept {
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
        m_WaveTimer = 0.0f;
    } else if (next == EWaveState::Cleared) {
        m_IntermissionTimer = 0.0f;
    }

    // callback: state 更新後に呼ぶ (= callback 内で CurrentState() を見て OK)。
    // wave_index は遷移時点の current wave index を渡す (AllComplete でも
    // m_CurrentWave は最後の wave index のまま、ここでは TotalWaves() ではなく
    // 「最後に処理していた wave」の index を渡す方が caller の解釈が容易)。
    if (_state_cb != nullptr) {
        _state_cb(_state_cb_user, m_CurrentWave, prev, next);
    }
}

/** 進行カウンタをリセットし 0 番 wave を Spawning に入れる (空 queue は即 AllComplete)。 */
void CWaveSpawner::StartWaves() noexcept {
    // 進行中の呼び出しは誤用なので警告 + no-op (= 意図しない state 巻き戻しを防ぐ)。
    if (_state == EWaveState::Spawning || _state == EWaveState::WaitingClear) {
        ACS_LOG_WARN("FWaveSpawner::StartWaves: already running (state=%u)",
                     static_cast<u32>(_state));
        return;
    }

    // 空 queue で開始 → 即 AllComplete (state callback が必要なら発火する)。
    if (m_Waves.IsEmpty()) {
        m_CurrentWave = 0u;
        TransitionTo(EWaveState::AllComplete);
        return;
    }

    // 進行カウンタを完全リセットしてから 0 番目の wave を Spawning にする。
    m_CurrentWave       = 0u;
    m_WaveTimer         = 0.0f;
    m_IntermissionTimer = 0.0f;
    m_AliveCount        = 0u;
    m_Paused             = false;
    // 0 番 wave の spawned_per_rule を 0 に揃え直す (= 再 Start での再湧き対応)。
    {
        TArray<u32>& s = m_Waves[0].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) s[j] = 0u;
    }
    TransitionTo(EWaveState::Spawning);
}

/** Tick の進行を一時停止する (pause フラグを立てる)。 */
void CWaveSpawner::StopWaves() noexcept {
    m_Paused = true;
}

/** 一時停止を解除して Tick の進行を再開する。 */
void CWaveSpawner::ResumeWaves() noexcept {
    m_Paused = false;
}

/** 敵 1 体の撃破を反映して alive_count を減らし、全滅時に Cleared へ遷移する。 */
void CWaveSpawner::NotifyEnemyKilled(const char* enemy_id) noexcept {
    (void)enemy_id;  // 識別子マッチングは行わない (上位責務)、ログ目的のみ予約。

    // 進行中の wave がない / 既に全滅済みなら no-op (過剰 kill 防御)。
    if (_state != EWaveState::Spawning && _state != EWaveState::WaitingClear) {
        return;
    }
    if (m_AliveCount == 0u) {
        // 既に 0 なら下回らせない (defense-in-depth)。
        return;
    }
    --m_AliveCount;

    // WaitingClear かつ alive==0 になった瞬間に Cleared へ遷移。
    // Spawning 中 + alive==0 はまだ全 rule が発火しきっていない可能性があり、
    // ここでは Cleared にせず Spawning 継続する (= TickSpawning が rule 完走 →
    // WaitingClear → 即 Cleared を保証する)。
    if (_state == EWaveState::WaitingClear && m_AliveCount == 0u) {
        TransitionTo(EWaveState::Cleared);
    }
}

/** 現 wave の各 rule を評価して spawn callback を発火し、完走で WaitingClear へ遷移する。 */
void CWaveSpawner::TickSpawning(f32 dt) noexcept {
    if (m_CurrentWave >= m_Waves.Size()) return;  // defense
    FWaveEntry&     entry = m_Waves[m_CurrentWave];
    const FWaveDef& def   = entry.def;

    m_WaveTimer += dt;

    // 空 wave (rule_count == 0): 即 WaitingClear へ。alive_count は 0 のまま。
    if (def.rule_count == 0u || def.rules == nullptr) {
        TransitionTo(EWaveState::WaitingClear);
        return;
    }

    // 各 rule を独立に評価し、発火条件を満たした spawn を m_PendingSpawns へ積む。
    // 同 Tick 内で複数発火する場合は while ループで carry する。
    //
    // spawn callback はここでは呼ばない: callback (ユーザーコード) が再入で
    // AddWave (m_Waves 再確保) / Init / Clear を行うと entry / rule / spawned の
    // 参照が dangling になるため、走査を終えて参照を手放してからまとめて発火する
    // (BuffSystem::Tick と同じ「配列操作を済ませてから発火」規約)。
    m_PendingSpawns.Clear();
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
        // (= 連続発火、m_WaveTimer >= initial_delay を満たした瞬間に残数を全部
        // 同フレームで発火させる)。
        if (rule.spawn_interval_sec <= 0.0f) {
            if (m_WaveTimer >= rule.initial_delay_sec) {
                while (spawned < rule.count) {
                    m_PendingSpawns.PushBack({rule.enemy_id, rule.spawn_position});
                    ++spawned;
                    ++m_AliveCount;
                }
            }
        } else {
            // 正常な周期 spawn: 大 dt 対策で while で carry。
            // 次に発火すべき時刻 = initial_delay + spawned * interval。
            while (spawned < rule.count) {
                const f32 next_fire_time = rule.initial_delay_sec
                                         + static_cast<f32>(spawned) * rule.spawn_interval_sec;
                if (m_WaveTimer < next_fire_time) break;
                m_PendingSpawns.PushBack({rule.enemy_id, rule.spawn_position});
                ++spawned;
                ++m_AliveCount;
            }
        }

        if (spawned >= rule.count) ++total_completed_rules;
    }

    // 参照を手放した後にまとめて発火する。
    const bool all_rules_completed = (total_completed_rules >= def.rule_count);
    for (usize p = 0; p < m_PendingSpawns.Size(); ++p) {
        if (m_SpawnCb == nullptr) break;
        m_SpawnCb(m_SpawnCbUser, m_PendingSpawns[p].enemy_id, m_PendingSpawns[p].position);
    }
    m_PendingSpawns.Clear();

    // callback が再入で Stop / Init / Clear 等を行い state が Spawning でなくなった
    // 場合は、この wave の遷移判定を続けない (外側の意思を尊重する)。
    if (_state != EWaveState::Spawning) return;

    // 全 rule が count を満たしたら WaitingClear へ。
    // alive_count が既に 0 なら (= spawn と同 Tick 内で全部キル通知が来た等の
    // 病的ケース or count==0 の wave) WaitingClear を経由して即 Cleared へ
    // 遷移する。これは Tick() のメインループ側で再判定するので、ここでは
    // WaitingClear に上げるだけ。
    if (all_rules_completed) {
        TransitionTo(EWaveState::WaitingClear);
    }
}

/** 次 wave へ進めて Spawning に入れる (最後の wave なら AllComplete へ遷移)。 */
void CWaveSpawner::AdvanceToNextWave() noexcept {
    const u32 next_index = m_CurrentWave + 1u;
    if (next_index >= m_Waves.Size()) {
        // 最後の wave を終えた → AllComplete。m_CurrentWave はそのまま (最後の
        // 有効 wave index を保持) で callback に渡す。
        TransitionTo(EWaveState::AllComplete);
        return;
    }

    m_CurrentWave = next_index;
    m_AliveCount  = 0u;
    // 次 wave の spawned_per_rule を 0 に揃え直す (= 再起動時の再湧きにも対応)。
    {
        TArray<u32>& s = m_Waves[m_CurrentWave].spawned_per_rule;
        const usize m = s.Size();
        for (usize j = 0; j < m; ++j) s[j] = 0u;
    }
    TransitionTo(EWaveState::Spawning);
}

/** 敵 spawn 時に呼ばれる callback と user pointer を設定する。 */
void CWaveSpawner::SetOnSpawnCallback(SpawnCallback cb, void* user) noexcept {
    m_SpawnCb      = cb;
    m_SpawnCbUser = user;
}

/** state 変更時に呼ばれる callback と user pointer を設定する。 */
void CWaveSpawner::SetOnWaveStateChangeCallback(WaveStateChangeCallback cb, void* user) noexcept {
    _state_cb      = cb;
    _state_cb_user = user;
}

/** 現 wave で各 rule が spawn した数の合計を返す (Idle/AllComplete は 0)。 */
u32 CWaveSpawner::EnemiesSpawnedInWave() const noexcept {
    if (_state == EWaveState::Idle || _state == EWaveState::AllComplete) return 0u;
    if (m_CurrentWave >= m_Waves.Size()) return 0u;
    const TArray<u32>& s = m_Waves[m_CurrentWave].spawned_per_rule;
    u32 total = 0u;
    const usize m = s.Size();
    for (usize i = 0; i < m; ++i) total += s[i];
    return total;
}

/** state machine を 1 フレーム駆動する (1 Tick 内の state 連鎖を 32 回まで許容)。 */
void CWaveSpawner::Tick(f32 dt) noexcept {
    if (m_Paused) return;
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
                if (_state == EWaveState::WaitingClear && m_AliveCount == 0u) {
                    TransitionTo(EWaveState::Cleared);
                }
                break;

            case EWaveState::WaitingClear:
                // ここでは spawn は終わっているが alive>0 のはず。何もしないで
                // NotifyEnemyKilled を待つ。dt はカウンタに加算しておく (= 演出/
                // 統計用、HUD に wave 経過秒として出せる)。
                m_WaveTimer += dt;
                // 防衛的に alive==0 を再チェック (= NotifyEnemyKilled が外から
                // 来て前 iter で alive を減らしていた場合に対応)。
                if (m_AliveCount == 0u) {
                    TransitionTo(EWaveState::Cleared);
                }
                break;

            case EWaveState::Cleared: {
                // intermission を進める。閾値到達で次 wave へ。
                // m_WaveTimer も加算継続する (= 「wave 開始からの経過秒」契約)。
                m_WaveTimer         += dt;
                m_IntermissionTimer += dt;
                // 現 wave の intermission 設定を引く。
                f32 threshold = 0.0f;
                if (m_CurrentWave < m_Waves.Size()) {
                    threshold = m_Waves[m_CurrentWave].def.wave_intermission_sec;
                    if (threshold < 0.0f) threshold = 0.0f;
                }
                if (m_IntermissionTimer >= threshold) {
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
