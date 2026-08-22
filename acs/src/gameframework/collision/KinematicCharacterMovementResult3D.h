// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionShapeId3D.h"
#include "gameframework/collision/KinematicCharacterState3D.h"

namespace acs::game {

/** 一回のkinematic character移動で確定した次状態と接触事象。 */
struct FKinematicCharacterMovementResult3D {
    /** 呼び出し側が次回入力として保持する確定済み状態。 */
    FKinematicCharacterState3D NextState{};

    /** 入力PositionからNextState.Positionまでのworld移動量。貫通解消を含む。 */
    FVec3 Translation{};

    /** 最後にsweepまたは接地確認で接触したshape。接触なしでは無効。 */
    FCollisionShapeId3D LastCollisionShape{};

    /** LastCollisionShapeからcharacterへ向くworld単位法線。接触なしでは零。 */
    FVec3 LastCollisionNormal{};

    /** 移動sweepと接地確認で検出した接触回数。 */
    u32 CollisionCount = 0u;

    /** 移動前後の貫通解消で実際に適用した分離回数の合計。 */
    u32 DepenetrationIterationCount = 0u;

    /** 接地中に0より大きいJumpSpeedでjump要求を受理した場合はtrue。 */
    bool Jumped = false;

    /** 一回以上の貫通分離を適用した場合はtrue。 */
    bool Depenetrated = false;

    /** 歩行可能な面へ接触または接地確認した場合はtrue。 */
    bool HitGround = false;

    /** 歩行可能面と天井以外へ接触した場合はtrue。 */
    bool HitWall = false;

    /** 下向き法線を持つ面へ接触した場合はtrue。 */
    bool HitCeiling = false;

    /** 4回のslide後にも未適用移動が残り、安全のため打ち切った場合はtrue。 */
    bool SlideIterationLimitReached = false;
};

} // namespace acs::game
