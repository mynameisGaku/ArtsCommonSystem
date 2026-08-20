// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedStepClockSnapshot.h"
#include "gameframework/FixedStepInputBufferSnapshot.h"

namespace acs::game {

/** 固定更新時計と未消費入力を同じ復元境界で保持する保存値。 */
struct FFixedStepRuntimeSnapshot {
    /** 固定更新時計の設定と進行状態。 */
    FFixedStepClockSnapshot clock{};

    /** active scene が保持する未消費の固定入力。 */
    FFixedStepInputBufferSnapshot input{};

    /** 固定タイムステップ更新が有効だったか。 */
    bool fixed_step_enabled = true;
};

} // namespace acs::game
