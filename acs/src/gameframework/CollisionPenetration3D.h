// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionShapeId3D.h"
#include "math/Vec.h"

namespace acs::game {

/** world空間のsphereを登録shapeから分離する最深接触情報。 */
struct FCollisionPenetration3D {
    /** 貫通しているshapeの世代付きhandle。 */
    FCollisionShapeId3D Shape{};

    /** sphereを法線方向へ動かして接触まで戻す距離。 */
    f32 Depth = 0.0f;

    /** 登録shapeからquery sphereへ向くworld空間の単位法線。 */
    FVec3 Normal{};

    /** 最小分離移動量を返す。 */
    FVec3 Translation() const noexcept
    {
        return Normal * Depth;
    }

    /** 有効形式のshape handleが格納されているかを返す。 */
    bool IsValid() const noexcept
    {
        return Shape.IsValid();
    }
};

} // namespace acs::game
