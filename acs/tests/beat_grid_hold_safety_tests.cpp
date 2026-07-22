// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/BeatGrid.h"
#include "memory/SystemAllocator.h"

#include <cfloat>
#include <cstring>
#include <limits>

using namespace acs;
using namespace acs::game;

namespace {

struct FBeatEvents {
    u32 judge_count = 0u;
    u32 end_count = 0u;
    EBeatLane last_lane = EBeatLane::Left;
    EJudgement last_judgement = EJudgement::Miss;
    u32 last_combo = 0u;
    u32 end_hits = 0u;
    u32 end_misses = 0u;
};

void RecordJudge(
    void* user, EBeatLane lane, EJudgement judgement, u32 combo) noexcept {
    auto& events = *static_cast<FBeatEvents*>(user);
    ++events.judge_count;
    events.last_lane = lane;
    events.last_judgement = judgement;
    events.last_combo = combo;
}

void RecordEnd(
    void* user, u32 hits, u32 misses, f32 /*accuracy*/) noexcept {
    auto& events = *static_cast<FBeatEvents*>(user);
    ++events.end_count;
    events.end_hits = hits;
    events.end_misses = misses;
}

class FSwitchableBeatAllocator final : public FAllocator {
public:
    explicit FSwitchableBeatAllocator(FAllocator& backing) noexcept
        : m_Backing(&backing) {
    }

    void SetFailing(bool failing) noexcept { m_Failing = failing; }

    void* Alloc(
        usize size, usize alignment, FSourceLoc location) noexcept override {
        return m_Failing
            ? nullptr
            : m_Backing->Alloc(size, alignment, location);
    }

    void Free(void* pointer) noexcept override {
        m_Backing->Free(pointer);
    }

private:
    FAllocator* m_Backing = nullptr;
    bool m_Failing = false;
};

struct FReentrantClearContext {
    FBeatGrid* grid = nullptr;
    u32 calls = 0u;
};

void ClearFromJudge(
    void* user, EBeatLane, EJudgement, u32) noexcept {
    auto& context = *static_cast<FReentrantClearContext*>(user);
    ++context.calls;
    context.grid->ClearAll();
}

} // namespace

ACS_TEST(BeatGridHold, NormalTapRetainsImmediateExactOnceSemantics) {
    FBeatGrid grid;
    FBeatEvents events;
    grid.SetOnJudgeCallback(&RecordJudge, &events);
    grid.SetOnEndCallback(&RecordEnd, &events);
    const FBeatNote note{0.1f, EBeatLane::Left, false, 0.0f};

    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 120.0f),
        EBeatChartLoadResult::Success);
    grid.Start();
    grid.Tick(0.1f);
    EXPECT_EQ(grid.Tap(EBeatLane::Left), EJudgement::Perfect);
    EXPECT_EQ(grid.HitNotes(), 1u);
    EXPECT_EQ(grid.CurrentCombo(), 1u);
    EXPECT_EQ(events.judge_count, 1u);
    EXPECT_EQ(events.end_count, 0u);

    EXPECT_EQ(grid.Tap(EBeatLane::Left), EJudgement::Miss);
    grid.Tick(1.0f);
    EXPECT_EQ(events.judge_count, 1u);
    EXPECT_EQ(events.end_count, 1u);
}

ACS_TEST(BeatGridHold, HoldScoresOnlyAfterSuccessfulTailRelease) {
    FBeatGrid grid;
    FBeatEvents events;
    grid.SetOnJudgeCallback(&RecordJudge, &events);
    grid.SetOnEndCallback(&RecordEnd, &events);
    const FBeatNote note{0.1f, EBeatLane::Up, true, 0.5f};

    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 120.0f),
        EBeatChartLoadResult::Success);
    grid.Start();
    grid.Tick(0.1f);
    EXPECT_EQ(grid.PressLane(EBeatLane::Up), EJudgement::Perfect);
    EXPECT_TRUE(grid.IsLaneHolding(EBeatLane::Up));
    EXPECT_EQ(grid.ActiveHoldCount(), 1u);
    EXPECT_EQ(grid.HitNotes(), 0u);
    EXPECT_EQ(events.judge_count, 0u);

    grid.Tick(0.5f);
    EXPECT_EQ(grid.ReleaseLane(EBeatLane::Up), EJudgement::Perfect);
    EXPECT_FALSE(grid.IsLaneHolding(EBeatLane::Up));
    EXPECT_EQ(grid.ActiveHoldCount(), 0u);
    EXPECT_EQ(grid.HitNotes(), 1u);
    EXPECT_EQ(events.judge_count, 1u);
    EXPECT_EQ(events.last_judgement, EJudgement::Perfect);
    EXPECT_EQ(events.end_count, 0u);

    EXPECT_EQ(grid.ReleaseLane(EBeatLane::Up), EJudgement::Miss);
    EXPECT_EQ(events.judge_count, 1u);
    grid.Tick(0.001f);
    EXPECT_EQ(events.end_count, 1u);
}

