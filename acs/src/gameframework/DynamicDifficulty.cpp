// SPDX-License-Identifier: Apache-2.0
// 離散難易度とplayer統計によるadaptive値から、敵とplayerの能力値乗数を決める。
#include "gameframework/DynamicDifficulty.h"

namespace acs::game {

namespace {

/** 敵 HP 乗数の 4 段テーブル (Easy / Normal / Hard / VeryHard)。 */
constexpr f32 kEnemyHpTable [4] = { 0.5f,  1.0f,  1.5f,  2.0f  };

/** 敵ダメージ乗数の 4 段テーブル (Easy / Normal / Hard / VeryHard)。 */
constexpr f32 kEnemyDmgTable[4] = { 0.6f,  1.0f,  1.4f,  1.8f  };

/** 敵速度乗数の 4 段テーブル (感触が壊れるので保守的な値)。 */
constexpr f32 kEnemySpdTable[4] = { 0.85f, 1.0f,  1.15f, 1.3f  };

/** プレイヤー HP 倍率の 4 段テーブル (難易度と逆相関、Easy で多 HP)。 */
constexpr f32 kPlayerHpTable[4] = { 1.5f,  1.0f,  0.85f, 0.75f };

/** kill レートの baseline (1 kill/min を 1.0 とする正規化基準)。 */
constexpr f32 kKillRatePerMinute    = 1.0f;

/** death レートの baseline (1 death/min を 1.0 とする正規化基準)。 */
constexpr f32 kDeathRatePerMinute   = 1.0f;

/** skill 推定での kill レートの重み (プラス寄与)。 */
constexpr f32 kWeightKillRate       = 0.35f;

/** skill 推定でのクリア速度スコアの重み (プラス寄与)。 */
constexpr f32 kWeightClearSpeed     = 0.25f;

/** skill 推定でのリトライ 1 回あたりの減点重み。 */
constexpr f32 kWeightRetryPenalty   = 0.10f;

/** skill 推定での death レートの重み (マイナス寄与)。 */
constexpr f32 kWeightDeathRate      = 0.40f;

/** クリア速度スコアの評価基準時間 (秒)。これ以下なら skill↑、上なら↓。 */
constexpr f32 kClearTimeBaselineSec = 60.0f;

/** セッション時間下限 (秒)。これ未満では density 推定の分母に使わない (発散回避)。 */
constexpr f32 kMinSessionTimeSec    = 1.0f;

/** 連続値の区間境界 1/3 (Easy↔Normal)。 */
constexpr f32 kSegThird             = 1.0f / 3.0f;

/** 連続値の区間境界 2/3 (Normal↔Hard)。 */
constexpr f32 kSegTwoThirds         = 2.0f / 3.0f;

} // namespace

/** 離散モードを連続値 [0,1] に変換する (Adaptive は fallback で Normal の連続値)。 */
f32 CDynamicDifficulty::ContinuousFromDiscrete(EDifficultyLevel m) noexcept {
    switch (m) {
        case EDifficultyLevel::Easy:     return 0.0f;
        case EDifficultyLevel::Normal:   return kSegThird;
        case EDifficultyLevel::Hard:     return kSegTwoThirds;
        case EDifficultyLevel::VeryHard: return 1.0f;
        case EDifficultyLevel::Adaptive: return kSegThird;  // fallback
    }
    return kSegThird;
}

/** 連続値 [0,1] を 4 段表で区間線形補間する (範囲外は両端値で頭打ち)。 */
f32 CDynamicDifficulty::SampleCurve(f32 t, const f32 vals[4]) noexcept {
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

/** 統計を初期化して base_level に切り替える (Adaptive は 0.5 中庸スタート)。 */
void CDynamicDifficulty::Init(EDifficultyLevel base_level) noexcept {
    _stats        = FPlayerSkillStats{};
    m_SessionTime = 0.0f;
    m_Mode         = base_level;
    if (base_level == EDifficultyLevel::Adaptive) {
        m_CurrentDifficulty = 0.5f;  // 中庸スタート、Tick で target へ寄せる
    } else {
        m_CurrentDifficulty = ContinuousFromDiscrete(base_level);
    }
}

/** モードを切り替える (離散モードは連続値へ即スナップ、Adaptive は current 保持)。 */
void CDynamicDifficulty::SetMode(EDifficultyLevel mode) noexcept {
    m_Mode = mode;
    if (mode != EDifficultyLevel::Adaptive) {
        // 離散モードへの切替は値を即スナップ (UI 反映が遅れない方が直感的)
        m_CurrentDifficulty = ContinuousFromDiscrete(mode);
    }
    // Adaptive への切替は current を保持 (Tick で target に向け smooth lerp 継続)
}

/** 死亡を 1 回記録する (u32 max でクランプ)。 */
void CDynamicDifficulty::RecordDeath() noexcept {
    if (_stats.deaths_last_session < 0xFFFFFFFFu) {
        ++_stats.deaths_last_session;
    }
}

/** 撃破を 1 回記録する (u32 max でクランプ)。 */
void CDynamicDifficulty::RecordKill() noexcept {
    if (_stats.kills_last_session < 0xFFFFFFFFu) {
        ++_stats.kills_last_session;
    }
}

/** レベルクリアを記録する (avg を EMA 更新、retries を 0 リセット)。 */
void CDynamicDifficulty::RecordLevelComplete(f32 time_taken) noexcept {
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

/** リトライを 1 回記録する (u32 max でクランプ)。 */
void CDynamicDifficulty::RecordRetry() noexcept {
    if (_stats.retries_current_level < 0xFFFFFFFFu) {
        ++_stats.retries_current_level;
    }
}

/** パワーアップ取得を 1 回記録する (u32 max でクランプ)。 */
void CDynamicDifficulty::RecordPowerupCollected() noexcept {
    if (_stats.powerups_collected < 0xFFFFFFFFu) {
        ++_stats.powerups_collected;
    }
}

/** 統計とセッション時間のみ初期化する (モード / current_difficulty は維持)。 */
void CDynamicDifficulty::ResetStats() noexcept {
    _stats        = FPlayerSkillStats{};
    m_SessionTime = 0.0f;
}

/** 敵 HP 乗数を返す (離散モードは表直接参照、Adaptive は 4 段表を補間)。 */
f32 CDynamicDifficulty::EnemyHealthMultiplier() const noexcept {
    if (m_Mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(m_CurrentDifficulty, kEnemyHpTable);
    }
    return kEnemyHpTable[static_cast<u32>(m_Mode)];
}

/** 敵ダメージ乗数を返す (離散モードは表直接参照、Adaptive は 4 段表を補間)。 */
f32 CDynamicDifficulty::EnemyDamageMultiplier() const noexcept {
    if (m_Mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(m_CurrentDifficulty, kEnemyDmgTable);
    }
    return kEnemyDmgTable[static_cast<u32>(m_Mode)];
}

/** 敵速度乗数を返す (離散モードは表直接参照、Adaptive は 4 段表を補間)。 */
f32 CDynamicDifficulty::EnemySpeedMultiplier() const noexcept {
    if (m_Mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(m_CurrentDifficulty, kEnemySpdTable);
    }
    return kEnemySpdTable[static_cast<u32>(m_Mode)];
}

/** プレイヤー HP 倍率を返す (離散モードは表直接参照、Adaptive は 4 段表を補間)。 */
f32 CDynamicDifficulty::PlayerHealthMultiplier() const noexcept {
    if (m_Mode == EDifficultyLevel::Adaptive) {
        return SampleCurve(m_CurrentDifficulty, kPlayerHpTable);
    }
    return kPlayerHpTable[static_cast<u32>(m_Mode)];
}

/**
 * skill 推定から Adaptive の target 難易度を算出する。
 *
 * @details
 * kill / death を分あたりに正規化して baseline と比較し、クリア速度スコアとリトライ減点を
 * 合成して符号付き skill を作る。skill = saturate(skill_signed * 0.5 + 0.5) で [-1,1] を
 * [0,1] にマップし、target = 1 - skill を返す (上手いプレイヤーほど高難易度)。
 * @return target 難易度 [0,1]。
 */
f32 CDynamicDifficulty::ComputeAdaptiveTarget() const noexcept {
    // session_time 下限ガード。プレイ開始直後の DDA 暴れを防ぐ。
    const f32 session_sec = m_SessionTime < kMinSessionTimeSec
                          ? kMinSessionTimeSec : m_SessionTime;
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

/** session_time を進め、Adaptive 時は target へ framerate-independent lerp で寄せる。 */
void CDynamicDifficulty::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    m_SessionTime += dt;

    if (m_Mode != EDifficultyLevel::Adaptive) {
        // 離散モードは current が固定 (SetMode で snap 済み) なので何もしない
        return;
    }

    const f32 target = ComputeAdaptiveTarget();
    // framerate-independent exponential smoothing (CCamera2D と同じ式)
    // t = 1 - exp(-rate * dt) で dt 不変な指数追従。rate = 0.5 で約 1.4 秒で 50%。
    const f32 t = 1.0f - Exp(-kAdaptiveLerpRate * dt);
    m_CurrentDifficulty += (target - m_CurrentDifficulty) * t;

    // 数値誤差で [0,1] を僅かに外れる可能性に備えて saturate
    m_CurrentDifficulty = Saturate(m_CurrentDifficulty);
}

} // namespace acs::game
