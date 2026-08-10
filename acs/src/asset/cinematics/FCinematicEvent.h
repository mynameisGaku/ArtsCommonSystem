// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "asset/cinematics/ECinematicEventKind.h"
#include "container/String.h"
#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::asset {

/** 一時点の演出値を所有する値型です。 */
struct FCinematicEvent {
    /** イベント時刻です。有限で0以上にします。 */
    f32 time_sec = 0.0f;

    /** イベントの種別です。 */
    ECinematicEventKind kind = ECinematicEventKind::Wait;

    /** MoveCamera の目標位置です。 */
    FVec2 target_pos{};

    /** MoveCamera のズーム値です。 */
    f32 camera_zoom = 1.0f;

    /** MoveCamera の移動時間です。 */
    f32 camera_duration = 0.0f;

    /** Dialogue または Music の所有文字列です。 */
    FString text;

    /** Music のフェード秒数です。 */
    f32 music_fade = 0.0f;

    /** FireEvent の識別子です。 */
    u32 event_id = 0u;
};

} // namespace acs::asset
