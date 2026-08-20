// SPDX-License-Identifier: Apache-2.0
#include "gameframework/FixedStepClock.h"

#include <cmath>
#include <limits>

namespace acs::game {
namespace {

/** 設定の全項目が有限かつ公開上限内かを返す。 */
bool IsValidOptions(const FFixedStepOptions& options) noexcept {
    return std::isfinite(options.step_seconds)
        && options.step_seconds >= kMinimumFixedStepSeconds
        && options.step_seconds <= kMaximumFixedStepSeconds
        && options.maximum_steps_per_advance > 0u
        && options.maximum_steps_per_advance <= kMaximumFixedStepsPerAdvance
        && std::isfinite(options.maximum_accumulated_seconds)
        && options.maximum_accumulated_seconds >= options.step_seconds
        && options.maximum_accumulated_seconds <= kMaximumFixedStepAccumulatedSeconds;
}

/** 保存値が有効な設定と 1 step 未満の剰余を持つかを返す。 */
bool IsValidSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept {
    return IsValidOptions(snapshot.options)
        && std::isfinite(snapshot.accumulated_seconds)
        && snapshot.accumulated_seconds >= 0.0
        && snapshot.accumulated_seconds < snapshot.options.step_seconds
        && std::isfinite(snapshot.total_dropped_seconds)
        && snapshot.total_dropped_seconds >= 0.0;
}

/** 非負の浮動小数加算を最大有限値で飽和させる。 */
f64 AddSaturated(f64 current, f64 addition) noexcept {
    /** 累積統計として保持できる最大有限値。 */
    constexpr f64 maximum = std::numeric_limits<f64>::max();
    if (addition <= 0.0) return current;
    if (current >= maximum - addition) return maximum;
    return current + addition;
}

/** 固定更新回数の加算を u64 上限で飽和させる。 */
u64 AddSaturated(u64 current, u64 addition) noexcept {
    /** 累積回数として保持できる最大値。 */
    constexpr u64 maximum = std::numeric_limits<u64>::max();
    if (current >= maximum - addition) return maximum;
    return current + addition;
}

/** 蓄積秒に含まれる完全な固定 step 数を境界誤差込みで求める。 */
u64 WholeStepCount(f64 accumulated, f64 step) noexcept {
    // 境界の表現誤差だけを1 ULP整数側へ寄せる。
    /** 固定 step 数へ変換する直前の補正済み商。 */
    const f64 quotient = std::nextafter(accumulated / step, std::numeric_limits<f64>::infinity());
    if (quotient >= static_cast<f64>(std::numeric_limits<u64>::max())) {
        return std::numeric_limits<u64>::max();
    }
    return static_cast<u64>(std::floor(quotient));
}

/** 演算誤差を除き、剰余を 0 以上 1 step 未満へ正規化する。 */
f64 NormalizeRemainder(f64 remainder, f64 step) noexcept {
    if (!std::isfinite(remainder) || remainder <= 0.0) return 0.0;
    if (remainder >= step) remainder = std::fmod(remainder, step);
    return (!std::isfinite(remainder) || remainder <= 0.0) ? 0.0 : remainder;
}

} // namespace

/** 設定全体を検証し、成功時だけ時計を初期状態へ切り替える。 */
bool FFixedStepClock::Configure(FFixedStepOptions options) noexcept {
    if (!IsValidOptions(options)) return false;
    m_Options = options;
    Reset();
    return true;
}

/** 可変 delta を有界な固定更新回数、剰余、破棄時間へ分解する。 */
FFixedStepAdvanceResult FFixedStepClock::Advance(f64 delta_seconds) noexcept {
    /** 今回の固定更新回数と診断値。 */
    FFixedStepAdvanceResult result{};
    result.interpolation_alpha = InterpolationAlpha();
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) return result;

    result.accepted = true;
    /** 現在の蓄積上限まで追加できる秒数。 */
    const f64 capacity = m_Options.maximum_accumulated_seconds - m_AccumulatedSeconds;
    /** 上限検証後に時計へ取り込む秒数。 */
    f64 accepted_delta = delta_seconds;
    if (accepted_delta > capacity) {
        accepted_delta = capacity > 0.0 ? capacity : 0.0;
        result.dropped_seconds = delta_seconds - accepted_delta;
        result.was_clamped = true;
    }

    /** 既存剰余と今回受理した delta の合計。 */
    const f64 accumulated = m_AccumulatedSeconds + accepted_delta;
    /** 合計秒数に含まれる完全な固定 step 数。 */
    const u64 available_steps = WholeStepCount(accumulated, m_Options.step_seconds);
    /** 呼び出し側へ返せる上限適用後の固定 step 数。 */
    const u64 executed_steps =
        available_steps > m_Options.maximum_steps_per_advance
            ? m_Options.maximum_steps_per_advance
            : available_steps;

    m_AccumulatedSeconds = NormalizeRemainder(accumulated - static_cast<f64>(available_steps) * m_Options.step_seconds, m_Options.step_seconds);

    if (available_steps > executed_steps) {
        /** 実行回数上限によって破棄する完全な固定 step の秒数。 */
        const f64 step_limit_drop = static_cast<f64>(available_steps - executed_steps) * m_Options.step_seconds;
        result.dropped_seconds = AddSaturated(result.dropped_seconds, step_limit_drop);
        result.was_clamped = true;
    }

    m_TotalStepCount = AddSaturated(m_TotalStepCount, executed_steps);
    m_TotalDroppedSeconds = AddSaturated(m_TotalDroppedSeconds, result.dropped_seconds);
    result.step_count = static_cast<u32>(executed_steps);
    result.interpolation_alpha = InterpolationAlpha();
    return result;
}

/** 現状態を検証してから出力先へ一括反映する。 */
bool FFixedStepClock::TryCaptureSnapshot(FFixedStepClockSnapshot& snapshot) const noexcept {
    /** 部分更新を避けるため先に組み立てる保存候補。 */
    const FFixedStepClockSnapshot candidate{m_Options, m_AccumulatedSeconds, m_TotalDroppedSeconds, m_TotalStepCount};
    if (!IsValidSnapshot(candidate)) return false;
    snapshot = candidate;
    return true;
}

/** 保存値全体を検証してから時計へ一括反映する。 */
bool FFixedStepClock::TryRestoreSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept {
    if (!IsValidSnapshot(snapshot)) return false;
    /** 呼び出し元と時計の領域が重なる場合にも一括反映できる複写値。 */
    const FFixedStepClockSnapshot candidate = snapshot;
    m_Options = candidate.options;
    m_AccumulatedSeconds = candidate.accumulated_seconds;
    m_TotalDroppedSeconds = candidate.total_dropped_seconds;
    m_TotalStepCount = candidate.total_step_count;
    return true;
}

/** 設定を維持し、剰余と累積統計だけを初期化する。 */
void FFixedStepClock::Reset() noexcept {
    m_AccumulatedSeconds = 0.0;
    m_TotalDroppedSeconds = 0.0;
    m_TotalStepCount = 0u;
}

/** 剰余秒を描画補間用の 0 以上 1 以下の比率へ変換する。 */
f64 FFixedStepClock::InterpolationAlpha() const noexcept {
    /** 現在の剰余を固定 step で割った補間率。 */
    const f64 alpha = m_AccumulatedSeconds / m_Options.step_seconds;
    if (!std::isfinite(alpha) || alpha <= 0.0) return 0.0;
    return alpha < 1.0 ? alpha : 1.0;
}

} // namespace acs::game
