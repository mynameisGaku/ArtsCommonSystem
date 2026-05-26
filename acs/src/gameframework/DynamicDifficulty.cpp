// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar U Phase 2 — DynamicDifficulty 実装
//
// 仕様意図は DynamicDifficulty.h を参照。本ファイルは「離散モード時は乗数を
// 即返し / Adaptive 時は skill 推定 + smooth lerp」の純粋 state machine。
#include "gameframework/DynamicDifficulty.h"

namespace acs::game {

// =============================================================================
// 内部定数 / テーブル
// -----------------------------------------------------------------------------
// 4 段乗数テーブル (Easy / Normal / Hard / VeryHard):
//   ・敵 HP : 0.5 / 1.0 / 1.5 / 2.0  (要件指示)
//   ・敵ダメ: 0.6 / 1.0 / 1.4 / 1.8
//   ・敵速度: 0.85 / 1.0 / 1.15 / 1.3 (速度は感触が壊れるので保守的)
//   ・PHP   : 1.5 / 1.0 / 0.85 / 0.75 (逆相関、Easy で player 多 HP)
//
// Adaptive 用 skill 推定の重み:
//   kill / clear 速度はプラス寄与、retry / death はマイナス寄与。
//   各統計を「セッション秒で正規化」してから合算 → saturate で [0,1]。
// =============================================================================
namespace {

constexpr f32 kEnemyHpTable [4] = { 0.5f,  1.0f,  1.5f,  2.0f  };
constexpr f32 kEnemyDmgTable[4] = { 0.6f,  1.0f,  1.4f,  1.8f  };
constexpr f32 kEnemySpdTable[4] = { 0.85f, 1.0f,  1.15f, 1.3f  };
constexpr f32 kPlayerHpTable[4] = { 1.5f,  1.0f,  0.85f, 0.75f };

// Skill 推定の重み (合計 ~1.0 程度になるよう調整)。
// kill: 1 kill/min がベース、それを上回ると skill++ 寄与
// death: 1 death/min がベース、それを上回ると skill-- 寄与
// retry: 1 retry でも skill-- に少し寄与 (current_level の難航判定)
// clear: avg_completion < 60s で skill++、それ以上で減衰
constexpr f32 kKillRatePerMinute    = 1.0f;   // baseline 1/min
constexpr f32 kDeathRatePerMinute   = 1.0f;
constexpr f32 kWeightKillRate       = 0.35f;
constexpr f32 kWeightClearSpeed     = 0.25f;
constexpr f32 kWeightRetryPenalty   = 0.10f;  // 1 retry につき
constexpr f32 kWeightDeathRate      = 0.40f;

// average_completion_time の評価用基準時間。これ以下なら skill ↑、上なら ↓。
constexpr f32 kClearTimeBaselineSec = 60.0f;

// セッション時間下限。1 秒未満では density 推定の分母として使わない (発散回避)。
constexpr f32 kMinSessionTimeSec    = 1.0f;

// 連続値 → 区間インデックス境界 (Easy=0 / Normal=1/3 / Hard=2/3 / VeryHard=1)。
constexpr f32 kSegThird             = 1.0f / 3.0f;
constexpr f32 kSegTwoThirds         = 2.0f / 3.0f;

} // namespace

// =============================================================================
// helper: 離散モード → 連続値 [0,1]
// -----------------------------------------------------------------------------
// Adaptive はここに来ない (= Adaptive 切替時の current 維持は呼出側で処理済み)。
// 念のためフォールバックとして Normal の連続値を返す。
// =============================================================================
f32 DynamicDifficulty::ContinuousFromDiscrete(EDifficultyLevel m) noexcept {
    switch (m) {
        case EDifficultyLevel::Easy:     return 0.0f;
        case EDifficultyLevel::Normal:   return kSegThird;
        case EDifficultyLevel::Hard:     return kSegTwoThirds;
        case EDifficultyLevel::VeryHard: return 1.0f;
        case EDifficultyLevel::Adaptive: return kSegThird;  // fallback
    }
    return kSegThird;
}

// =============================================================================
// helper: 連続値 [0,1] を 4 段表で線形補間
// -----------------------------------------------------------------------------
// 区間 [0, 1/3] → vals[0]..vals[1]
// 区間 [1/3, 2/3] → vals[1]..vals[2]
// 区間 [2/3, 1.0] → vals[2]..vals[3]
// 範囲外は両端の値で頭打ち (defensive)。
// =============================================================================
f32 DynamicDifficulty::SampleCurve(f32 t, const f32 vals[4]) noexcept {
    if (t <= 0.0f)         return vals[0];
    if (t >= 1.0f)         return vals[3];
    if (t < kSegThird) {
        const f32 local = t / kSegThird;
        return Lerp(vals[0], vals[1], local);
    }
    if (t < kSegTwoThirds) {
        const f32 local = (t - kSegThird) / kSegThird;
        return Lerp(vals[1], vals[2], local);
    }
    const f32 local = (t - kSegTwoThirds) / kSegThird;
    return Lerp(vals[2], vals[3], local);
}

// =============================================================================
// Init / SetMode
// -----------------------------------------------------------------------------
// Init は state を初期化して base_level に切替。Adaptive 指定時は 0.5 から
// スタート (= 中庸)、それ以外は離散値スナップ。
// SetMode は Adaptive → 離散の切替で「離散値に即スナップ」、その他遷移は
// current を保持。これにより「Adaptive 中だが UI から Normal を強制」みたいな
// 用例でプレイヤーが期待する離散値が即反映される。
// =============================================================================
void DynamicDifficulty::Init(EDifficultyLevel base_level) noexcept {
    _stats        = PlayerSkillStats{};
    _session_time = 0.0f;
    _mode         = base_level;
    if (base_level == EDifficultyLevel::Adaptive) {
        _current_difficulty = 0.5f;  // 中庸スタート、Tick で target へ寄せる
    } else {
        _current_difficulty = ContinuousFromDiscrete(base_level);
    }
}

void DynamicDifficulty::SetMode(EDifficultyLevel mode) noexcept {
    _mode = mode;
    if (mode != EDifficultyLevel::Adaptive) {
        // 離散モードへの切替は値を即スナップ (UI 反映が遅れない方が直感的)
        _current_difficulty = ContinuousFromDiscrete(mode);
    }
    // Adaptive への切替は current を保持 (Tick で target に向け smooth lerp 継続)
}

// =============================================================================
// 統計記録
// -----------------------------------------------------------------------------
// すべて加算のみ (overflow は u32 max クランプ)。RecordLevelComplete は
// average_completion_time を EMA で更新し、retries_current_level を 0 リセット。
// =============================================================================
void DynamicDifficulty::RecordDeath() noexcept {
    if (_stats.deaths_last_session < 0xFFFFFFFFu) {
        ++_stats.deaths_last_session;
    }
}

void DynamicDifficulty::RecordKill() noexcept {
    if (_stats.kills_last_session < 0xFFFFFFFFu) {
        ++_stats.kills_last_session;
    }
}

void DynamicDifficulty::RecordLevelComplete(f32 time_taken) noexcept {
    if (time_taken < 0.0f) time_taken = 0.0f;
    if (_stats.average_completion_time <= 0.0f) {
        // 初回計測: そのまま採用 (EMA の初期値問題回避)
        _stats.average_completion_time = time_taken;
    } else {
        // EMA: new = α * sample + (1-α) * old (α = 0.3)
        _stats.average_completion_time =
            kCompletionTimeEmaAlpha * time_taken
          + (1.0f - kCompletionTimeEmaAlpha) * _stats.average_completion_time;
    }
    _stats.retries_current_level = 0;  // クリアできたのでリトライ累計はリセット
}

void DynamicDifficulty::RecordRetry() noexcept {
    if (_stats.retries_current_level < 0xFFFFFFFFu) {
        ++_stats.retries_current_level;
    }
}

void DynamicDifficulty::RecordPowerupCollected() noexcept {
    if (_stats.powerups_collected < 0xFFFFFFFFu) {
        ++_stats.powerups_collected;
    }
}

// =============================================================================
// ResetStats
// -----------------------------------------------------------------------------
// 統計のみ初期化。モード / current_difficulty / session_time は維持。
// NewGame や章切替時に呼ぶ想定 (= 進行は残すがスタッツは仕切り直し)。
// =============================================================================
void DynamicDifficulty::ResetStats() noexcept {
    _stats        = PlayerSkillStats{};
    _session_time = 0.0f;
}

// =============================================================================
// 乗数 accessor
// -----------------------------------------------------------------------------
// 離散モード: 該当段の値を即返却。Adaptive: 連続値で 4 段表を補間。
// 離散モードでも SampleCurve を経由しても良いが (current は離散値スナップ済み)、
// テーブル直接参照のほうが意図が読めて分岐コストも誤差レベル。
// =============================================================================
f32 DynamicDifficulty::EnemyHealthMultiplier() const noexcept {
    if (_mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(_current_difficulty, kEnemyHpTable);
    }
    return kEnemyHpTable[static_cast<u32>(_mode)];
}

f32 DynamicDifficulty::EnemyDamageMultiplier() const noexcept {
    if (_mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(_current_difficulty, kEnemyDmgTable);
    }
    return kEnemyDmgTable[static_cast<u32>(_mode)];
}

f32 DynamicDifficulty::EnemySpeedMultiplier() const noexcept {
    if (_mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(_current_difficulty, kEnemySpdTable);
    }
    return kEnemySpdTable[static_cast<u32>(_mode)];
}

f32 DynamicDifficulty::PlayerHealthMultiplier() const noexcept {
    if (_mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(_current_difficulty, kPlayerHpTable);
    }
    return kPlayerHpTable[static_cast<u32>(_mode)];
}

// =============================================================================
// ComputeAdaptiveTarget
// -----------------------------------------------------------------------------
// skill 推定を [0,1] で求め、target = 1 - skill を返す。
// skill 推定式:
//   minutes = max(session_time / 60, ε)
//   kill_rate  = kills / minutes / kKillRatePerMinute     ... 1.0 で baseline
//   death_rate = deaths / minutes / kDeathRatePerMinute
//   clear_score: avg_time が小さいほど高い (baseline 60s で 1.0)
//                avg_time == 0 (未計測) なら 0 寄与
//   retry_pen  = retries_current_level * kWeightRetryPenalty
//
//   skill = kill_rate * w_k + clear_score * w_c
//         - death_rate * w_d - retry_pen
//   skill = saturate(skill * 0.5 + 0.5)  ... [-1,1] 範囲を [0,1] にマップ
//
// 各統計の重みは合計でほぼ ±1 程度のスケールに収まるよう調整。
// =============================================================================
f32 DynamicDifficulty::ComputeAdaptiveTarget() const noexcept {
    // session_time 下限ガード。プレイ開始直後の DDA 暴れを防ぐ。
    const f32 session_sec = _session_time < kMinSessionTimeSec
                          ? kMinSessionTimeSec : _session_time;
    const f32 minutes     = session_sec / 60.0f;

    // kill / death を「分あたり」に正規化して baseline と比較
    const f32 kill_per_min  = static_cast<f32>(_stats.kills_last_session)  / minutes;
    const f32 death_per_min = static_cast<f32>(_stats.deaths_last_session) / minutes;

    const f32 kill_score   = kill_per_min  / kKillRatePerMinute;   // 1.0 = baseline
    const f32 death_score  = death_per_min / kDeathRatePerMinute;

    // clear_score: avg_time が baseline 以下なら 1.0+α、以上なら減衰 (0 で 0)
    f32 clear_score = 0.0f;
    if (_stats.average_completion_time > 0.0f) {
        // baseline / avg で「速いほど高スコア」。avg=60s → 1.0、avg=30s → 2.0、
        // avg=120s → 0.5。極端値で振り回されないよう [0, 2] にクランプ。
        clear_score = kClearTimeBaselineSec / _stats.average_completion_time;
        if (clear_score > 2.0f) clear_score = 2.0f;
        if (clear_score < 0.0f) clear_score = 0.0f;
    }

    // retry penalty: 1 retry につき固定減点
    const f32 retry_pen = static_cast<f32>(_stats.retries_current_level)
                        * kWeightRetryPenalty;

    // 合成 skill (符号付き、おおむね [-1, +1] 範囲想定)
    const f32 skill_signed = kill_score  * kWeightKillRate
                           + clear_score * kWeightClearSpeed
                           - death_score * kWeightDeathRate
                           - retry_pen;

    // [-1, 1] → [0, 1] にマップして saturate
    const f32 skill = Saturate(skill_signed * 0.5f + 0.5f);

    // target = 1 - skill (上手いプレイヤーには高難易度を)
    return 1.0f - skill;
}

// =============================================================================
// Tick
// -----------------------------------------------------------------------------
// 離散モード時:
//   ・session_time だけ加算 (Adaptive へ切替えた瞬間でも統計分母として効く)
//   ・current_difficulty はスナップ済みなので何もしない
// Adaptive 時:
//   ・session_time += dt
//   ・target = ComputeAdaptiveTarget()
//   ・current_difficulty を framerate-independent exponential lerp で target へ寄せる
//
// dt が負 (clock 巻き戻し) なら 0 扱い (state を破壊しない)。
// =============================================================================
void DynamicDifficulty::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    _session_time += dt;

    if (_mode != EDifficultyLevel::Adaptive) {
        // 離散モードは current が固定 (SetMode で snap 済み) なので何もしない
        return;
    }

    const f32 target = ComputeAdaptiveTarget();
    // framerate-independent exponential smoothing (Camera2D と同じ式)
    // t = 1 - exp(-rate * dt) で dt 不変な指数追従。rate = 0.5 で約 1.4 秒で 50%。
    const f32 t = 1.0f - Exp(-kAdaptiveLerpRate * dt);
    _current_difficulty += (target - _current_difficulty) * t;

    // 数値誤差で [0,1] を僅かに外れる可能性に備えて saturate
    _current_difficulty = Saturate(_current_difficulty);
}

} // namespace acs::game
