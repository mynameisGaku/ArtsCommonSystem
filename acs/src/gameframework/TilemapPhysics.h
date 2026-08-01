// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FTilemap → CRigidWorld2D 静的コライダ生成 (TilemapPhysics)
// -----------------------------------------------------------------------------
// タイルマップの指定レイヤの「埋まっている (非空) タイル」を、剛体ワールドへ静的
// AABB として登録する。これで I 作った CRigidWorld2D の動的ボディ (箱/ボール) が
// タイル地形と衝突できる (プラットフォーマ / 物理パズルの地形当たり)。
//
// 効率のため横方向の連続タイルを 1 つの幅広 AABB にまとめる (greedy 行マージ)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "gameframework/Forward.h"

namespace acs::game {

class FTilemap;

/**
 * タイルマップの layer の非空タイルを world へ静的 AABB として登録する (行ごと greedy マージ)。
 *
 * @param map 元タイルマップ。
 * @param layer 対象レイヤ。
 * @param world 追加先の剛体ワールド。
 * @param restitution 生成コライダの反発係数。
 * @param friction 生成コライダの摩擦係数。
 * @return 生成した静的 AABB コライダ数 (連続タイルはマージ済み)。
 */
u32 BuildRigidColliders(const FTilemap& map, u32 layer, CRigidWorld2D& world,
                        f32 restitution = 0.0f, f32 friction = 0.5f) noexcept;

} // namespace acs::game
