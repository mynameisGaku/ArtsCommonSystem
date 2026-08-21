// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionShapeId3D.h"
#include "math/Vec.h"

namespace acs::game {

/** world空間を移動するsphereが登録shapeへ最初に接触した結果。 */
struct FCollisionSweepHit3D {
    /** 接触したshapeの世代付きhandle。 */
    FCollisionShapeId3D Shape{};

    /** `RayOrigin + T * RayDirection` で表す最初の接触parameter。 */
    f32 T = 0.0f;

    /** 接触時の移動sphere中心。 */
    FVec3 Center{};

    /** 登録shapeから移動sphereへ向くworld空間の単位法線。 */
    FVec3 Normal{};

    /** T=0で既にshapeと重なっていた場合はtrue。 */
    bool StartedOverlapping = false;

    /** 有効形式のshape handleが格納されているかを返す。 */
    bool IsValid() const noexcept
    {
        return Shape.IsValid();
    }
};

} // namespace acs::game
