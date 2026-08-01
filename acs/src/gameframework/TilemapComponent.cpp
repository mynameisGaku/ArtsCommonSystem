// SPDX-License-Identifier: Apache-2.0
#include "gameframework/TilemapComponent.h"
#include "gameframework/ANode.h"
#include "gameframework/RenderContext.h"
#include "gameframework/CollisionWorld2D.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"

namespace acs::game {

/** 全レイヤの非空タイルを atlas UV (未設定時はデバッグ色) で SpriteBatch へ描く。 */
void ATilemapComponent::OnDraw(FRenderContext& rc) noexcept {
    if (!rc.HasSprites()) return;
    FSpriteBatch& sb = rc.Sprites();

    const FVec2 origin = Owner().World2D().position;
    const f32   ts     = m_Map.TileSize();
    const u32   w      = m_Map.Width();
    const u32   h      = m_Map.Height();
    const u32   layers = m_Map.LayerCount();
    const f32   cell_w = 1.0f / static_cast<f32>(m_Cols);
    const f32   cell_h = 1.0f / static_cast<f32>(m_Rows);

    for (u32 L = 0; L < layers; ++L) {        // 0 = 最背面 → 前面へ
        const FTileId* data = m_Map.LayerData(L);
        if (data == nullptr) continue;
        for (u32 y = 0; y < h; ++y) {
            for (u32 x = 0; x < w; ++x) {
                const FTileId t = data[y * w + x];
                if (t.IsEmpty()) continue;
                const FVec2 c  = m_Map.TileToWorld(x, y);
                const f32   cx = origin.x + c.x;
                const f32   cy = origin.y + c.y;
                if (m_Atlas != nullptr) {
                    const u32 cell = static_cast<u32>(t.value - 1u);
                    const u32 col  = cell % m_Cols;
                    const u32 row  = (cell / m_Cols) % m_Rows;
                    const f32 u0   = static_cast<f32>(col) * cell_w;
                    const f32 v0   = static_cast<f32>(row) * cell_h;
                    sb.DrawRotated(*m_Atlas, cx, cy, ts, ts, 0.0f,
                                   u0, v0, u0 + cell_w, v0 + cell_h, m_Tint);
                } else {
                    // テクスチャ未設定: tile id 由来の色でデバッグ描画。
                    const f32 r = static_cast<f32>((t.value * 53u) % 255u) / 255.0f;
                    const f32 g = static_cast<f32>((t.value * 97u) % 255u) / 255.0f;
                    const f32 b = static_cast<f32>((t.value * 193u) % 255u) / 255.0f;
                    sb.DrawRectRotated(cx, cy, ts, ts, 0.0f, FVec4{r, g, b, 1.0f});
                }
            }
        }
    }
}

/** 指定レイヤの非空タイルを 1 タイル = 1 AABB で物理ワールドへ登録する。 */
void ATilemapComponent::BuildCollision(CCollisionWorld2D& world, u32 layer,
                                       u32 collision_layer_bit) noexcept {
    const FTileId* data = m_Map.LayerData(layer);
    if (data == nullptr || !HasOwner()) return;
    const FVec2 origin = Owner().World2D().position;
    const f32   ts     = m_Map.TileSize();
    const FVec2 half{ts * 0.5f, ts * 0.5f};
    const u32   w = m_Map.Width();
    const u32   h = m_Map.Height();
    for (u32 y = 0; y < h; ++y) {
        for (u32 x = 0; x < w; ++x) {
            if (data[y * w + x].IsEmpty()) continue;
            const FVec2 c = m_Map.TileToWorld(x, y);
            world.AddAabb(FAabb2{FVec2{origin.x + c.x, origin.y + c.y}, half},
                          collision_layer_bit);
        }
    }
}

} // namespace acs::game
