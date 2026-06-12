// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — TilemapQuery 実装。グリッド DDA レイキャスト。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/TilemapQuery.h"
#include "gameframework/Tilemap.h"

#include <cmath>   // std::sqrt / std::floor / std::fabs

namespace acs::game {

namespace {
inline bool InBounds(i32 x, i32 y, i32 w, i32 h) noexcept {
    return x >= 0 && x < w && y >= 0 && y < h;
}
inline bool SolidAt(const FTilemap& map, u32 layer, i32 x, i32 y) noexcept {
    return !map.GetTile(static_cast<u32>(x), static_cast<u32>(y), layer).IsEmpty();
}
} // namespace

FTileRayHit RaycastTilemap(const FTilemap& map, u32 layer,
                           FVec2 origin, FVec2 dir, f32 max_dist) noexcept {
    FTileRayHit hit;
    const f32 ts = map.TileSize();
    if (ts <= 0.0f) return hit;

    const f32 dl = std::sqrt(dir.x * dir.x + dir.y * dir.y);
    if (dl < 1e-9f) return hit;
    const FVec2 d{ dir.x / dl, dir.y / dl };

    const i32 w = static_cast<i32>(map.Width());
    const i32 h = static_cast<i32>(map.Height());

    // 始点セル (負も許容して DDA で進める)。
    i32 cx = static_cast<i32>(std::floor(origin.x / ts));
    i32 cy = static_cast<i32>(std::floor(origin.y / ts));

    // 始点が壁内なら距離 0 で即ヒット。
    if (InBounds(cx, cy, w, h) && SolidAt(map, layer, cx, cy)) {
        hit.hit = true; hit.tile_x = static_cast<u32>(cx); hit.tile_y = static_cast<u32>(cy);
        hit.point = origin; hit.normal = FVec2{ 0.0f, 0.0f }; hit.distance = 0.0f;   // 壁内: 法線は不定 = 0
        return hit;
    }

    const i32 stepX = (d.x >= 0.0f) ? 1 : -1;
    const i32 stepY = (d.y >= 0.0f) ? 1 : -1;
    constexpr f32 kInf = 1e30f;

    f32 tMaxX, tDeltaX, tMaxY, tDeltaY;
    if (std::fabs(d.x) < 1e-9f) { tMaxX = kInf; tDeltaX = kInf; }
    else {
        const f32 boundaryX = (stepX > 0 ? static_cast<f32>(cx + 1) : static_cast<f32>(cx)) * ts;
        tMaxX   = (boundaryX - origin.x) / d.x;
        tDeltaX = ts / std::fabs(d.x);
    }
    if (std::fabs(d.y) < 1e-9f) { tMaxY = kInf; tDeltaY = kInf; }
    else {
        const f32 boundaryY = (stepY > 0 ? static_cast<f32>(cy + 1) : static_cast<f32>(cy)) * ts;
        tMaxY   = (boundaryY - origin.y) / d.y;
        tDeltaY = ts / std::fabs(d.y);
    }

    // セル境界を順に越えて進む。t は次セルへ入る world 距離。
    // 反復上限は max_dist 依存にする (グリッド外の遠い始点でも正しい結果まで届く)。
    const i32 kMaxIter = static_cast<i32>(max_dist / ts) + 4 * (w + h) + 16;
    for (i32 iter = 0; iter < kMaxIter; ++iter) {
        f32   t;
        FVec2 nrm;
        if (tMaxX < tMaxY) { t = tMaxX; cx += stepX; tMaxX += tDeltaX; nrm = FVec2{ static_cast<f32>(-stepX), 0.0f }; }
        else               { t = tMaxY; cy += stepY; tMaxY += tDeltaY; nrm = FVec2{ 0.0f, static_cast<f32>(-stepY) }; }

        if (t > max_dist) return hit;        // 距離外 → miss

        if (InBounds(cx, cy, w, h) && SolidAt(map, layer, cx, cy)) {
            hit.hit = true; hit.tile_x = static_cast<u32>(cx); hit.tile_y = static_cast<u32>(cy);
            hit.point = FVec2{ origin.x + d.x * t, origin.y + d.y * t };
            hit.normal = nrm; hit.distance = t;
            return hit;
        }
    }
    return hit;   // 安全網に達した = miss
}

} // namespace acs::game
