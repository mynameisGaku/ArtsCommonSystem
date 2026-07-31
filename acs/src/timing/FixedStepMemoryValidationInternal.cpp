// SPDX-License-Identifier: Apache-2.0
#include "timing/FixedStepMemoryValidationInternal.h"

#include "timing/FixedStepClock.h"

#include <limits>

namespace acs::timing::detail {

bool TryMakeFixedStepMemoryRange(const void* pointer, usize element_size, u32 count, usize alignment, FFixedStepMemoryRangeInternal& range) noexcept
{
    if (count == 0u) {
        range = {};
        return true;
    }
    if (!pointer || element_size == 0u || alignment == 0u) return false;

    /** 検証対象領域の先頭アドレス。 */
    const std::uintptr_t address = reinterpret_cast<std::uintptr_t>(pointer);
    if (address % static_cast<std::uintptr_t>(alignment) != 0u) return false;

    /** アドレス計算で扱える最大値。 */
    constexpr std::uintptr_t maximum = std::numeric_limits<std::uintptr_t>::max();

    /** 一要素のバイト数。 */
    const std::uintptr_t element_bytes = static_cast<std::uintptr_t>(element_size);

    /** 配列として検証する要素数。 */
    const std::uintptr_t element_count = static_cast<std::uintptr_t>(count);
    if (element_count > maximum / element_bytes) return false;

    /** 検証対象領域の総バイト数。 */
    const std::uintptr_t byte_count = element_count * element_bytes;
    if (address > maximum - byte_count) return false;

    range = {address, address + byte_count};
    return true;
}

bool FixedStepMemoryRangesOverlap(const FFixedStepMemoryRangeInternal& first, const FFixedStepMemoryRangeInternal& second) noexcept
{
    if (first.begin == first.end || second.begin == second.end) return false;
    return first.begin < second.end && second.begin < first.end;
}

bool TryValidateFixedStepSnapshotMemory(const void* clock_address, const void* snapshot_address, usize clock_size, usize clock_alignment, usize snapshot_size, usize snapshot_alignment) noexcept
{
    /** 時計本体が占める領域。 */
    FFixedStepMemoryRangeInternal clock_range{};

    /** 保存値が占める領域。 */
    FFixedStepMemoryRangeInternal snapshot_range{};
    if (!TryMakeFixedStepMemoryRange(clock_address, clock_size, 1u, clock_alignment, clock_range) || !TryMakeFixedStepMemoryRange(snapshot_address, snapshot_size, 1u, snapshot_alignment, snapshot_range)) {
        return false;
    }
    return !FixedStepMemoryRangesOverlap(clock_range, snapshot_range);
}

bool TryValidateFixedStepAdvanceBatchMemory(const void* clock_address, usize clock_size, usize clock_alignment, const void* delta_address, u32 count, const void* result_address, u32 result_capacity, usize result_size, usize result_alignment, const void* result_count_address) noexcept
{
    /** 結果件数の出力領域。 */
    FFixedStepMemoryRangeInternal result_count_range{};

    /** 時計本体が占める領域。 */
    FFixedStepMemoryRangeInternal clock_range{};
    if (!TryMakeFixedStepMemoryRange(result_count_address, sizeof(u32), 1u, alignof(u32), result_count_range) || !TryMakeFixedStepMemoryRange(clock_address, clock_size, 1u, clock_alignment, clock_range) || count > kMaximumFixedStepBatchCount || result_capacity < count) {
        return false;
    }

    /** 経過秒配列が占める領域。 */
    FFixedStepMemoryRangeInternal delta_range{};

    /** 結果配列の容量全体が占める領域。 */
    FFixedStepMemoryRangeInternal result_range{};
    if (!TryMakeFixedStepMemoryRange(delta_address, sizeof(f64), count, alignof(f64), delta_range) || !TryMakeFixedStepMemoryRange(result_address, result_size, result_capacity, result_alignment, result_range)) {
        return false;
    }

    return !FixedStepMemoryRangesOverlap(clock_range, delta_range) &&
           !FixedStepMemoryRangesOverlap(clock_range, result_range) &&
           !FixedStepMemoryRangesOverlap(clock_range, result_count_range) &&
           !FixedStepMemoryRangesOverlap(delta_range, result_range) &&
           !FixedStepMemoryRangesOverlap(delta_range, result_count_range) &&
           !FixedStepMemoryRangesOverlap(result_range, result_count_range);
}

bool TryValidateFixedStepSnapshotCaptureBatchMemory(const void* clock_address, u32 count, usize clock_size, usize clock_alignment, const void* snapshot_address, u32 snapshot_capacity, usize snapshot_size, usize snapshot_alignment, const void* snapshot_count_address) noexcept
{
    /** 保存件数の出力領域。 */
    FFixedStepMemoryRangeInternal snapshot_count_range{};
    if (!TryMakeFixedStepMemoryRange(snapshot_count_address, sizeof(u32), 1u, alignof(u32), snapshot_count_range) || count > kMaximumFixedStepBatchCount || snapshot_capacity < count) {
        return false;
    }

    /** 複数時計が占める領域。 */
    FFixedStepMemoryRangeInternal clock_range{};

    /** 保存先配列の容量全体が占める領域。 */
    FFixedStepMemoryRangeInternal snapshot_range{};
    if (!TryMakeFixedStepMemoryRange(clock_address, clock_size, count, clock_alignment, clock_range) || !TryMakeFixedStepMemoryRange(snapshot_address, snapshot_size, snapshot_capacity, snapshot_alignment, snapshot_range)) {
        return false;
    }

    return !FixedStepMemoryRangesOverlap(clock_range, snapshot_range) &&
           !FixedStepMemoryRangesOverlap(clock_range, snapshot_count_range) &&
           !FixedStepMemoryRangesOverlap(snapshot_range, snapshot_count_range);
}

bool TryValidateFixedStepSnapshotRestoreBatchMemory(const void* clock_address, const void* snapshot_address, u32 count, usize clock_size, usize clock_alignment, usize snapshot_size, usize snapshot_alignment) noexcept
{
    if (count > kMaximumFixedStepBatchCount) return false;

    /** 復元先の複数時計が占める領域。 */
    FFixedStepMemoryRangeInternal clock_range{};

    /** 復元元の保存値配列が占める領域。 */
    FFixedStepMemoryRangeInternal snapshot_range{};
    if (!TryMakeFixedStepMemoryRange(clock_address, clock_size, count, clock_alignment, clock_range) || !TryMakeFixedStepMemoryRange(snapshot_address, snapshot_size, count, snapshot_alignment, snapshot_range)) {
        return false;
    }
    return !FixedStepMemoryRangesOverlap(clock_range, snapshot_range);
}

} // namespace acs::timing::detail
