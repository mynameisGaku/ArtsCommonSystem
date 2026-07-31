// SPDX-License-Identifier: Apache-2.0
#include "timing/FixedStepClockBatch.h"

#include "timing/FixedStepMemoryValidationInternal.h"

namespace acs::timing {

bool TryCaptureFixedStepClockSnapshots(const FFixedStepClock* clocks, u32 count, FFixedStepClockSnapshot* snapshots, u32 snapshot_capacity, u32& snapshot_count) noexcept
{
    if (!detail::TryValidateFixedStepSnapshotCaptureBatchMemory(clocks, count, sizeof(FFixedStepClock), alignof(FFixedStepClock), snapshots, snapshot_capacity, sizeof(FFixedStepClockSnapshot), alignof(FFixedStepClockSnapshot), &snapshot_count)) {
        return false;
    }
    if (count == 0u) {
        snapshot_count = 0u;
        return true;
    }

    /** 全時計の妥当性を出力前に検証する一時保存値。 */
    FFixedStepClockSnapshot candidate{};
    for (u32 index = 0u; index < count; ++index) {
        if (!clocks[index].TryCaptureSnapshot(candidate)) return false;
    }

    for (u32 index = 0u; index < count; ++index) {
        if (!clocks[index].TryCaptureSnapshot(candidate)) return false;
        snapshots[index] = candidate;
    }
    snapshot_count = count;
    return true;
}

bool TryRestoreFixedStepClockSnapshots(FFixedStepClock* clocks, const FFixedStepClockSnapshot* snapshots, u32 count) noexcept
{
    if (!detail::TryValidateFixedStepSnapshotRestoreBatchMemory(clocks, snapshots, count, sizeof(FFixedStepClock), alignof(FFixedStepClock), sizeof(FFixedStepClockSnapshot), alignof(FFixedStepClockSnapshot))) {
        return false;
    }
    if (count == 0u) return true;

    /** 復元前の全時計が妥当であることを確認する一時保存値。 */
    FFixedStepClockSnapshot current_state{};
    for (u32 index = 0u; index < count; ++index) {
        if (!clocks[index].TryCaptureSnapshot(current_state)) return false;
    }

    for (u32 index = 0u; index < count; ++index) {
        /** 対応する保存値の内容を事前検証する候補時計。 */
        FFixedStepClock candidate{};
        if (!candidate.TryRestoreSnapshot(snapshots[index])) return false;
    }

    for (u32 index = 0u; index < count; ++index) {
        /** 検証済みの保存値から確定状態を作る候補時計。 */
        FFixedStepClock candidate{};
        if (!candidate.TryRestoreSnapshot(snapshots[index])) return false;
        clocks[index] = candidate;
    }
    return true;
}

} // namespace acs::timing
