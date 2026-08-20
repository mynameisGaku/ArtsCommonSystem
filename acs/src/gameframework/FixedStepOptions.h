// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** 固定ステップ時計の上限を呼び出し側が明示するための設定値。 */
struct FFixedStepOptions {
    /** 固定更新 1 回の秒数。 */
    f64 step_seconds = 1.0 / 60.0;

    /** 1 回の Advance で返せる固定更新回数の上限。 */
    u32 maximum_steps_per_advance = 8u;

    /** 遅延入力から時計へ取り込む秒数の上限。 */
    f64 maximum_accumulated_seconds = 0.25;
};

} // namespace acs::game