ACS_TEST(BeatGridHold, WorseOfHeadAndTailDeterminesFinalJudgement) {
    FBeatGrid grid;
    const FBeatNote note{0.1f, EBeatLane::Down, true, 0.5f};
    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 100.0f),
        EBeatChartLoadResult::Success);

    grid.Start();
    grid.Tick(0.14f);
    EXPECT_EQ(grid.PressLane(EBeatLane::Down), EJudgement::Great);
    grid.Tick(0.46f);
    EXPECT_EQ(grid.ReleaseLane(EBeatLane::Down), EJudgement::Great);
    EXPECT_EQ(grid.HitNotes(), 1u);
    EXPECT_NEAR(grid.Accuracy(), 0.8f, 0.0001f);
}

ACS_TEST(BeatGridHold, EarlyReleaseIsImmediateSingleMiss) {
    FBeatGrid grid;
    FBeatEvents events;
    grid.SetOnJudgeCallback(&RecordJudge, &events);
    const FBeatNote note{0.0f, EBeatLane::Right, true, 1.0f};
    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 100.0f),
        EBeatChartLoadResult::Success);

    grid.Start();
    EXPECT_EQ(grid.PressLane(EBeatLane::Right), EJudgement::Perfect);
    grid.Tick(0.2f);
    EXPECT_EQ(grid.ReleaseLane(EBeatLane::Right), EJudgement::Miss);
    EXPECT_EQ(grid.MissedNotes(), 1u);
    EXPECT_EQ(events.judge_count, 1u);
    EXPECT_EQ(events.last_combo, 0u);

    grid.Tick(2.0f);
    EXPECT_EQ(grid.MissedNotes(), 1u);
    EXPECT_EQ(events.judge_count, 1u);
}

ACS_TEST(BeatGridHold, UnreleasedHoldTimesOutOncePastTailWindow) {
    FBeatGrid grid;
    FBeatEvents events;
    grid.SetOnJudgeCallback(&RecordJudge, &events);
    const FBeatNote note{0.0f, EBeatLane::Custom1, true, 1.0f};
    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 100.0f),
        EBeatChartLoadResult::Success);

    grid.Start();
    EXPECT_EQ(grid.PressLane(EBeatLane::Custom1), EJudgement::Perfect);
    grid.Tick(1.101f);
    EXPECT_FALSE(grid.IsLaneHolding(EBeatLane::Custom1));
    EXPECT_EQ(grid.MissedNotes(), 1u);
    EXPECT_EQ(events.judge_count, 1u);

    grid.Tick(1.0f);
    EXPECT_EQ(grid.MissedNotes(), 1u);
    EXPECT_EQ(events.judge_count, 1u);
}

ACS_TEST(BeatGridHold, EqualDistancePressUsesLowestChartIndex) {
    FBeatGrid grid;
    const FBeatNote notes[] = {
        {0.0f, EBeatLane::Left, true, 0.5f},
        {0.0f, EBeatLane::Left, false, 0.0f},
    };
    EXPECT_EQ(
        grid.TryLoadChart(notes, 2u, 120.0f),
        EBeatChartLoadResult::Success);

    grid.Start();
    EXPECT_EQ(grid.PressLane(EBeatLane::Left), EJudgement::Perfect);
    EXPECT_TRUE(grid.IsLaneHolding(EBeatLane::Left));
    EXPECT_EQ(grid.HitNotes(), 0u);
    EXPECT_EQ(grid.PressLane(EBeatLane::Left), EJudgement::Perfect);
    EXPECT_EQ(grid.HitNotes(), 1u);
}

ACS_TEST(BeatGridSafety, CheckedLoadRejectsInvalidValuesTransactionally) {
    FBeatGrid grid;
    const FBeatNote original{1.0f, EBeatLane::Down, false, 0.0f};
    EXPECT_EQ(
        grid.TryLoadChart(&original, 1u, 90.0f),
        EBeatChartLoadResult::Success);
    grid.Start();
    grid.Tick(0.2f);

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    FBeatNote bad = original;
    bad.time_sec = nan;
    EXPECT_EQ(
        grid.TryLoadChart(&bad, 1u, 120.0f),
        EBeatChartLoadResult::InvalidNoteTime);
    EXPECT_EQ(grid.TotalNotes(), 1u);
    EXPECT_NEAR(grid.Bpm(), 90.0f, 0.0001f);
    EXPECT_TRUE(grid.IsPlaying());
    EXPECT_NEAR(grid.CurrentTimeSec(), 0.2f, 0.0001f);

    bad = original;
    bad.lane = static_cast<EBeatLane>(kBeatLaneCount);
    EXPECT_EQ(
        grid.TryLoadChart(&bad, 1u, 120.0f),
        EBeatChartLoadResult::InvalidLane);
    EXPECT_EQ(
        grid.TryLoadChart(nullptr, 1u, 120.0f),
        EBeatChartLoadResult::NullNotes);
    EXPECT_EQ(
        grid.TryLoadChart(
            &original, kMaxBeatChartNotes + 1u, 120.0f),
        EBeatChartLoadResult::TooManyNotes);

    bad = original;
    bad.is_hold = true;
    bad.hold_duration_sec = 0.0f;
    EXPECT_EQ(
        grid.TryLoadChart(&bad, 1u, 120.0f),
        EBeatChartLoadResult::InvalidHoldDuration);
    EXPECT_EQ(
        grid.TryLoadChart(&original, 1u, nan),
        EBeatChartLoadResult::InvalidBpm);
    EXPECT_EQ(grid.TotalNotes(), 1u);
    EXPECT_NEAR(grid.Bpm(), 90.0f, 0.0001f);
    EXPECT_TRUE(grid.IsPlaying());
    EXPECT_NEAR(grid.CurrentTimeSec(), 0.2f, 0.0001f);
}

