// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — TilemapNav 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/TilemapNav.h"
#include "gameframework/Tilemap.h"
#include "gameframework/Pathfinding.h"

namespace acs::game {

void BuildNavGridFromTilemap(const FTilemap& map, u32 layer, CNavGrid& nav) noexcept {
    const u32 w = map.Width();
    const u32 h = map.Height();
    nav.Init(w, h);
    for (u32 y = 0; y < h; ++y)
        for (u32 x = 0; x < w; ++x)
            nav.SetWalkable(x, y, map.GetTile(x, y, layer).IsEmpty());   // 非空 = 障害物
}

bool FindTilemapPath(const FTilemap& map, CNavGrid& nav,
                     FVec2 start_world, FVec2 goal_world, TArray<FVec2>& out_path) noexcept {
    out_path.Reset();   // 失敗時も出力を空にする (stale データを残さない)
    u32 sx, sy, gx, gy;
    if (!map.WorldToTile(start_world, sx, sy)) return false;
    if (!map.WorldToTile(goal_world,  gx, gy)) return false;

    TArray<CNavGrid::FPathPoint> pts;
    if (!nav.FindPath(sx, sy, gx, gy, pts)) return false;

    for (u32 i = 0; i < pts.Num(); ++i)
        out_path.Add(map.TileToWorld(pts[i].x, pts[i].y));   // tile 中心 world 座標へ
    return true;
}

} // namespace acs::game
