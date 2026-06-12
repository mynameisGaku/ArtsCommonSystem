// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FRigidWorld2D のデバッグ可視化 (RigidWorldDebug)
// -----------------------------------------------------------------------------
// 剛体ワールドの全アクティブボディ (円/箱) の輪郭と、動的ボディの速度ベクトルを
// FDebugDraw へ積む。物理の挙動を目で確認するための薄いブリッジ (描画は呼び出し側)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FRigidWorld2D;
class FDebugDraw;

/**
 * ワールドの全アクティブボディ輪郭 + 速度矢印を dd へ積む。
 *
 * @param world 可視化する剛体ワールド。
 * @param dd 線分を積む先のデバッグ描画バッファ。
 * @param color ボディ輪郭の色。
 * @param vel_color 速度ベクトル矢印の色。
 * @param vel_scale 速度ベクトルの長さ倍率 (world 単位/(単位速度))。
 */
void DebugDrawRigidWorld(const FRigidWorld2D& world, FDebugDraw& dd,
                         FVec4 color, FVec4 vel_color, f32 vel_scale = 0.1f) noexcept;

} // namespace acs::game
