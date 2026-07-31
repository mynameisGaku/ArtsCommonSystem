// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::timing {

/** 固定更新の刻み幅と一回の処理上限を指定する値。 */
struct FFixedStepOptions {
    /** 一回の固定更新が進める秒数。 */
    f64 step_seconds = 1.0 / 60.0;

    /** 一回の入力で実行できる固定更新の最大回数。 */
    u32 maximum_steps_per_advance = 8u;

    /** 一回の入力で受け付ける経過秒の上限。 */
    f64 maximum_accumulated_seconds = 0.25;
};

} // namespace acs::timing
