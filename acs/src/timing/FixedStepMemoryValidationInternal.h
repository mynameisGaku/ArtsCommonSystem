// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "timing/FixedStepMemoryRangeInternal.h"

namespace acs::timing::detail {

/**
 * 指定個数の生領域についてnull、整列、乗算、終端を検証する。
 *
 * countが0の場合はpointerを参照せず空区間を返す。
 */
bool TryMakeFixedStepMemoryRange(const void* pointer, usize element_size, u32 count, usize alignment, FFixedStepMemoryRangeInternal& range) noexcept;

/** 二つの半開区間が一バイト以上重なる場合に真を返す。 */
bool FixedStepMemoryRangesOverlap(const FFixedStepMemoryRangeInternal& first, const FFixedStepMemoryRangeInternal& second) noexcept;

/** 時計と保存先が有効な単一領域で、互いに重ならないことを検証する。 */
bool TryValidateFixedStepSnapshotMemory(const void* clock_address, const void* snapshot_address, usize clock_size, usize clock_alignment, usize snapshot_size, usize snapshot_alignment) noexcept;

/**
 * 一括経過入力の全領域、容量、重複を参照前に検証する。
 *
 * countが上限を超える場合やresult_capacityが不足する場合は失敗する。
 */
bool TryValidateFixedStepAdvanceBatchMemory(const void* clock_address, usize clock_size, usize clock_alignment, const void* delta_address, u32 count, const void* result_address, u32 result_capacity, usize result_size, usize result_alignment, const void* result_count_address) noexcept;

/**
 * 複数時計の保存処理に使う全領域、容量、重複を参照前に検証する。
 *
 * countが上限を超える場合やsnapshot_capacityが不足する場合は失敗する。
 */
bool TryValidateFixedStepSnapshotCaptureBatchMemory(const void* clock_address, u32 count, usize clock_size, usize clock_alignment, const void* snapshot_address, u32 snapshot_capacity, usize snapshot_size, usize snapshot_alignment, const void* snapshot_count_address) noexcept;

/** 複数時計の復元処理に使う入力領域と出力領域が重ならないことを検証する。 */
bool TryValidateFixedStepSnapshotRestoreBatchMemory(const void* clock_address, const void* snapshot_address, u32 count, usize clock_size, usize clock_alignment, usize snapshot_size, usize snapshot_alignment) noexcept;

} // namespace acs::timing::detail
