// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "timing/FixedStepClock.h"

namespace acs::timing {

/**
 * 複数時計の状態を指定配列へまとめて保存する。
 *
 * 入力、容量、整列、領域重複を先に検証し、失敗時は保存先と件数を変更しない。
 */
bool TryCaptureFixedStepClockSnapshots(const CFixedStepClock* clocks, u32 count, FFixedStepClockSnapshot* snapshots, u32 snapshot_capacity, u32& snapshot_count) noexcept;

/**
 * 複数の保存値を対応する時計へまとめて復元する。
 *
 * 全時計と全保存値を先に検証し、失敗時はどの時計も変更しない。
 */
bool TryRestoreFixedStepClockSnapshots(CFixedStepClock* clocks, const FFixedStepClockSnapshot* snapshots, u32 count) noexcept;

} // namespace acs::timing
