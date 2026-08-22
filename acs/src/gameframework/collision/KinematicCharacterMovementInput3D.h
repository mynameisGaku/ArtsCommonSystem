// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionShapeId3D.h"
#include "math/Vec.h"

namespace acs::game {

/** 一回のkinematic character移動へ渡す操作入力とcollision filter。 */
struct FKinematicCharacterMovementInput3D {
    /** world X/Z軸へ適用する希望水平速度。xはworld X、yはworld Zを表す。 */
    FVec2 DesiredHorizontalVelocity{};

    /** queryから除外する自身の登録shape。無効またはstaleなら除外しない。 */
    FCollisionShapeId3D SelfShape{};

    /** 登録shapeのlayerとのANDが0でないshapeだけを対象にするmask。 */
    u32 CollisionMask = 0xFFFFFFFFu;

    /** 接地中かつJumpSpeedが0より大きい場合に上向き初速を与える要求。 */
    bool JumpRequested = false;
};

} // namespace acs::game
