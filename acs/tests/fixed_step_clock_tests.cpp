// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/FixedStepClock.h"

#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

/** テストの前提を整え、検証結果を呼び出し元へ渡す。
 * @param first 補助処理の分岐または比較に使う入力値。
 * @param second 補助処理の分岐または比較に使う入力値。
 */
bool SameState(const FFixedStepClockSnapshot& first, const FFixedStepClockSnapshot& second) noexcept {
    return first.options.step_seconds == second.options.step_seconds
        && first.options.maximum_steps_per_advance
            == second.options.maximum_steps_per_advance
        && first.options.maximum_accumulated_seconds
            == second.options.maximum_accumulated_seconds
        && first.accumulated_seconds == second.accumulated_seconds
        && first.total_dropped_seconds == second.total_dropped_seconds
        && first.total_step_count == second.total_step_count;
}

} // namespace

ACS_TEST(FixedStepClock, ProducesBoundedStepsAndInterpolation) {
    /** FFixedStepClock の初期状態を境界条件として検証する値。 */
    FFixedStepClock clock;
    /** FFixedStepOptions の初期状態を境界条件として検証する値。 */
    FFixedStepOptions options{};
    options.step_seconds = 0.1;
    options.maximum_steps_per_advance = 2u;
    options.maximum_accumulated_seconds = 1.0;
    EXPECT_TRUE(clock.Configure(options));

    /** 順序比較に使う一つ目の値。 */
    const FFixedStepAdvanceResult first = clock.Advance(0.25);
    EXPECT_TRUE(first.accepted);
    EXPECT_EQ(first.step_count, 2u);
    EXPECT_FALSE(first.was_clamped);
    EXPECT_TRUE(first.interpolation_alpha > 0.49);
    EXPECT_TRUE(first.interpolation_alpha < 0.51);

    /** 「Advance」呼び出しから得た状態を期待条件と照合する。 */
    const FFixedStepAdvanceResult limited = clock.Advance(0.50);
    EXPECT_TRUE(limited.accepted);
    EXPECT_EQ(limited.step_count, 2u);
    EXPECT_TRUE(limited.was_clamped);
    EXPECT_TRUE(limited.dropped_seconds > 0.29);
}

ACS_TEST(FixedStepClock, RejectsInvalidDeltaWithoutChangingState) {
    /** FFixedStepClock の初期状態を境界条件として検証する値。 */
    FFixedStepClock clock;
    EXPECT_TRUE(clock.Advance(0.01).accepted);

    /** 処理前の状態を保持する比較基準。 */
    FFixedStepClockSnapshot before{};
    EXPECT_TRUE(clock.TryCaptureSnapshot(before));

    /** 正常系と失敗系を選び分ける検証値。 */
    const f64 invalid_values[] = {-1.0, std::numeric_limits<f64>::infinity(), -std::numeric_limits<f64>::infinity(), std::numeric_limits<f64>::quiet_NaN(),};
    for (/* 「value」へ保存した実測値を直後の期待値と照合する。 */ const f64 value : invalid_values) {
        /** 「Advance」呼び出しの戻り値を期待条件と照合する。 */
        const FFixedStepAdvanceResult result = clock.Advance(value);
        EXPECT_FALSE(result.accepted);
        /** 処理後の状態を保持する観測結果。 */
        FFixedStepClockSnapshot after{};
        EXPECT_TRUE(clock.TryCaptureSnapshot(after));
        EXPECT_TRUE(SameState(before, after));
    }
}

ACS_TEST(FixedStepClock, ClampsHugeFiniteDeltaAndMaximumStepCount) {
    /** FFixedStepClock の初期状態を境界条件として検証する値。 */
    FFixedStepClock clock;
    /** FFixedStepOptions の初期状態を境界条件として検証する値。 */
    FFixedStepOptions options{};
    options.step_seconds = 0.001;
    options.maximum_steps_per_advance = 3u;
    options.maximum_accumulated_seconds = 0.010;
    EXPECT_TRUE(clock.Configure(options));

    /** 「max」呼び出しの戻り値を期待条件と照合する。 */
    const FFixedStepAdvanceResult result =
        clock.Advance(std::numeric_limits<f64>::max());
    EXPECT_TRUE(result.accepted);
    EXPECT_TRUE(result.was_clamped);
    EXPECT_EQ(result.step_count, 3u);
    EXPECT_TRUE(result.dropped_seconds > 0.0);
    EXPECT_EQ(clock.TotalStepCount(), 3u);
}

ACS_TEST(FixedStepClock, InvalidConfigurationAndSnapshotPreserveState) {
    /** FFixedStepClock の初期状態を境界条件として検証する値。 */
    FFixedStepClock clock;
    EXPECT_TRUE(clock.Advance(0.01).accepted);
    /** 処理前の状態を保持する比較基準。 */
    FFixedStepClockSnapshot before{};
    EXPECT_TRUE(clock.TryCaptureSnapshot(before));

    /** 正常系と失敗系を選び分ける検証値。 */
    FFixedStepOptions invalid_options = before.options;
    invalid_options.maximum_steps_per_advance = 0u;
    EXPECT_FALSE(clock.Configure(invalid_options));

    /** 正常系と失敗系を選び分ける検証値。 */
    FFixedStepClockSnapshot invalid_snapshot = before;
    invalid_snapshot.accumulated_seconds = invalid_snapshot.options.step_seconds;
    EXPECT_FALSE(clock.TryRestoreSnapshot(invalid_snapshot));

    /** 処理後の状態を保持する観測結果。 */
    FFixedStepClockSnapshot after{};
    EXPECT_TRUE(clock.TryCaptureSnapshot(after));
    EXPECT_TRUE(SameState(before, after));
}

ACS_TEST(FixedStepClock, SnapshotRoundTripRestoresProgressAndStatistics) {
    /** 複製または変換の入力となる値。 */
    FFixedStepClock source;
    /** FFixedStepOptions の初期状態を境界条件として検証する値。 */
    FFixedStepOptions options{};
    options.step_seconds = 0.1;
    options.maximum_steps_per_advance = 1u;
    options.maximum_accumulated_seconds = 0.5;
    EXPECT_TRUE(source.Configure(options));
    EXPECT_TRUE(source.Advance(0.35).accepted);

    /** FFixedStepClockSnapshot の初期状態を境界条件として検証する値。 */
    FFixedStepClockSnapshot saved{};
    EXPECT_TRUE(source.TryCaptureSnapshot(saved));

    /** 初期化または復元操作から得た状態を期待値と照合する。 */
    FFixedStepClock restored;
    EXPECT_TRUE(restored.TryRestoreSnapshot(saved));
    /** FFixedStepClockSnapshot の初期状態を境界条件として検証する値。 */
    FFixedStepClockSnapshot round_trip{};
    EXPECT_TRUE(restored.TryCaptureSnapshot(round_trip));
    EXPECT_TRUE(SameState(saved, round_trip));

    restored.Reset();
    EXPECT_EQ(restored.Options().step_seconds, options.step_seconds);
    EXPECT_EQ(restored.TotalStepCount(), 0u);
    EXPECT_EQ(restored.TotalDroppedSeconds(), 0.0);
}
