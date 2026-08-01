// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — TilemapPhysics 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/TilemapPhysics.h"
#include "gameframework/Tilemap.h"
#include "gameframework/RigidWorld2D.h"
#include "math/Vec.h"

namespace acs::game {

u32 BuildRigidColliders(const FTilemap& map, u32 layer, CRigidWorld2D& world,
                        f32 restitution, f32 friction) noexcept {
    const u32 w  = map.Width();
    const u32 h  = map.Height();
    const f32 ts = map.TileSize();
    u32 count = 0u;

    for (u32 y = 0; y < h; ++y) {
        u32 x = 0;
        while (x < w) {
            if (map.GetTile(x, y, layer).IsEmpty()) { ++x; continue; }
            // 連続する非空タイルの run [x0..x1] を見つける。
            const u32 x0 = x;
            while (x < w && !map.GetTile(x, y, layer).IsEmpty()) ++x;
            const u32 x1 = x - 1;

            // run を 1 つの幅広 AABB にまとめる (TileToWorld は tile 中心)。
            const FVec2 lc = map.TileToWorld(x0, y);
            const FVec2 rc = map.TileToWorld(x1, y);
            const FVec2 center{ (lc.x + rc.x) * 0.5f, lc.y };
            const FVec2 half{ static_cast<f32>(x1 - x0 + 1) * ts * 0.5f, ts * 0.5f };
            world.AddStaticAabb(center, half, restitution, friction);
            ++count;
        }
    }
    return count;
}

} // namespace acs::game
