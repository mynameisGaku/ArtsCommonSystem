// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game::cinetimeline {

/** 編集キーの種類を表し、未知の値は文書へ追加できません。 */
enum class ECinematicTimelineKeyKind : u8 {
    /** カメラ位置を記録します。 */
    CameraCut = 0,
    /** フェード終了色を記録します。 */
    FadeColor = 1,
    /** 再生時間倍率を記録します。 */
    TimeScale = 2,
    /** エフェクト識別子と編集位置を記録します。 */
    SpawnEffect = 3,
    /** コールバック識別子を記録します。 */
    TriggerCallback = 4,
};

using ETimelineKeyKind = ECinematicTimelineKeyKind;

}
