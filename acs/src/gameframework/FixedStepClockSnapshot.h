// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/FixedStepOptions.h"

namespace acs::game {

/** 固定ステップ時計の設定、再生位置、累積統計を所有する保存値。 */
struct FFixedStepClockSnapshot {
    /** 保存時に使われていた固定時計の設定。 */
    FFixedStepOptions options{};

    /** 次の固定 step へ繰り越す 1 step 未満の秒数。 */
    f64 accumulated_seconds = 0.0;

    /** 上限処理によって破棄した秒数の累計。 */
    f64 total_dropped_seconds = 0.0;

    /** 時計が返した固定更新回数の累計。 */
    u64 total_step_count = 0u;
};

} // namespace acs::game
