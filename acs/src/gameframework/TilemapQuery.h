// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — タイルマップ空間クエリ (TilemapQuery)
// -----------------------------------------------------------------------------
// FTilemap の指定レイヤに対するレイキャスト (グリッド DDA, Amanatides-Woo)。
// AI の視線判定 (line-of-sight)・弾道・レーザー照準・視界コーン等に使う。
// 「非空タイル = 遮蔽」とみなし、最初に当たったタイルのセル・world 交点・面法線を返す。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs::game {

class FTilemap;

/** タイルマップ・レイキャストの結果 (hit=false でヒットなし)。 */
struct FTileRayHit {
    bool  hit      = false;
    u32   tile_x   = 0u;          // ヒットしたタイルのセル
    u32   tile_y   = 0u;
    FVec2 point{0.0f, 0.0f};      // world 交点
    FVec2 normal{0.0f, 0.0f};     // ヒット面の外向き法線 (軸並行、始点が壁内なら 0)
    f32   distance = 0.0f;        // origin からの距離
};

/**
 * 指定レイヤの非空タイルに対してレイキャストする (グリッド DDA)。
 *
 * @param map 対象タイルマップ。
 * @param layer 遮蔽判定に使うレイヤ。
 * @param origin レイ始点 (world)。
 * @param dir レイ方向 (内部で正規化、ゼロは miss)。
 * @param max_dist 最大距離 (world)。
 * @return 最初に当たったタイルの情報 (hit=false でなし)。始点が壁内なら距離 0 でヒット。
 */
FTileRayHit RaycastTilemap(const FTilemap& map, u32 layer,
                           FVec2 origin, FVec2 dir, f32 max_dist) noexcept;

} // namespace acs::game
