// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"
#include "timing/FixedStepClock.h"
#include "timing/FixedStepClockBatch.h"
#include "timing/FixedStepMemoryRangeInternal.h"
#include "timing/FixedStepMemoryValidationInternal.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>

using namespace acs;
using namespace acs::timing;

namespace {

/** 浮動小数の差が固定更新試験の許容範囲内にあるかを返す。 */
bool Near(f64 left, f64 right, f64 tolerance = 1.0e-9) noexcept
{
    return std::fabs(left - right) <= tolerance;
}

/** 二つの設定値が全項目で一致するかを返す。 */
bool SameOptions(const FFixedStepOptions& first, const FFixedStepOptions& second) noexcept
{
    return first.step_seconds == second.step_seconds &&
           first.maximum_steps_per_advance == second.maximum_steps_per_advance &&
           first.maximum_accumulated_seconds == second.maximum_accumulated_seconds;
}

/** 二つの保存値が全項目で一致するかを返す。 */
bool SameSnapshot(const FFixedStepClockSnapshot& first, const FFixedStepClockSnapshot& second) noexcept
{
    return SameOptions(first.options, second.options) &&
           first.accumulated_seconds == second.accumulated_seconds &&
           first.total_dropped_seconds == second.total_dropped_seconds &&
           first.total_step_count == second.total_step_count;
}

/** 二つの経過入力結果が全項目で一致するかを返す。 */
bool SameResult(const FFixedStepAdvanceResult& first, const FFixedStepAdvanceResult& second) noexcept
{
    return first.step_count == second.step_count &&
           first.interpolation_alpha == second.interpolation_alpha &&
           first.dropped_seconds == second.dropped_seconds &&
           first.accepted == second.accepted &&
           first.was_clamped == second.was_clamped;
}

/** 時計の現在状態を取得し、試験で取得できない場合は既定値を返す。 */
FFixedStepClockSnapshot Capture(const CFixedStepClock& clock) noexcept
{
    /** 時計から取得する現在状態。 */
    FFixedStepClockSnapshot snapshot{};
    (void)clock.TryCaptureSnapshot(snapshot);
    return snapshot;
}

static_assert(std::is_standard_layout_v<FFixedStepOptions>);
static_assert(std::is_trivially_copyable_v<FFixedStepOptions>);
static_assert(sizeof(FFixedStepOptions) == 24u);
static_assert(alignof(FFixedStepOptions) == 8u);
static_assert(std::is_standard_layout_v<FFixedStepAdvanceResult>);
static_assert(std::is_trivially_copyable_v<FFixedStepAdvanceResult>);
static_assert(sizeof(FFixedStepAdvanceResult) == 32u);
static_assert(alignof(FFixedStepAdvanceResult) == 8u);
static_assert(std::is_standard_layout_v<FFixedStepClockSnapshot>);
static_assert(std::is_trivially_copyable_v<FFixedStepClockSnapshot>);
static_assert(sizeof(FFixedStepClockSnapshot) == 48u);
static_assert(alignof(FFixedStepClockSnapshot) == 8u);
static_assert(std::is_same_v<FFixedStepClock, CFixedStepClock>);
static_assert(std::is_standard_layout_v<CFixedStepClock>);
static_assert(std::is_trivially_copyable_v<CFixedStepClock>);
static_assert(sizeof(CFixedStepClock) == 48u);
static_assert(alignof(CFixedStepClock) == 8u);
static_assert(sizeof(detail::FFixedStepMemoryRangeInternal) == sizeof(std::uintptr_t) * 2u);
static_assert(alignof(detail::FFixedStepMemoryRangeInternal) == alignof(std::uintptr_t));
static_assert(noexcept(static_cast<CFixedStepClock*>(nullptr)->TryAdvanceBatch(nullptr, 0u, nullptr, 0u, *static_cast<u32*>(nullptr))));
static_assert(noexcept(static_cast<const CFixedStepClock*>(nullptr)->TryCaptureSnapshot(static_cast<FFixedStepClockSnapshot*>(nullptr))));
static_assert(noexcept(static_cast<CFixedStepClock*>(nullptr)->TryRestoreSnapshot(static_cast<const FFixedStepClockSnapshot*>(nullptr))));
static_assert(noexcept(TryCaptureFixedStepClockSnapshots(nullptr, 0u, nullptr, 0u, *static_cast<u32*>(nullptr))));
static_assert(noexcept(TryRestoreFixedStepClockSnapshots(nullptr, nullptr, 0u)));

} // namespace

ACS_TEST(FixedStepClock, DefaultsAccumulateDeterministically)
{
    /** 既定設定で進める時計。 */
    CFixedStepClock clock{};

    /** 既定刻み幅の半分に相当する経過秒。 */
    const f64 half_step = clock.Options().step_seconds * 0.5;

    /** 一回目の半刻み入力結果。 */
    const FFixedStepAdvanceResult first = clock.Advance(half_step);
    EXPECT_TRUE(first.accepted);
    EXPECT_EQ(first.step_count, 0u);
    EXPECT_FALSE(first.was_clamped);
    EXPECT_TRUE(Near(first.interpolation_alpha, 0.5));

    /** 二回目の半刻み入力結果。 */
    const FFixedStepAdvanceResult second = clock.Advance(half_step);
    EXPECT_TRUE(second.accepted);
    EXPECT_EQ(second.step_count, 1u);
    EXPECT_FALSE(second.was_clamped);
    EXPECT_TRUE(Near(second.interpolation_alpha, 0.0));
    EXPECT_EQ(clock.TotalStepCount(), 1u);
}

