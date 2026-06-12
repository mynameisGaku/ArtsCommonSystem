// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FTilemap → NavGrid 経路探索ブリッジ (TilemapNav)
// -----------------------------------------------------------------------------
// タイルマップの指定レイヤから NavGrid (A*) を構築し (非空タイル = 非歩行可能)、
// world 座標の start→goal を探索して world 中心の waypoint 列を返す。
// タイル地形上の敵 AI ナビゲーションに使う (FTilemap + NavGrid の合成)。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"
#include "container/Array.h"

namespace acs::game {

class FTilemap;
class NavGrid;

/**
 * タイルマップの layer から NavGrid を構築する (非空タイル = 非歩行可能)。
 *
 * @param map 元タイルマップ。
 * @param layer 障害物判定に使うレイヤ。
 * @param nav 構築先 NavGrid (Init + SetWalkable される)。
 */
void BuildNavGridFromTilemap(const FTilemap& map, u32 layer, NavGrid& nav) noexcept;

/**
 * world 座標の start→goal を nav で探索し、world 中心の waypoint 列を返す。
 *
 * @details nav は事前に BuildNavGridFromTilemap 済みであること。start/goal の tile 変換は
 * map で行う。範囲外 / 経路なしは false。
 * @param map 座標変換用タイルマップ。
 * @param nav 探索に使う NavGrid (構築済み)。
 * @param start_world 始点 (world)。
 * @param goal_world 終点 (world)。
 * @param out_path 結果の world waypoint 列 (tile 中心、start→goal 順)。
 * @return 経路が見つかれば true。
 */
bool FindTilemapPath(const FTilemap& map, NavGrid& nav,
                     FVec2 start_world, FVec2 goal_world, TArray<FVec2>& out_path) noexcept;

} // namespace acs::game
