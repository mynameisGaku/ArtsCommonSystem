// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "gameframework/AComponent.h"
#include "gameframework/Forward.h"
#include "gameframework/Tilemap.h"
#include "math/Vec.h"

namespace acs {
class IRhiTexture;
}

namespace acs::game {

/**
 * FTilemap のデータを ANode 上で描画する AComponent。
 *
 * @details
 * グリッドアトラス (cols×rows のタイル) テクスチャを持ち、各レイヤの非空タイルを
 * その atlas セルの UV で CSpriteBatch に描く。タイル ID v (1-based、0=空) はセル
 * index (v-1) に対応する。owner ノードの world 位置がマップ原点になるので、タイル
 * マップを丸ごと移動/配置できる。BuildCollision で指定レイヤを物理ワールドへ AABB
 * 登録してソリッド化できる。
 */
class ATilemapComponent : public AComponent {
public:
    /** このコンポーネントの種別 ID を提供する (Kind() を override)。 */
    ACS_GAME_COMPONENT_KIND(ATilemapComponent)

    /** 空のタイルマップコンポーネントを構築する (アトラス未設定、tint 白)。 */
    ATilemapComponent() noexcept = default;

    /**
     * 所有するタイルマップへの可変参照を返す (Init/Fill/SetTile はここ経由で行う)。
     *
     * @return タイルマップへの参照。
     */
    FTilemap& Map() noexcept { return m_Map; }

    /**
     * 所有するタイルマップへの const 参照を返す。
     *
     * @return タイルマップへの const 参照。
     */
    const FTilemap& Map() const noexcept { return m_Map; }

    /**
     * アトラステクスチャを設定する (非所有)。
     *
     * @details nullptr のときは tile id 由来の色でデバッグ描画になる。
     * @param tex アトラステクスチャ (所有権は移らない)。
     */
    void SetTexture(IRhiTexture* tex) noexcept { m_Atlas = tex; }

    /**
     * アトラス内のタイル格子 (cols×rows) を設定する。
     *
     * @details 0 を渡した次元は 1 にクランプされる (0 除算回避)。
     * @param cols アトラスの列数。
     * @param rows アトラスの行数。
     */
    void SetAtlasGrid(u32 cols, u32 rows) noexcept {
        m_Cols = cols ? cols : 1u;
        m_Rows = rows ? rows : 1u;
    }

    /**
     * 全タイルへ乗算する tint カラーを設定する。
     *
     * @param tint 乗算する RGBA tint (既定は白)。
     */
    void SetTint(FVec4 tint) noexcept { m_Tint = tint; }

    /**
     * 指定レイヤの非空タイルを AABB として物理ワールドへ登録する (ソリッド化)。
     *
     * @details
     * 1 タイル = 1 AABB で、各タイルの world 中心に半辺 tile_size/2 の FAabb2 を追加する。
     * owner が無い、または layer が範囲外で LayerData が nullptr の場合は何もしない。
     * @param world 登録先の 2D 衝突ワールド。
     * @param layer ソリッド化するレイヤ index。
 * @param collision_layer_bit CCollisionWorld2D のレイヤ bitmask。
     */
    void BuildCollision(CCollisionWorld2D& world, u32 layer, u32 collision_layer_bit) noexcept;

    /**
     * 全レイヤの非空タイルを CSpriteBatch へ描画する。
     *
     * @details
     * レイヤ 0 を最背面として前面へ順に描く。アトラス設定時はタイル ID から算出した
     * セルの UV 矩形で描画し、未設定時は tile id 由来の色でデバッグ矩形を描く。rc に
     * CSpriteBatch が無い場合は何もしない。
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(FRenderContext& rc) noexcept override;

private:
    /** 所有するタイルマップデータ。 */
    FTilemap     m_Map;

    /** アトラステクスチャ (非所有、nullptr ならデバッグ色描画)。 */
    IRhiTexture* m_Atlas = nullptr;

    /** アトラスの列数 (最小 1)。 */
    u32          m_Cols  = 1;

    /** アトラスの行数 (最小 1)。 */
    u32          m_Rows  = 1;

    /** 全タイルへ乗算する tint カラー。 */
    FVec4        m_Tint{1.0f, 1.0f, 1.0f, 1.0f};
};

} // namespace acs::game
