// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "subsystem/SubsystemTickPhase.h"

namespace acs {

/** 1 回のサブシステム更新へ渡す時刻と更新段階。 */
struct FSubsystemFrameContext {
    /** 時間倍率を反映した経過秒。 */
    f32 scaled_delta_seconds = 0.0f;
    /** 時間倍率を反映しない経過秒。 */
    f32 unscaled_delta_seconds = 0.0f;
    /** 呼び出し元が管理するフレーム番号。 */
    u64 frame_number = 0u;
    /** 今回呼び出す更新段階。 */
    ESubsystemTickPhase phase = ESubsystemTickPhase::None;
};

} // namespace acs
