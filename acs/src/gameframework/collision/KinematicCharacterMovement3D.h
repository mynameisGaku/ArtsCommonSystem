// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/CollisionWorld3D.h"
#include "gameframework/collision/KinematicCharacterMovementInput3D.h"
#include "gameframework/collision/KinematicCharacterMovementParams3D.h"
#include "gameframework/collision/KinematicCharacterMovementResult3D.h"

namespace acs::game {

/**
 * 既存collision worldを変更せず、sphere型characterの次状態と接触事象を計算する。
 *
 * @details 移動前後の貫通解消は各4回、移動sweepと接触面slideは最大4回、接地確認は1回に
 * 固定する。入力不正、非有限な中間値、または固定回数内に貫通を解消できない場合はfalseを返し、
 * out_resultを変更しない。JumpSpeedが0ならjump要求を無効として接地を再確認する。
 * world、state、input、paramsも変更しない。
 * @param world AABBとsphereを登録済みの3D collision query world。
 * @param input 希望水平速度、jump要求、layer mask、自己除外shape。
 * @param state 現在のsphere中心、速度、接地状態。
 * @param delta_time 進める有限かつ0以上の秒数。
 * @param params sphere半径、重力、jump初速、接触調整値。
 * @param out_result 次状態と接触事象の書き込み先。失敗時は変更しない。
 * @return 入力検証、貫通解消、有限な次状態の確定に成功した場合だけtrue。
 */
bool TryMoveKinematicCharacter3D(const CCollisionWorld3D& world, const FKinematicCharacterMovementInput3D& input, const FKinematicCharacterState3D& state, f32 delta_time, const FKinematicCharacterMovementParams3D& params, FKinematicCharacterMovementResult3D& out_result) noexcept;

} // namespace acs::game
