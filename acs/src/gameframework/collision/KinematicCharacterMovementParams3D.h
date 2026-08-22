// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/** sphere型kinematic characterの形状と移動調整値。 */
struct FKinematicCharacterMovementParams3D {
    /** characterを近似するsphere半径。有限かつ0より大きい値だけを受理する。 */
    f32 Radius = 0.5f;

    /** world -Y方向へ加える加速度。有限かつ0以上の値だけを受理する。 */
    f32 GravityAcceleration = 9.80665f;

    /** 接地中のjump要求で設定するworld +Y初速。有限かつ0以上で、0はjump無効を表す。 */
    f32 JumpSpeed = 5.0f;

    /** 接触後に法線方向へ離す距離。有限で0より大きく、Radius未満でなければならない。 */
    f32 ContactOffset = 0.001f;

    /** 接地確認でsphereを下へ調べる追加距離。有限かつ0以上の値だけを受理する。 */
    f32 GroundProbeDistance = 0.05f;

    /** 歩行可能とみなす接触法線Y成分の下限。有限な0以上1以下でなければならない。 */
    f32 MinimumGroundNormalY = 0.7f;
};

} // namespace acs::game
