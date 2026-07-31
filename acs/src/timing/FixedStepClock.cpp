// SPDX-License-Identifier: Apache-2.0
#include "timing/FixedStepClock.h"

#include "timing/FixedStepMemoryValidationInternal.h"

#include <cmath>
#include <limits>

namespace acs::timing {
namespace {

/** 固定更新設定の有限性と公開上限を検証する。 */
bool IsValidOptions(const FFixedStepOptions& options) noexcept
{
    return std::isfinite(options.step_seconds) &&
           options.step_seconds >= kMinimumFixedStepSeconds &&
           options.step_seconds <= kMaximumFixedStepSeconds &&
           options.maximum_steps_per_advance > 0u &&
           options.maximum_steps_per_advance <= kMaximumFixedStepsPerAdvance &&
           std::isfinite(options.maximum_accumulated_seconds) &&
           options.maximum_accumulated_seconds >= options.step_seconds &&
           options.maximum_accumulated_seconds <= kMaximumFixedStepAccumulatedSeconds;
}

/** 保存値が通常操作で生成できる範囲内にあることを検証する。 */
bool IsValidSnapshot(const FFixedStepClockSnapshot& snapshot) noexcept
{
    return IsValidOptions(snapshot.options) &&
           std::isfinite(snapshot.accumulated_seconds) &&
           snapshot.accumulated_seconds >= 0.0 &&
           snapshot.accumulated_seconds < snapshot.options.step_seconds &&
           std::isfinite(snapshot.total_dropped_seconds) &&
           snapshot.total_dropped_seconds >= 0.0;
}

/** 浮動小数の累積値を無限大へせず最大有限値で飽和加算する。 */
f64 AddSaturated(f64 current, f64 addition) noexcept
{
    /** 累積統計に保存できる最大有限値。 */
    constexpr f64 maximum = std::numeric_limits<f64>::max();
    if (addition <= 0.0) return current;
    if (current >= maximum - addition) return maximum;
    return current + addition;
}

/** 固定更新回数を符号なし64ビットの最大値で飽和加算する。 */
u64 AddSaturated(u64 current, u64 addition) noexcept
{
    /** 累積回数に保存できる最大値。 */
    constexpr u64 maximum = std::numeric_limits<u64>::max();
    if (current >= maximum - addition) return maximum;
    return current + addition;
}

/** 刻み幅の整数境界にある経過秒から固定更新回数を求める。 */
u64 WholeStepCount(f64 accumulated, f64 step) noexcept
{
    /** 二進表現誤差を一単位だけ整数側へ寄せた除算結果。 */
    const f64 quotient = std::nextafter(accumulated / step, std::numeric_limits<f64>::infinity());
    if (quotient >= static_cast<f64>(std::numeric_limits<u64>::max())) return std::numeric_limits<u64>::max();
    return static_cast<u64>(std::floor(quotient));
}

/** 計算誤差を除き、剰余を0以上刻み幅未満へ収める。 */
f64 NormalizeRemainder(f64 remainder, f64 step) noexcept
{
    if (!std::isfinite(remainder) || remainder <= 0.0) return 0.0;
    if (remainder >= step) remainder = std::fmod(remainder, step);
    return !std::isfinite(remainder) || remainder <= 0.0 ? 0.0 : remainder;
}

} // namespace

bool FFixedStepClock::Configure(FFixedStepOptions options) noexcept
{
    if (!IsValidOptions(options)) return false;
    m_Options = options;
    Reset();
    return true;
}

bool FFixedStepClock::TryReconfigurePreservingProgress(FFixedStepOptions options) noexcept
{
    /** 変更前の設定、補間位置、累積統計。 */
    FFixedStepClockSnapshot current{};
    if (!TryCaptureSnapshot(current)) return false;

    /** 新設定を現在状態へ反映する前に検証する候補時計。 */
    FFixedStepClock candidate = *this;
    if (!candidate.Configure(options)) return false;

    /** 変更前の補間率を新しい刻み幅へ換算した持ち越し秒。 */
    f64 replacement_accumulated = current.accumulated_seconds / current.options.step_seconds * options.step_seconds;
    if (replacement_accumulated >= options.step_seconds) replacement_accumulated = std::nextafter(options.step_seconds, 0.0);

    /** 補間率と累積統計を維持した新設定の保存値。 */
    const FFixedStepClockSnapshot replacement = {options, replacement_accumulated, current.total_dropped_seconds, current.total_step_count};
    if (!candidate.TryRestoreSnapshot(replacement)) return false;

    *this = candidate;
    return true;
}

FFixedStepOptions FFixedStepClock::Options() const noexcept
{
    return m_Options;
}

FFixedStepAdvanceResult FFixedStepClock::Advance(f64 delta_seconds) noexcept
{
    /** 失敗時にも現在補間率を返す今回の結果。 */
    FFixedStepAdvanceResult result{};
    result.interpolation_alpha = InterpolationAlpha();
    if (!std::isfinite(delta_seconds) || delta_seconds < 0.0) return result;

    result.accepted = true;

    /** 今回追加できる残りの経過秒。 */
    const f64 available_capacity = m_Options.maximum_accumulated_seconds - m_AccumulatedSeconds;

    /** 蓄積上限を適用した今回の経過秒。 */
    f64 accepted_delta = delta_seconds;
    if (accepted_delta > available_capacity) {
        accepted_delta = available_capacity > 0.0 ? available_capacity : 0.0;
        result.dropped_seconds = delta_seconds - accepted_delta;
        result.was_clamped = true;
    }

    /** 今回の固定更新判定に使う総経過秒。 */
    const f64 accumulated = m_AccumulatedSeconds + accepted_delta;

    /** 総経過秒に含まれる固定更新回数。 */
    const u64 available_steps = WholeStepCount(accumulated, m_Options.step_seconds);

    /** 一回の実行上限を適用した固定更新回数。 */
    const u64 executed_steps = available_steps > m_Options.maximum_steps_per_advance ? m_Options.maximum_steps_per_advance : available_steps;

    /** 実行または破棄した固定更新分を除いた持ち越し秒。 */
    const f64 raw_remainder = accumulated - static_cast<f64>(available_steps) * m_Options.step_seconds;
    m_AccumulatedSeconds = NormalizeRemainder(raw_remainder, m_Options.step_seconds);

    if (available_steps > executed_steps) {
        /** 実行回数上限によって破棄した経過秒。 */
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

bool FFixedStepClock::TryAdvanceBatch(const f64* delta_seconds, u32 count, FFixedStepAdvanceResult* results, u32 result_capacity, u32& result_count) noexcept
{
    if (!detail::TryValidateFixedStepAdvanceBatchMemory(this, sizeof(FFixedStepClock), alignof(FFixedStepClock), delta_seconds, count, results, result_capacity, sizeof(FFixedStepAdvanceResult), alignof(FFixedStepAdvanceResult), &result_count)) {
        return false;
    }
    if (count == 0u) {
        result_count = 0u;
        return true;
    }

    /** 一括処理前の時計状態を検証する保存値。 */
    FFixedStepClockSnapshot current_state{};
    if (!TryCaptureSnapshot(current_state)) return false;

    for (u32 index = 0u; index < count; ++index) {
        if (!std::isfinite(delta_seconds[index]) || delta_seconds[index] < 0.0) return false;
    }

    for (u32 index = 0u; index < count; ++index) {
        results[index] = Advance(delta_seconds[index]);
    }
    result_count = count;
    return true;
}

bool FFixedStepClock::TryCaptureSnapshot(FFixedStepClockSnapshot* snapshot) const noexcept
{
    if (!detail::TryValidateFixedStepSnapshotMemory(this, snapshot, sizeof(FFixedStepClock), alignof(FFixedStepClock), sizeof(FFixedStepClockSnapshot), alignof(FFixedStepClockSnapshot))) {
        return false;
    }

    /** 出力へ書き込む前に内容を検証する現在状態。 */
    const FFixedStepClockSnapshot candidate = {m_Options, m_AccumulatedSeconds, m_TotalDroppedSeconds, m_TotalStepCount};
    if (!IsValidSnapshot(candidate)) return false;

    *snapshot = candidate;
    return true;
}

bool FFixedStepClock::TryRestoreSnapshot(const FFixedStepClockSnapshot* snapshot) noexcept
{
    if (!detail::TryValidateFixedStepSnapshotMemory(this, snapshot, sizeof(FFixedStepClock), alignof(FFixedStepClock), sizeof(FFixedStepClockSnapshot), alignof(FFixedStepClockSnapshot))) {
        return false;
    }
    if (!IsValidSnapshot(*snapshot)) return false;

    /** 入力領域との重複を排除した後に保持する復元候補。 */
    const FFixedStepClockSnapshot candidate = *snapshot;
    m_Options = candidate.options;
    m_AccumulatedSeconds = candidate.accumulated_seconds;
    m_TotalDroppedSeconds = candidate.total_dropped_seconds;
    m_TotalStepCount = candidate.total_step_count;
    return true;
}

void FFixedStepClock::Reset() noexcept
{
    m_AccumulatedSeconds = 0.0;
    m_TotalDroppedSeconds = 0.0;
    m_TotalStepCount = 0u;
}

f64 FFixedStepClock::AccumulatedSeconds() const noexcept
{
    return m_AccumulatedSeconds;
}

f64 FFixedStepClock::InterpolationAlpha() const noexcept
{
    if (m_Options.step_seconds <= 0.0) return 0.0;

    /** 持ち越し秒を現在の刻み幅で割った補間率。 */
    const f64 alpha = m_AccumulatedSeconds / m_Options.step_seconds;
    if (!std::isfinite(alpha) || alpha <= 0.0) return 0.0;
    return alpha < 1.0 ? alpha : 1.0;
}

u64 FFixedStepClock::TotalStepCount() const noexcept
{
    return m_TotalStepCount;
}

f64 FFixedStepClock::TotalDroppedSeconds() const noexcept
{
    return m_TotalDroppedSeconds;
}

} // namespace acs::timing
