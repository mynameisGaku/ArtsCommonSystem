// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "math/Vec.h"

namespace acs::game {

/** sphereで近似するkinematic characterの一時刻分の状態。 */
struct FKinematicCharacterState3D {
    /** character sphere中心のworld座標。 */
    FVec3 Position{};

    /** 重力と接触面への投影を反映したworld速度。 */
    FVec3 Velocity{};

    /** 接地面のworld単位法線。非接地時は零ベクトル。 */
    FVec3 GroundNormal{};

    /** 歩行可能な面を直下または移動経路で検出していればtrue。 */
    bool Grounded = false;
};

} // namespace acs::game