ACS_TEST(FixedStepClock, MatchesDecimalBoundaryAndClampGoldens)
{
    /** 十進小数境界を確認する時計。 */
    CFixedStepClock clock{};

    /** 0.1秒刻みで十分な蓄積幅を持つ設定。 */
    const FFixedStepOptions options = {0.1, 8u, 1.0};
    EXPECT_TRUE(clock.Configure(options));

    /** 0.3秒を一括入力した境界結果。 */
    const FFixedStepAdvanceResult single = clock.Advance(0.3);
    EXPECT_TRUE(single.accepted);
    EXPECT_EQ(single.step_count, 3u);
    EXPECT_FALSE(single.was_clamped);
    EXPECT_TRUE(Near(single.interpolation_alpha, 0.0));

    clock.Reset();

    /** 0.1秒を三回入力して得た固定更新回数。 */
    u32 repeated_steps = 0u;
    repeated_steps += clock.Advance(0.1).step_count;
    repeated_steps += clock.Advance(0.1).step_count;
    repeated_steps += clock.Advance(0.1).step_count;
    EXPECT_EQ(repeated_steps, 3u);
    EXPECT_TRUE(Near(clock.InterpolationAlpha(), 0.0));

    clock.Reset();

    /** 0.3秒境界をわずかに下回る入力結果。 */
    const FFixedStepAdvanceResult below = clock.Advance(0.3 - 1.0e-12);
    EXPECT_EQ(below.step_count, 2u);
    EXPECT_TRUE(below.interpolation_alpha > 0.99 && below.interpolation_alpha < 1.0);

    clock.Reset();

    /** 0.3秒境界をわずかに上回る入力結果。 */
    const FFixedStepAdvanceResult above = clock.Advance(0.3 + 1.0e-12);
    EXPECT_EQ(above.step_count, 3u);
    EXPECT_TRUE(above.interpolation_alpha > 0.0 && above.interpolation_alpha < 1.0e-8);

    /** 一回二更新へ制限した設定。 */
    const FFixedStepOptions limited_options = {0.1, 2u, 1.0};
    EXPECT_TRUE(clock.Configure(limited_options));

    /** 更新回数上限で五刻み分を破棄する入力結果。 */
    const FFixedStepAdvanceResult limited = clock.Advance(0.75);
    EXPECT_EQ(limited.step_count, 2u);
    EXPECT_TRUE(limited.was_clamped);
    EXPECT_TRUE(Near(limited.dropped_seconds, 0.5));
    EXPECT_TRUE(Near(limited.interpolation_alpha, 0.5));
    EXPECT_EQ(clock.TotalStepCount(), 2u);
    EXPECT_TRUE(Near(clock.TotalDroppedSeconds(), 0.5));
}

