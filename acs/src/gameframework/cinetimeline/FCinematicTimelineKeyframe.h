// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "gameframework/cinetimeline/ECinematicTimelineKeyKind.h"
#include "math/Vec.h"

namespace acs::game::cinetimeline {

/** 編集キーの値を保持し、再生列へ変換する入力を表します。 */
struct FCinematicTimelineKeyframe {
    /** 発火時刻。有限で文書範囲内でなければ追加できません。 */
    f32 time_sec = 0.0f;
    /** 編集キーの種類です。 */
    ECinematicTimelineKeyKind kind = ECinematicTimelineKeyKind::TriggerCallback;
    /** カメラまたはエフェクト位置です。 */
    FVec3 camera_target{0.0f, 0.0f, 0.0f};
    /** フェード開始色です。 */
    FVec3 fade_start_color{0.0f, 0.0f, 0.0f};
    /** フェード終了色です。 */
    FVec3 fade_end_color{1.0f, 1.0f, 1.0f};
    /** 再生時間倍率です。 */
    f32 time_scale = 1.0f;
    /** エフェクトまたはコールバック識別子です。 */
    u32 event_id = 0u;
};

using FEditorKeyframe = FCinematicTimelineKeyframe;

}
