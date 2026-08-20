// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 1回の Advance が確定した固定更新回数と描画補間情報。 */
struct FFixedStepAdvanceResult {
    /** 呼び出し側が今回実行する固定更新回数。 */
    u32 step_count = 0u;

    /** 次の固定 step までの剰余率。 */
    f64 interpolation_alpha = 0.0;

    /** 蓄積上限または実行回数上限によって破棄した秒数。 */
    f64 dropped_seconds = 0.0;

    /** 入力 delta が有限かつ 0 以上で受理されたか。 */
    bool accepted = false;

    /** 今回、時間を少しでも破棄して上限処理を行ったか。 */
    bool was_clamped = false;
};

} // namespace acs::game