ACS_TEST(FixedStepClock, RejectsInvalidInputsWithoutChangingState)
{
    /** 失敗時不変を確認する時計。 */
    CFixedStepClock clock{};

    /** 公開下限をすべて満たす最小設定。 */
    const FFixedStepOptions lower_boundary = {kMinimumFixedStepSeconds, 1u, kMinimumFixedStepSeconds};
    EXPECT_TRUE(clock.Configure(lower_boundary));

    /** 公開上限をすべて満たす最大設定。 */
    const FFixedStepOptions upper_boundary = {kMaximumFixedStepSeconds, kMaximumFixedStepsPerAdvance, kMaximumFixedStepAccumulatedSeconds};
    EXPECT_TRUE(clock.Configure(upper_boundary));

    EXPECT_TRUE(clock.Configure({0.1, 4u, 1.0}));
    (void)clock.Advance(0.25);

    /** 不正入力前の時計状態。 */
    const FFixedStepClockSnapshot before = Capture(clock);

    /** 状態を変更してはならない不正な経過秒。 */
    const f64 invalid_deltas[] = {-1.0, std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN()};
    for (f64 delta_seconds : invalid_deltas) {
        /** 不正な経過秒に対する拒否結果。 */
        const FFixedStepAdvanceResult rejected = clock.Advance(delta_seconds);
        EXPECT_FALSE(rejected.accepted);
        EXPECT_EQ(rejected.step_count, 0u);
        EXPECT_TRUE(SameSnapshot(Capture(clock), before));
    }

    /** 設定失敗時に保持する初期候補。 */
    FFixedStepOptions invalid_options = before.options;
    invalid_options.step_seconds = std::nextafter(kMinimumFixedStepSeconds, 0.0);
    EXPECT_FALSE(clock.Configure(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    invalid_options = before.options;
    invalid_options.step_seconds = std::nextafter(kMaximumFixedStepSeconds, std::numeric_limits<f64>::infinity());
    EXPECT_FALSE(clock.Configure(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    invalid_options = before.options;
    invalid_options.maximum_steps_per_advance = 0u;
    EXPECT_FALSE(clock.TryReconfigurePreservingProgress(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    invalid_options = before.options;
    invalid_options.maximum_steps_per_advance = kMaximumFixedStepsPerAdvance + 1u;
    EXPECT_FALSE(clock.Configure(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    invalid_options = before.options;
    invalid_options.maximum_accumulated_seconds = invalid_options.step_seconds * 0.5;
    EXPECT_FALSE(clock.Configure(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    invalid_options = before.options;
    invalid_options.maximum_accumulated_seconds = std::nextafter(kMaximumFixedStepAccumulatedSeconds, std::numeric_limits<f64>::infinity());
    EXPECT_FALSE(clock.Configure(invalid_options));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));
}

ACS_TEST(FixedStepClock, RestoresSnapshotsAndPreservesProgressWhenReconfigured)
{
    /** 保存と再設定を確認する時計。 */
    CFixedStepClock clock{};
    EXPECT_TRUE(clock.Configure({0.25, 3u, 1.0}));
    (void)clock.Advance(0.625);

    /** 復元後の正規状態として保存する値。 */
    const FFixedStepClockSnapshot saved = Capture(clock);
    EXPECT_EQ(saved.total_step_count, 2u);
    EXPECT_TRUE(Near(saved.accumulated_seconds, 0.125));

    (void)clock.Advance(0.25);
    EXPECT_TRUE(clock.TryRestoreSnapshot(saved));
    EXPECT_TRUE(SameSnapshot(Capture(clock), saved));

    /** 設定変更前に維持する補間率。 */
    const f64 alpha_before = clock.InterpolationAlpha();

    /** 補間率を維持して適用する新設定。 */
    const FFixedStepOptions replacement = {0.5, 4u, 2.0};
    EXPECT_TRUE(clock.TryReconfigurePreservingProgress(replacement));
    EXPECT_TRUE(Near(clock.InterpolationAlpha(), alpha_before));
    EXPECT_TRUE(Near(clock.AccumulatedSeconds(), 0.25));
    EXPECT_EQ(clock.TotalStepCount(), saved.total_step_count);
    EXPECT_TRUE(Near(clock.TotalDroppedSeconds(), saved.total_dropped_seconds));

    /** 不正な保存値を拒否する前の時計状態。 */
    const FFixedStepClockSnapshot before_invalid = Capture(clock);

    /** 持ち越し秒が刻み幅へ到達した不正な保存値。 */
    FFixedStepClockSnapshot invalid = before_invalid;
    invalid.accumulated_seconds = invalid.options.step_seconds;
    EXPECT_FALSE(clock.TryRestoreSnapshot(invalid));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before_invalid));

    invalid = before_invalid;
    invalid.total_dropped_seconds = std::numeric_limits<f64>::quiet_NaN();
    EXPECT_FALSE(clock.TryRestoreSnapshot(invalid));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before_invalid));
}

ACS_TEST(FixedStepClock, AdvancesBatchAtomicallyAndMatchesSequentialOrder)
{
    /** 一括処理を実行する時計。 */
    CFixedStepClock batch_clock{};

    /** 単発処理の期待値を作る時計。 */
    CFixedStepClock sequential_clock{};
    EXPECT_TRUE(batch_clock.Configure({0.1, 2u, 1.0}));
    EXPECT_TRUE(sequential_clock.Configure({0.1, 2u, 1.0}));

    /** 一括処理する経過秒列。 */
    const f64 deltas[] = {0.05, 0.15, 0.6, 0.0};

    /** 一括処理が書き込む結果列。 */
    FFixedStepAdvanceResult batch_results[4] = {};

    /** 一括処理後に確定する結果件数。 */
    u32 result_count = 91u;
    EXPECT_TRUE(batch_clock.TryAdvanceBatch(deltas, 4u, batch_results, 4u, result_count));
    EXPECT_EQ(result_count, 4u);

    for (u32 index = 0u; index < 4u; ++index) {
        /** 同じ入力を単発処理した期待結果。 */
        const FFixedStepAdvanceResult expected = sequential_clock.Advance(deltas[index]);
        EXPECT_TRUE(SameResult(batch_results[index], expected));
    }
    EXPECT_TRUE(SameSnapshot(Capture(batch_clock), Capture(sequential_clock)));

    /** 失敗時に保持する一括処理前の時計状態。 */
    const FFixedStepClockSnapshot before_invalid = Capture(batch_clock);

    /** 二件目が不正な入力列。 */
    const f64 invalid_deltas[] = {0.1, std::numeric_limits<f64>::quiet_NaN(), 0.2};

    /** 失敗時に全域を保持する結果列。 */
    FFixedStepAdvanceResult unchanged_results[3] = {{7u, 0.25, 3.0, true, true}, {8u, 0.5, 4.0, true, false}, {9u, 0.75, 5.0, false, true}};

    /** 失敗前の結果列をバイト単位で固定する値。 */
    FFixedStepAdvanceResult expected_results[3] = {};
    std::memcpy(expected_results, unchanged_results, sizeof(expected_results));

    result_count = 77u;
    EXPECT_FALSE(batch_clock.TryAdvanceBatch(invalid_deltas, 3u, unchanged_results, 3u, result_count));
    EXPECT_EQ(result_count, 77u);
    EXPECT_TRUE(std::memcmp(expected_results, unchanged_results, sizeof(expected_results)) == 0);
    EXPECT_TRUE(SameSnapshot(Capture(batch_clock), before_invalid));

    result_count = 66u;
    EXPECT_FALSE(batch_clock.TryAdvanceBatch(deltas, 4u, batch_results, 3u, result_count));
    EXPECT_EQ(result_count, 66u);
    EXPECT_TRUE(SameSnapshot(Capture(batch_clock), before_invalid));

    result_count = 55u;
    EXPECT_TRUE(batch_clock.TryAdvanceBatch(nullptr, 0u, nullptr, 0u, result_count));
    EXPECT_EQ(result_count, 0u);

    result_count = 44u;
    EXPECT_FALSE(batch_clock.TryAdvanceBatch(deltas, kMaximumFixedStepBatchCount + 1u, batch_results, kMaximumFixedStepBatchCount + 1u, result_count));
    EXPECT_EQ(result_count, 44u);
}

ACS_TEST(FixedStepClock, RejectsInvalidRawMemoryAndOverlap)
{
    /** 生領域検証を確認する時計。 */
    CFixedStepClock clock{};
    (void)clock.Advance(clock.Options().step_seconds * 0.5);

    /** 生領域失敗前の時計状態。 */
    const FFixedStepClockSnapshot before = Capture(clock);
    EXPECT_FALSE(clock.TryCaptureSnapshot(static_cast<FFixedStepClockSnapshot*>(nullptr)));
    EXPECT_FALSE(clock.TryRestoreSnapshot(static_cast<const FFixedStepClockSnapshot*>(nullptr)));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 保存値を整列から一バイトずらすための領域。 */
    alignas(FFixedStepClockSnapshot) unsigned char snapshot_storage[sizeof(FFixedStepClockSnapshot) + alignof(FFixedStepClockSnapshot)] = {};

    /** 意図的に整列を崩した保存先。 */
    FFixedStepClockSnapshot* const misaligned_snapshot = reinterpret_cast<FFixedStepClockSnapshot*>(snapshot_storage + 1u);
    EXPECT_FALSE(clock.TryCaptureSnapshot(misaligned_snapshot));
    EXPECT_FALSE(clock.TryRestoreSnapshot(misaligned_snapshot));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 時計領域そのものを保存値として誤指定する重複先。 */
    FFixedStepClockSnapshot* const overlapping_snapshot = reinterpret_cast<FFixedStepClockSnapshot*>(&clock);
    EXPECT_FALSE(clock.TryCaptureSnapshot(overlapping_snapshot));
    EXPECT_FALSE(clock.TryRestoreSnapshot(overlapping_snapshot));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 重複拒否時に保持する結果値。 */
    FFixedStepAdvanceResult result = {19u, 0.75, 9.0, true, true};

    /** 重複拒否時に保持する結果件数。 */
    u32 result_count = 33u;

    /** 時計領域内を経過秒入力として誤指定する重複先。 */
    const f64* const overlapping_delta = reinterpret_cast<const f64*>(&clock);
    EXPECT_FALSE(clock.TryAdvanceBatch(overlapping_delta, 1u, &result, 1u, result_count));
    EXPECT_EQ(result.step_count, 19u);
    EXPECT_EQ(result_count, 33u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 経過秒を整列から一バイトずらすための領域。 */
    alignas(f64) unsigned char delta_storage[sizeof(f64) + alignof(f64)] = {};

    /** 意図的に整列を崩した経過秒入力。 */
    const f64* const misaligned_delta = reinterpret_cast<const f64*>(delta_storage + 1u);
    EXPECT_FALSE(clock.TryAdvanceBatch(misaligned_delta, 1u, &result, 1u, result_count));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));
}

ACS_TEST(FixedStepClock, RejectsOverflowingRawAddressRanges)
{
    /** 失敗時に変更されないことを確認する番兵領域。 */
    detail::FFixedStepMemoryRangeInternal range = {11u, 22u};

    /** 整列を満たしながら終端加算がアドレス上限を超える先頭値。 */
    constexpr std::uintptr_t maximum_address = std::numeric_limits<std::uintptr_t>::max();
    constexpr std::uintptr_t overflowing_address = maximum_address & ~static_cast<std::uintptr_t>(7u);
    const void* const overflowing_pointer = reinterpret_cast<const void*>(overflowing_address);
    EXPECT_FALSE(detail::TryMakeFixedStepMemoryRange(overflowing_pointer, 16u, 1u, 8u, range));
    EXPECT_EQ(range.begin, static_cast<std::uintptr_t>(11u));
    EXPECT_EQ(range.end, static_cast<std::uintptr_t>(22u));

    /** 要素数との乗算がアドレス幅を超える一要素のバイト数。 */
    const usize overflowing_element_size = static_cast<usize>(maximum_address / 2u + 1u);
    const void* const aligned_pointer = reinterpret_cast<const void*>(static_cast<std::uintptr_t>(8u));
    EXPECT_FALSE(detail::TryMakeFixedStepMemoryRange(aligned_pointer, overflowing_element_size, 2u, 8u, range));
    EXPECT_EQ(range.begin, static_cast<std::uintptr_t>(11u));
    EXPECT_EQ(range.end, static_cast<std::uintptr_t>(22u));

    EXPECT_TRUE(detail::TryMakeFixedStepMemoryRange(nullptr, sizeof(u64), 0u, alignof(u64), range));
    EXPECT_EQ(range.begin, static_cast<std::uintptr_t>(0u));
    EXPECT_EQ(range.end, static_cast<std::uintptr_t>(0u));
}

ACS_TEST(FixedStepClock, RejectsAliasedAdvanceBatchRangesAtomically)
{
    /** 重複拒否で状態を維持する基準時計。 */
    CFixedStepClock clock{};
    (void)clock.Advance(clock.Options().step_seconds * 0.5);

    /** 各失敗後に照合する基準状態。 */
    const FFixedStepClockSnapshot before = Capture(clock);

    /** 一括入力に使う有効な経過秒。 */
    const f64 delta = 0.0;

    /** 失敗時に維持する結果の番兵値。 */
    const FFixedStepAdvanceResult sentinel = {21u, 0.75, 8.0, true, true};

    /** null入力拒否を確認する結果。 */
    FFixedStepAdvanceResult result = sentinel;

    /** null入力拒否で維持する結果件数。 */
    u32 result_count = 701u;
    EXPECT_FALSE(clock.TryAdvanceBatch(nullptr, 1u, &result, 1u, result_count));
    EXPECT_FALSE(clock.TryAdvanceBatch(&delta, 1u, nullptr, 1u, result_count));
    EXPECT_TRUE(SameResult(result, sentinel));
    EXPECT_EQ(result_count, 701u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 経過秒と結果が同じ領域を指す重複試験領域。 */
    alignas(FFixedStepAdvanceResult) unsigned char delta_result_storage[sizeof(FFixedStepAdvanceResult)] = {};
    std::memset(delta_result_storage, 0x5au, sizeof(delta_result_storage));

    /** 重複試験前の全バイト。 */
    unsigned char delta_result_before[sizeof(delta_result_storage)] = {};
    std::memcpy(delta_result_before, delta_result_storage, sizeof(delta_result_storage));

    /** 結果領域と重なる経過秒入力。 */
    const f64* const aliased_delta = reinterpret_cast<const f64*>(delta_result_storage);

    /** 経過秒領域と重なる結果出力。 */
    FFixedStepAdvanceResult* const aliased_result = reinterpret_cast<FFixedStepAdvanceResult*>(delta_result_storage);
    result_count = 702u;
    EXPECT_FALSE(clock.TryAdvanceBatch(aliased_delta, 1u, aliased_result, 1u, result_count));
    EXPECT_EQ(std::memcmp(delta_result_before, delta_result_storage, sizeof(delta_result_storage)), 0);
    EXPECT_EQ(result_count, 702u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 経過秒と結果件数を同じ先頭へ置く重複試験領域。 */
    alignas(f64) unsigned char delta_count_storage[sizeof(f64)] = {};

    /** 経過秒領域内で寿命を開始する結果件数。 */
    u32* const delta_count = ::new (static_cast<void*>(delta_count_storage)) u32(703u);

    /** 結果件数と重なる経過秒入力。 */
    const f64* const count_aliased_delta = reinterpret_cast<const f64*>(delta_count_storage);
    result = sentinel;
    EXPECT_FALSE(clock.TryAdvanceBatch(count_aliased_delta, 1u, &result, 1u, *delta_count));
    EXPECT_EQ(*delta_count, 703u);
    EXPECT_TRUE(SameResult(result, sentinel));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 結果と結果件数を同じ先頭へ置く重複試験領域。 */
    alignas(FFixedStepAdvanceResult) unsigned char result_count_storage[sizeof(FFixedStepAdvanceResult)] = {};

    /** 結果領域内で寿命を開始する結果件数。 */
    u32* const direct_result_count = ::new (static_cast<void*>(result_count_storage)) u32(704u);

    /** 結果件数と先頭から重なる結果出力。 */
    FFixedStepAdvanceResult* const count_aliased_result = reinterpret_cast<FFixedStepAdvanceResult*>(result_count_storage);
    EXPECT_FALSE(clock.TryAdvanceBatch(&delta, 1u, count_aliased_result, 1u, *direct_result_count));
    EXPECT_EQ(*direct_result_count, 704u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 結果容量の未使用末尾へ結果件数を置く試験領域。 */
    alignas(FFixedStepAdvanceResult) unsigned char result_tail_count_storage[sizeof(FFixedStepAdvanceResult) * 2u] = {};

    /** 結果容量の先頭で寿命を開始する番兵結果。 */
    FFixedStepAdvanceResult* const tail_count_results = ::new (static_cast<void*>(result_tail_count_storage)) FFixedStepAdvanceResult(sentinel);

    /** 二件目の結果領域内で寿命を開始する結果件数。 */
    u32* const tail_result_count = ::new (static_cast<void*>(result_tail_count_storage + sizeof(FFixedStepAdvanceResult))) u32(705u);
    EXPECT_FALSE(clock.TryAdvanceBatch(&delta, 1u, tail_count_results, 2u, *tail_result_count));
    EXPECT_TRUE(SameResult(*tail_count_results, sentinel));
    EXPECT_EQ(*tail_result_count, 705u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 結果容量の未使用末尾へ時計を置く試験領域。 */
    alignas(FFixedStepAdvanceResult) alignas(CFixedStepClock) unsigned char result_tail_clock_storage[sizeof(FFixedStepAdvanceResult) + sizeof(CFixedStepClock)] = {};

    /** 結果容量の先頭で寿命を開始する番兵結果。 */
    FFixedStepAdvanceResult* const tail_clock_results = ::new (static_cast<void*>(result_tail_clock_storage)) FFixedStepAdvanceResult(sentinel);

    /** 二件目の結果領域と重なる位置で寿命を開始する時計。 */
    CFixedStepClock* const tail_result_clock = ::new (static_cast<void*>(result_tail_clock_storage + sizeof(FFixedStepAdvanceResult))) CFixedStepClock();

    /** 結果容量重複前の時計状態。 */
    const FFixedStepClockSnapshot tail_clock_before = Capture(*tail_result_clock);

    /** 結果容量重複で維持する結果件数。 */
    u32 tail_clock_count = 706u;
    EXPECT_FALSE(tail_result_clock->TryAdvanceBatch(&delta, 1u, tail_clock_results, 2u, tail_clock_count));
    EXPECT_TRUE(SameResult(*tail_clock_results, sentinel));
    EXPECT_EQ(tail_clock_count, 706u);
    EXPECT_TRUE(SameSnapshot(Capture(*tail_result_clock), tail_clock_before));

    /** 結果出力を整列から一バイトずらす領域。 */
    alignas(FFixedStepAdvanceResult) unsigned char misaligned_result_storage[sizeof(FFixedStepAdvanceResult) + alignof(FFixedStepAdvanceResult)] = {};

    /** 意図的に整列を崩した結果出力。 */
    FFixedStepAdvanceResult* const misaligned_result = reinterpret_cast<FFixedStepAdvanceResult*>(misaligned_result_storage + 1u);
    result_count = 707u;
    EXPECT_FALSE(clock.TryAdvanceBatch(&delta, 1u, misaligned_result, 1u, result_count));
    EXPECT_EQ(result_count, 707u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** アドレス幅の終端を超える配列試験の最大値。 */
    constexpr std::uintptr_t maximum_address = std::numeric_limits<std::uintptr_t>::max();

    /** 整列を満たす最大の経過秒アドレス。 */
    constexpr std::uintptr_t maximum_delta_address = maximum_address - maximum_address % static_cast<std::uintptr_t>(alignof(f64));

    /** 終端がアドレス幅を超える経過秒入力。 */
    const f64* const overflowing_deltas = reinterpret_cast<const f64*>(maximum_delta_address);

    /** 高位アドレス拒否で維持する結果列。 */
    FFixedStepAdvanceResult high_address_results[2] = {sentinel, sentinel};
    result_count = 708u;
    EXPECT_FALSE(clock.TryAdvanceBatch(overflowing_deltas, 2u, high_address_results, 2u, result_count));
    EXPECT_TRUE(SameResult(high_address_results[0], sentinel));
    EXPECT_TRUE(SameResult(high_address_results[1], sentinel));
    EXPECT_EQ(result_count, 708u);

    /** 整列を満たす最大の結果アドレス。 */
    constexpr std::uintptr_t maximum_result_address = maximum_address - maximum_address % static_cast<std::uintptr_t>(alignof(FFixedStepAdvanceResult));

    /** 終端がアドレス幅を超える結果出力。 */
    FFixedStepAdvanceResult* const overflowing_results = reinterpret_cast<FFixedStepAdvanceResult*>(maximum_result_address);

    /** 高位結果アドレス試験に使う有効な経過秒列。 */
    const f64 valid_deltas[2] = {0.0, 0.0};
    result_count = 709u;
    EXPECT_FALSE(clock.TryAdvanceBatch(valid_deltas, 2u, overflowing_results, 2u, result_count));
    EXPECT_EQ(result_count, 709u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));
}

ACS_TEST(FixedStepClock, RejectsAliasedSnapshotBatchRangesAtomically)
{
    /** 重複拒否で状態を維持する基準時計。 */
    CFixedStepClock clock{};
    (void)clock.Advance(clock.Options().step_seconds * 0.5);

    /** 各失敗後に照合する基準状態。 */
    const FFixedStepClockSnapshot before = Capture(clock);

    /** 失敗時に維持する保存値の番兵。 */
    const FFixedStepClockSnapshot sentinel = {{0.5, 2u, 1.0}, 0.25, 3.0, 4u};

    /** null入力拒否を確認する保存先。 */
    FFixedStepClockSnapshot snapshot = sentinel;

    /** null入力拒否で維持する保存件数。 */
    u32 snapshot_count = 801u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(nullptr, 1u, &snapshot, 1u, snapshot_count));
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(&clock, 1u, nullptr, 1u, snapshot_count));
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(nullptr, &snapshot, 1u));
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(&clock, nullptr, 1u));
    EXPECT_TRUE(SameSnapshot(snapshot, sentinel));
    EXPECT_EQ(snapshot_count, 801u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 保存先と保存件数を同じ先頭へ置く重複試験領域。 */
    alignas(FFixedStepClockSnapshot) unsigned char snapshot_count_storage[sizeof(FFixedStepClockSnapshot)] = {};

    /** 保存先領域内で寿命を開始する保存件数。 */
    u32* const direct_snapshot_count = ::new (static_cast<void*>(snapshot_count_storage)) u32(802u);

    /** 保存件数と先頭から重なる保存先。 */
    FFixedStepClockSnapshot* const count_aliased_snapshot = reinterpret_cast<FFixedStepClockSnapshot*>(snapshot_count_storage);
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(&clock, 1u, count_aliased_snapshot, 1u, *direct_snapshot_count));
    EXPECT_EQ(*direct_snapshot_count, 802u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 保存容量の未使用末尾へ保存件数を置く試験領域。 */
    alignas(FFixedStepClockSnapshot) unsigned char snapshot_tail_count_storage[sizeof(FFixedStepClockSnapshot) * 2u] = {};

    /** 保存容量の先頭で寿命を開始する番兵保存値。 */
    FFixedStepClockSnapshot* const tail_count_snapshots = ::new (static_cast<void*>(snapshot_tail_count_storage)) FFixedStepClockSnapshot(sentinel);

    /** 二件目の保存領域内で寿命を開始する保存件数。 */
    u32* const tail_snapshot_count = ::new (static_cast<void*>(snapshot_tail_count_storage + sizeof(FFixedStepClockSnapshot))) u32(803u);
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(&clock, 1u, tail_count_snapshots, 2u, *tail_snapshot_count));
    EXPECT_TRUE(SameSnapshot(*tail_count_snapshots, sentinel));
    EXPECT_EQ(*tail_snapshot_count, 803u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 保存容量の未使用末尾へ時計を置く試験領域。 */
    alignas(FFixedStepClockSnapshot) alignas(CFixedStepClock) unsigned char snapshot_tail_clock_storage[sizeof(FFixedStepClockSnapshot) + sizeof(CFixedStepClock)] = {};

    /** 保存容量の先頭で寿命を開始する番兵保存値。 */
    FFixedStepClockSnapshot* const tail_clock_snapshots = ::new (static_cast<void*>(snapshot_tail_clock_storage)) FFixedStepClockSnapshot(sentinel);

    /** 二件目の保存領域と重なる位置で寿命を開始する時計。 */
    CFixedStepClock* const tail_snapshot_clock = ::new (static_cast<void*>(snapshot_tail_clock_storage + sizeof(FFixedStepClockSnapshot))) CFixedStepClock();

    /** 保存容量重複前の時計状態。 */
    const FFixedStepClockSnapshot tail_clock_before = Capture(*tail_snapshot_clock);

    /** 保存容量重複で維持する保存件数。 */
    u32 tail_clock_count = 804u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(tail_snapshot_clock, 1u, tail_clock_snapshots, 2u, tail_clock_count));
    EXPECT_TRUE(SameSnapshot(*tail_clock_snapshots, sentinel));
    EXPECT_EQ(tail_clock_count, 804u);
    EXPECT_TRUE(SameSnapshot(Capture(*tail_snapshot_clock), tail_clock_before));

    /** 複数保存先を整列から一バイトずらす領域。 */
    alignas(FFixedStepClockSnapshot) unsigned char misaligned_snapshot_storage[sizeof(FFixedStepClockSnapshot) + alignof(FFixedStepClockSnapshot)] = {};

    /** 意図的に整列を崩した複数保存先。 */
    FFixedStepClockSnapshot* const misaligned_snapshots = reinterpret_cast<FFixedStepClockSnapshot*>(misaligned_snapshot_storage + 1u);
    snapshot_count = 805u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(&clock, 1u, misaligned_snapshots, 1u, snapshot_count));
    EXPECT_EQ(snapshot_count, 805u);
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 複数復元先を整列から一バイトずらす領域。 */
    alignas(CFixedStepClock) unsigned char misaligned_clock_storage[sizeof(CFixedStepClock) + alignof(CFixedStepClock)] = {};
    std::memset(misaligned_clock_storage, 0x6bu, sizeof(misaligned_clock_storage));

    /** 不整列復元拒否前の全バイト。 */
    unsigned char misaligned_clock_before[sizeof(misaligned_clock_storage)] = {};
    std::memcpy(misaligned_clock_before, misaligned_clock_storage, sizeof(misaligned_clock_storage));

    /** 意図的に整列を崩した複数復元先。 */
    CFixedStepClock* const misaligned_clocks = reinterpret_cast<CFixedStepClock*>(misaligned_clock_storage + 1u);
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(misaligned_clocks, &sentinel, 1u));
    EXPECT_EQ(std::memcmp(misaligned_clock_before, misaligned_clock_storage, sizeof(misaligned_clock_storage)), 0);

    /** アドレス幅の終端を超える保存値試験の最大値。 */
    constexpr std::uintptr_t maximum_address = std::numeric_limits<std::uintptr_t>::max();

    /** 整列を満たす最大の保存値アドレス。 */
    constexpr std::uintptr_t maximum_snapshot_address = maximum_address - maximum_address % static_cast<std::uintptr_t>(alignof(FFixedStepClockSnapshot));

    /** 終端がアドレス幅を超える単一保存先。 */
    FFixedStepClockSnapshot* const overflowing_snapshot = reinterpret_cast<FFixedStepClockSnapshot*>(maximum_snapshot_address);
    EXPECT_FALSE(clock.TryCaptureSnapshot(overflowing_snapshot));
    EXPECT_FALSE(clock.TryRestoreSnapshot(overflowing_snapshot));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));

    /** 整列を満たす最大の時計アドレス。 */
    constexpr std::uintptr_t maximum_clock_address = maximum_address - maximum_address % static_cast<std::uintptr_t>(alignof(CFixedStepClock));

    /** 終端がアドレス幅を超える時計入力。 */
    const CFixedStepClock* const overflowing_clocks = reinterpret_cast<const CFixedStepClock*>(maximum_clock_address);

    /** 高位時計アドレス拒否で維持する保存先列。 */
    FFixedStepClockSnapshot high_address_snapshots[2] = {sentinel, sentinel};
    snapshot_count = 806u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(overflowing_clocks, 2u, high_address_snapshots, 2u, snapshot_count));
    EXPECT_EQ(snapshot_count, 806u);
    EXPECT_TRUE(SameSnapshot(high_address_snapshots[0], sentinel));
    EXPECT_TRUE(SameSnapshot(high_address_snapshots[1], sentinel));

    /** 終端がアドレス幅を超える時計復元先。 */
    CFixedStepClock* const overflowing_clock_outputs = reinterpret_cast<CFixedStepClock*>(maximum_clock_address);
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(overflowing_clock_outputs, high_address_snapshots, 2u));
    EXPECT_TRUE(SameSnapshot(Capture(clock), before));
}

