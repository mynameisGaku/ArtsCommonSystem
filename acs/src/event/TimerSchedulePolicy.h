// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/** タイマ登録時にコンパイル時選択する発火方針。 */
enum class ETimerSchedulePolicy : u8 {
    /** 遅延後に 1 回だけ発火する。 */
    Once,

    /** 指定周期で繰り返し発火する。 */
    Repeating,
};

} // namespace acs
