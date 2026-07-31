// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::timing {

/** 一回の経過入力から確定した固定更新回数と補間状態。 */
struct FFixedStepAdvanceResult {
    /** 実行対象として確定した固定更新の回数。 */
    u32 step_count = 0u;

    /** 次の固定更新までの進み具合。 */
    f64 interpolation_alpha = 0.0;

    /** 上限によって破棄した経過秒。 */
    f64 dropped_seconds = 0.0;

    /** 入力値を受け付けた場合に真となる値。 */
    bool accepted = false;

    /** 蓄積量または実行回数の上限を適用した場合に真となる値。 */
    bool was_clamped = false;
};

} // namespace acs::timing