ACS_TEST(FixedStepClock, CapturesAndRestoresMultipleSnapshotsAtomically)
{
    /** 複数保存を確認する二つの時計。 */
    CFixedStepClock clocks[2] = {};
    EXPECT_TRUE(clocks[0].Configure({0.1, 4u, 1.0}));
    EXPECT_TRUE(clocks[1].Configure({0.25, 2u, 2.0}));
    (void)clocks[0].Advance(0.35);
    (void)clocks[1].Advance(0.625);

    /** 二つの時計からまとめて取得する保存値。 */
    FFixedStepClockSnapshot snapshots[2] = {};

    /** 保存成功時に確定する件数。 */
    u32 snapshot_count = 71u;
    EXPECT_TRUE(TryCaptureFixedStepClockSnapshots(clocks, 2u, snapshots, 2u, snapshot_count));
    EXPECT_EQ(snapshot_count, 2u);

    (void)clocks[0].Advance(0.4);
    (void)clocks[1].Advance(0.5);
    EXPECT_TRUE(TryRestoreFixedStepClockSnapshots(clocks, snapshots, 2u));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[0]), snapshots[0]));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[1]), snapshots[1]));

    /** 不正復元を拒否する前の一つ目の時計状態。 */
    const FFixedStepClockSnapshot first_before = Capture(clocks[0]);

    /** 不正復元を拒否する前の二つ目の時計状態。 */
    const FFixedStepClockSnapshot second_before = Capture(clocks[1]);

    /** 二件目だけ持ち越し秒が範囲外の保存値列。 */
    FFixedStepClockSnapshot invalid_snapshots[2] = {snapshots[0], snapshots[1]};
    invalid_snapshots[1].accumulated_seconds = invalid_snapshots[1].options.step_seconds;
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(clocks, invalid_snapshots, 2u));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[0]), first_before));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[1]), second_before));

    /** 容量不足時に保持する保存先の番兵値。 */
    FFixedStepClockSnapshot unchanged_snapshot = {{0.5, 1u, 0.5}, 0.25, 3.0, 9u};

    /** 容量不足前の保存先を固定する値。 */
    const FFixedStepClockSnapshot expected_unchanged = unchanged_snapshot;
    snapshot_count = 63u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(clocks, 2u, &unchanged_snapshot, 1u, snapshot_count));
    EXPECT_EQ(snapshot_count, 63u);
    EXPECT_TRUE(SameSnapshot(unchanged_snapshot, expected_unchanged));

    snapshot_count = 52u;
    EXPECT_TRUE(TryCaptureFixedStepClockSnapshots(nullptr, 0u, nullptr, 0u, snapshot_count));
    EXPECT_EQ(snapshot_count, 0u);
    EXPECT_TRUE(TryRestoreFixedStepClockSnapshots(nullptr, nullptr, 0u));

    /** 時計領域を保存先として誤指定する重複配列。 */
    FFixedStepClockSnapshot* const overlapping_snapshots = reinterpret_cast<FFixedStepClockSnapshot*>(clocks);
    snapshot_count = 41u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(clocks, 2u, overlapping_snapshots, 2u, snapshot_count));
    EXPECT_EQ(snapshot_count, 41u);
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(clocks, overlapping_snapshots, 2u));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[0]), first_before));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[1]), second_before));

    /** 複数保存の時計入力を整列から一バイトずらす領域。 */
    alignas(CFixedStepClock) unsigned char misaligned_clock_storage[sizeof(CFixedStepClock) + alignof(CFixedStepClock)] = {};

    /** 意図的に整列を崩した複数保存の時計入力。 */
    const CFixedStepClock* const misaligned_clocks = reinterpret_cast<const CFixedStepClock*>(misaligned_clock_storage + 1u);
    snapshot_count = 31u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(misaligned_clocks, 1u, &unchanged_snapshot, 1u, snapshot_count));
    EXPECT_EQ(snapshot_count, 31u);
    EXPECT_TRUE(SameSnapshot(unchanged_snapshot, expected_unchanged));

    /** 複数復元の保存値入力を整列から一バイトずらす領域。 */
    alignas(FFixedStepClockSnapshot) unsigned char misaligned_snapshot_storage[sizeof(FFixedStepClockSnapshot) + alignof(FFixedStepClockSnapshot)] = {};

    /** 意図的に整列を崩した複数復元の保存値入力。 */
    const FFixedStepClockSnapshot* const misaligned_input = reinterpret_cast<const FFixedStepClockSnapshot*>(misaligned_snapshot_storage + 1u);
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(clocks, misaligned_input, 1u));
    EXPECT_TRUE(SameSnapshot(Capture(clocks[0]), first_before));
}

