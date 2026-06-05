// SPDX-License-Identifier: Apache-2.0
// FSprite2DComponent - minimal render component for FNode2D.
//
// It draws a colored rectangle or a texture through the FSpriteBatch installed
// in RenderContext by FScene2D. Size is in world units; the owner transform
// supplies position/rotation/scale.
#pragma once

#include "gameframework/Component2D.h"
#include "math/Vec.h"

namespace acs {
class IRhiTexture;
}

namespace acs::game {

/**
 * FNode2D に色付き矩形 / テクスチャを描く最小の描画コンポーネント。
 *
 * @details
 * FScene2D が RenderContext に差し込んだ FSpriteBatch を通じて、owner の world
 * transform を pivot に矩形 1 枚を積む。サイズは world 単位で、位置・回転・スケールは
 * owner の transform から供給される。テクスチャ未設定なら tint 色の塗り潰し矩形を、
 * 設定済みなら UV サブ矩形 + tint でテクスチャを描く。
 */
class FSprite2DComponent : public FComponent2D {
public:
    /** コンポーネント種別タグを宣言する (Kind() による型識別用)。 */
    ACS_GAME_COMPONENT_KIND(FSprite2DComponent)

    /** 空のスプライトを構築する (size {1,1}・tint 白・テクスチャ無し)。 */
    FSprite2DComponent() noexcept = default;

    /**
     * サイズと tint 色を指定して構築する。
     *
     * @param size スプライトの world 単位サイズ。
     * @param tint 乗算する tint 色 (既定 {1,1,1,1} = 白)。
     */
    explicit FSprite2DComponent(FVec2 size, FVec4 tint = FVec4{1, 1, 1, 1}) noexcept
        : m_Size(size), m_Tint(tint) {}

    /**
     * 描画するテクスチャを設定する (nullptr で塗り潰し矩形に戻る)。
     *
     * @param tex 描画に使うテクスチャ (非所有)。
     */
    void SetTexture(IRhiTexture* tex) noexcept { m_Texture = tex; }

    /**
     * 設定済みテクスチャを返す。
     *
     * @return 描画テクスチャ (未設定なら nullptr)。
     */
    IRhiTexture* Texture() const noexcept { return m_Texture; }

    /**
     * スプライトのサイズを設定する。
     *
     * @param s 新しい world 単位サイズ。
     */
    void SetSize(FVec2 s) noexcept { m_Size = s; }

    /**
     * スプライトのサイズを返す。
     *
     * @return world 単位サイズ。
     */
    FVec2 Size() const noexcept { return m_Size; }

    /**
     * tint 色を設定する。
     *
     * @param c 乗算する tint 色。
     */
    void SetTint(FVec4 c) noexcept { m_Tint = c; }

    /**
     * tint 色を返す。
     *
     * @return 乗算する tint 色。
     */
    FVec4 Tint() const noexcept { return m_Tint; }

    /**
     * pivot を設定する (size 内の正規化位置)。
     *
     * @param p pivot (0..1、{0.5,0.5} で中心)。
     */
    void SetPivot(FVec2 p) noexcept { m_Pivot = p; }

    /**
     * pivot を返す。
     *
     * @return size 内の正規化 pivot。
     */
    FVec2 Pivot() const noexcept { return m_Pivot; }

    /**
     * UV サブ矩形を 4 成分で設定する (スプライトシートの 1 フレーム等)。
     *
     * @details 既定 {0,0,1,1} = テクスチャ全体。FSpriteAnimComponent がフレーム毎にここを書き換えてアニメさせる。
     * @param u0 左上 U 座標。
     * @param v0 左上 V 座標。
     * @param u1 右下 U 座標。
     * @param v1 右下 V 座標。
     */
    void SetUvRect(f32 u0, f32 v0, f32 u1, f32 v1) noexcept { m_UvMin = FVec2{u0, v0}; m_UvMax = FVec2{u1, v1}; }

    /**
     * UV サブ矩形を {u0, v0, u1, v1} の 1 ベクタで設定する。
     *
     * @param uv UV サブ矩形 {u0, v0, u1, v1}。
     */
    void SetUvRect(FVec4 uv) noexcept { m_UvMin = FVec2{uv.x, uv.y}; m_UvMax = FVec2{uv.z, uv.w}; }

    /**
     * UV サブ矩形の左上を返す。
     *
     * @return UV 左上 {u0, v0}。
     */
    FVec2 UvMin() const noexcept { return m_UvMin; }

    /**
     * UV サブ矩形の右下を返す。
     *
     * @return UV 右下 {u1, v1}。
     */
    FVec2 UvMax() const noexcept { return m_UvMax; }

    /**
     * 描画フック。owner の world transform を pivot に矩形 1 枚を SpriteBatch へ積む。
     *
     * @param rc 描画コマンドを積む先のレンダーコンテキスト。
     */
    void OnDraw(RenderContext& rc) noexcept override;

private:
    /** 描画するテクスチャ (非所有。nullptr なら塗り潰し矩形)。 */
    IRhiTexture* m_Texture = nullptr;

    /** スプライトの world 単位サイズ。 */
    FVec2        m_Size{1.0f, 1.0f};

    /** 乗算する tint 色。 */
    FVec4        m_Tint{1.0f, 1.0f, 1.0f, 1.0f};

    /** pivot (size 内の 0..1 正規化位置)。 */
    FVec2        m_Pivot{0.5f, 0.5f};

    /** UV サブ矩形の左上 (既定 = テクスチャ全体)。 */
    FVec2        m_UvMin{0.0f, 0.0f};

    /** UV サブ矩形の右下。 */
    FVec2        m_UvMax{1.0f, 1.0f};
};

} // namespace acs::game