ACS_TEST(BeatGridSafety, AllocationFailurePreservesExistingChart) {
    FSystemAllocator backing;
    FSwitchableBeatAllocator allocator(backing);
    FBeatGrid grid(allocator);
    const FBeatNote original{0.0f, EBeatLane::Up, false, 0.0f};
    const FBeatNote replacement[] = {
        {0.0f, EBeatLane::Left, false, 0.0f},
        {1.0f, EBeatLane::Right, false, 0.0f},
    };
    EXPECT_EQ(
        grid.TryLoadChart(&original, 1u, 80.0f),
        EBeatChartLoadResult::Success);
    grid.Start();

    allocator.SetFailing(true);
    EXPECT_EQ(
        grid.TryLoadChart(replacement, 2u, 160.0f),
        EBeatChartLoadResult::OutOfMemory);
    EXPECT_EQ(grid.TotalNotes(), 1u);
    EXPECT_NEAR(grid.Bpm(), 80.0f, 0.0001f);
    EXPECT_TRUE(grid.IsPlaying());

    EXPECT_EQ(grid.Tap(EBeatLane::Up), EJudgement::Perfect);
    EXPECT_EQ(grid.HitNotes(), 1u);
}

ACS_TEST(BeatGridSafety, InvalidAndOverflowingDeltaTimeIsNonMutating) {
    FBeatGrid grid;
    const FBeatNote note{1.0f, EBeatLane::Left, false, 0.0f};
    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 120.0f),
        EBeatChartLoadResult::Success);
    grid.Start();

    const f32 nan = std::numeric_limits<f32>::quiet_NaN();
    const f32 infinity = std::numeric_limits<f32>::infinity();
    grid.Tick(nan);
    grid.Tick(infinity);
    grid.Tick(-1.0f);
    EXPECT_NEAR(grid.CurrentTimeSec(), 0.0f, 0.0f);
    EXPECT_EQ(grid.MissedNotes(), 0u);

    grid.Tick(FLT_MAX);
    const f32 before = grid.CurrentTimeSec();
    EXPECT_TRUE(before > 0.0f);
    grid.Tick(FLT_MAX);
    EXPECT_EQ(grid.CurrentTimeSec(), before);
}

ACS_TEST(BeatGridSafety, CheckedTimingWindowsAreTransactional) {
    FBeatGrid grid;
    EXPECT_TRUE(grid.TrySetTimingWindows(10.0f, 20.0f, 30.0f));
    EXPECT_FALSE(grid.TrySetTimingWindows(20.0f, 10.0f, 30.0f));
    EXPECT_FALSE(grid.TrySetTimingWindows(
        10.0f, 20.0f, std::numeric_limits<f32>::quiet_NaN()));

    const FBeatNote note{0.025f, EBeatLane::Left, false, 0.0f};
    EXPECT_EQ(
        grid.TryLoadChart(&note, 1u, 120.0f),
        EBeatChartLoadResult::Success);
    grid.Start();
    EXPECT_EQ(grid.Tap(EBeatLane::Left), EJudgement::Good);
}

ACS_TEST(BeatGridSafety, JudgeCallbackMayClearGridWithoutStaleLoopAccess) {
    FBeatGrid grid;
    FReentrantClearContext context{&grid, 0u};
    grid.SetOnJudgeCallback(&ClearFromJudge, &context);
    const FBeatNote notes[] = {
        {0.0f, EBeatLane::Left, false, 0.0f},
        {0.0f, EBeatLane::Right, false, 0.0f},
    };
    EXPECT_EQ(
        grid.TryLoadChart(notes, 2u, 120.0f),
        EBeatChartLoadResult::Success);

    grid.Start();
    grid.Tick(1.0f);
    EXPECT_EQ(context.calls, 1u);
    EXPECT_EQ(grid.TotalNotes(), 0u);
    EXPECT_FALSE(grid.IsPlaying());
}

ACS_TEST(BeatGridSafety, LoadResultNamesAreStable) {
    EXPECT_TRUE(std::strcmp(
        BeatChartLoadResultName(EBeatChartLoadResult::Success),
        "Success") == 0);
    EXPECT_TRUE(std::strcmp(
        BeatChartLoadResultName(EBeatChartLoadResult::OutOfMemory),
        "OutOfMemory") == 0);
    EXPECT_TRUE(std::strcmp(
        BeatChartLoadResultName(static_cast<EBeatChartLoadResult>(255u)),
        "Unknown") == 0);
}
