// SPDX-License-Identifier: Apache-2.0
// FTilemapComponent — FTilemap (data) を FScene2D 上で描画する Component2D。
//
// グリッドアトラス (cols×rows のタイル) テクスチャを持ち、各レイヤの非空タイルを
// その atlas セルの UV で SpriteBatch に描く。タイル ID v (1-based、0=空) は
// セル index (v-1) に対応する。owner ノードの world 位置がマップ原点になるので、
// タイルマップを丸ごと移動/配置できる。
//
// 使い方:
//   auto& tm = node->AddComponent<FTilemapComponent>();
//   tm.Map().Init(/*w=*/16, /*h=*/12, /*layers=*/1, /*tile_size=*/1.0f);  // world 単位
//   tm.Map().FillRect(0, 0, 15, 0, FTileId{1});   // 上端を tile 1 で
//   tm.SetTexture(atlas_tex);
//   tm.SetAtlasGrid(/*cols=*/4, /*rows=*/4);
//   // ソリッドにしたいレイヤを物理ワールドへ:
//   tm.BuildCollision(Services().Physics(), /*layer=*/0, /*collision_layer_bit=*/kWall);
#pragma once

#include "gameframework/Component2D.h"
#include "gameframework/Tilemap.h"
#include "math/Vec.h"

namespace acs {
class IRhiTexture;
}

namespace acs::game {

class FCollisionWorld2D;

class FTilemapComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FTilemapComponent)

    FTilemapComponent() noexcept = default;

    // 所有するタイルマップ。Init/Fill/SetTile はここ経由で行う。
    FTilemap& Map() noexcept { return m_Map; }
    const FTilemap& Map() const noexcept { return m_Map; }

    // アトラステクスチャ (非所有) と、その中のタイル格子 (cols×rows)。
    void SetTexture(IRhiTexture* tex) noexcept { m_Atlas = tex; }
    void SetAtlasGrid(u32 cols, u32 rows) noexcept {
        m_Cols = cols ? cols : 1u;
        m_Rows = rows ? rows : 1u;
    }
    void SetTint(FVec4 tint) noexcept { m_Tint = tint; }

    // 指定レイヤの非空タイルを AABB として物理ワールドへ登録する (ソリッド化)。
    // 1 タイル = 1 AABB。collision_layer_bit は CollisionWorld2D のレイヤ bitmask。
    void BuildCollision(FCollisionWorld2D& world, u32 layer, u32 collision_layer_bit) noexcept;

    void OnDraw(RenderContext& rc) noexcept override;

private:
    FTilemap     m_Map;
    IRhiTexture* m_Atlas = nullptr;   // non-owning
    u32          m_Cols  = 1;
    u32          m_Rows  = 1;
    FVec4        m_Tint{1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace acs::game
