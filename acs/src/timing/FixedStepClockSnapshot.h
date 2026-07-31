// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "timing/FixedStepOptions.h"

namespace acs::timing {

/** 固定更新時計の設定、補間位置、累積統計をまとめた保存値。 */
struct FFixedStepClockSnapshot {
    /** 保存時に時計へ適用されていた設定。 */
    FFixedStepOptions options{};

    /** 次の固定更新へ持ち越す経過秒。 */
    f64 accumulated_seconds = 0.0;

    /** 上限によって破棄した累積秒数。 */
    f64 total_dropped_seconds = 0.0;

    /** 時計が確定した固定更新の累積回数。 */
    u64 total_step_count = 0u;
};

} // namespace acs::timing