ACS_TEST(FixedStepClock, AcceptsMaximumBatchAndSaturatesStatistics)
{
    /** 最大件数一括処理で使う経過秒列。 */
    static f64 deltas[kMaximumFixedStepBatchCount] = {};

    /** 最大件数一括処理で使う結果列。 */
    static FFixedStepAdvanceResult results[kMaximumFixedStepBatchCount] = {};

    /** 最大件数を一括処理する時計。 */
    CFixedStepClock clock{};

    /** 最大件数処理後に確定する結果件数。 */
    u32 result_count = 0u;
    EXPECT_TRUE(clock.TryAdvanceBatch(deltas, kMaximumFixedStepBatchCount, results, kMaximumFixedStepBatchCount, result_count));
    EXPECT_EQ(result_count, kMaximumFixedStepBatchCount);

    /** 最大件数の複数保存で使う時計列。 */
    static CFixedStepClock clocks[kMaximumFixedStepBatchCount] = {};

    /** 最大件数の複数保存で使う保存値列。 */
    static FFixedStepClockSnapshot snapshots[kMaximumFixedStepBatchCount] = {};

    /** 最大件数の複数保存後に確定する件数。 */
    u32 snapshot_count = 0u;
    EXPECT_TRUE(TryCaptureFixedStepClockSnapshots(clocks, kMaximumFixedStepBatchCount, snapshots, kMaximumFixedStepBatchCount, snapshot_count));
    EXPECT_EQ(snapshot_count, kMaximumFixedStepBatchCount);
    EXPECT_TRUE(TryRestoreFixedStepClockSnapshots(clocks, snapshots, kMaximumFixedStepBatchCount));

    snapshot_count = 29u;
    EXPECT_FALSE(TryCaptureFixedStepClockSnapshots(clocks, kMaximumFixedStepBatchCount + 1u, snapshots, kMaximumFixedStepBatchCount + 1u, snapshot_count));
    EXPECT_EQ(snapshot_count, 29u);
    EXPECT_FALSE(TryRestoreFixedStepClockSnapshots(clocks, snapshots, kMaximumFixedStepBatchCount + 1u));

    /** 累積値の飽和境界を作る保存値。 */
    FFixedStepClockSnapshot saturated = {{0.1, 1u, 1.0}, 0.0, std::numeric_limits<f64>::max(), std::numeric_limits<u64>::max() - 1u};
    EXPECT_TRUE(clock.TryRestoreSnapshot(saturated));

    /** 更新回数と破棄秒をともに飽和させる入力結果。 */
    const FFixedStepAdvanceResult advance = clock.Advance(0.3);
    EXPECT_EQ(advance.step_count, 1u);
    EXPECT_TRUE(advance.was_clamped);
    EXPECT_EQ(clock.TotalStepCount(), std::numeric_limits<u64>::max());
    EXPECT_EQ(clock.TotalDroppedSeconds(), std::numeric_limits<f64>::max());
}
