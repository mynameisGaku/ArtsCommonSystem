// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — RenderContext
//
// 全シーン共有の描画コンテキスト。「現フレームの IRhiCommandList と画面サイズ」
// を保持する軽量参照ホルダに、FSpriteBatch / Font / 共通シェーダを足して
// 「シーン切替でパイプライン再構築しない」を実現する。
//
// Scene 側はこれを OnRender(rc) で受け取り、必要なら rc.Cmd()/Width()/Height()
// から描画コマンドを発行する。素の RHI を直接叩いてもよいし、ユーザーが自分の
// FSpriteBatch を持ってもよい。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace acs {
class IRhiCommandList;
class FRenderer;
class FSpriteBatch;
class Font;
class IRhiTexture;
}

namespace acs::game {

/**
 * 全シーン共有の描画コンテキスト。
 *
 * @details
 * 現フレームの IRhiCommandList と画面サイズを保持する軽量参照ホルダ。FSpriteBatch /
 * Font / 反射テクスチャ / world→screen 変換などをまとめ、シーン切替でパイプラインを
 * 再構築せずに描画できるようにする。Scene 側は OnRender(rc) でこれを受け取り、
 * rc.Cmd()/Width()/Height() から描画コマンドを発行する。
 */
class RenderContext {
public:
    /** 空状態で構築する (各参照は _BeginFrame で配線)。 */
    RenderContext() noexcept = default;

    /** 破棄する (参照のみ保持するため何もしない)。 */
    ~RenderContext() noexcept = default;

    /** コピー禁止 (フレーム配線を単独で保持するため)。 */
    RenderContext(const RenderContext&)            = delete;

    /** コピー代入も禁止。 */
    RenderContext& operator=(const RenderContext&) = delete;

    /**
     * フレーム冒頭で FGame が呼び、描画リソースを配線する。
     *
     * @details m_Font はこの後 FGame が _SetFont で配線する (game 寿命で共有)。
     * @param r 描画に使う FRenderer。
     * @param cl 現フレームのコマンドリスト。
     * @param w 画面の幅 (px)。
     * @param h 画面の高さ (px)。
     */
    void _BeginFrame(FRenderer& r, IRhiCommandList& cl, u32 w, u32 h) noexcept {
        m_Renderer = &r;
        m_Cmd      = &cl;
        m_Width    = w;
        m_Height   = h;
        m_Sprites  = nullptr;
        m_Reflection = nullptr;
        m_StencilMaskActive = false;
        // m_Font は FGame が _BeginFrame 後に _SetFont で配線する (game 寿命で共有)。
    }

    /** フレーム終端で per-frame 参照をクリアする (コマンドリスト等を無効化)。 */
    void _EndFrame() noexcept {
        m_Cmd = nullptr;
        m_Sprites = nullptr;
        m_Font = nullptr;
        m_Reflection = nullptr;
        m_StencilMaskActive = false;
    }

    /**
     * 現フレームの IRhiCommandList を返す。
     *
     * @details nullptr の可能性は OnRender 外でのみ起きる。
     * @return 現フレームのコマンドリスト参照。
     */
    IRhiCommandList& Cmd() const noexcept { return *m_Cmd; }

    /**
     * 描画に使う FRenderer を返す。
     *
     * @return 配線済みの FRenderer 参照。
     */
    FRenderer&        GetRenderer() const noexcept { return *m_Renderer; }

    /**
     * 画面の幅を返す。
     *
     * @return 画面の幅 (px)。
     */
    u32              Width()  const noexcept { return m_Width; }

    /**
     * 画面の高さを返す。
     *
     * @return 画面の高さ (px)。
     */
    u32              Height() const noexcept { return m_Height; }

    /**
     * フレーム描画が進行中か (コマンドリストが配線済みか) を返す。
     *
     * @return 描画中なら true。
     */
    bool             IsFrameActive() const noexcept { return m_Cmd != nullptr; }

    /**
     * 現パス用の 2D 描画バッチを配線する。
     *
     * @details FScene2D または独自ホストが設定する。コンポーネントは SpriteBatch を
     * 自前で持たずに HasSprites()/Sprites() で利用できる。
     * @param sb 配線する SpriteBatch (nullptr で解除)。
     */
    void _SetSpriteBatch(FSpriteBatch* sb) noexcept { m_Sprites = sb; }

    /**
     * 2D 描画バッチが配線済みかを返す。
     *
     * @return 配線済みなら true。
     */
    bool HasSprites() const noexcept { return m_Sprites != nullptr; }

    /**
     * 配線済みの 2D 描画バッチを返す。
     *
     * @return SpriteBatch 参照。
     */
    FSpriteBatch& Sprites() const noexcept { return *m_Sprites; }

    /**
     * 全シーン共有の UI フォントを配線する。
     *
     * @details FGame がロードして毎フレーム配線する。OnDrawHud から
     * sb.DrawString(rc.GetFont(), ...) でテキストを描ける。
     * @param f 配線するフォント (読込失敗時は nullptr で配線され HasFont()==false)。
     */
    void _SetFont(Font* f) noexcept { m_Font = f; }

    /**
     * UI フォントが配線済みかを返す。
     *
     * @return 配線済みなら true (false ならテキストは単に描かれない)。
     */
    bool HasFont() const noexcept { return m_Font != nullptr; }

    /**
     * 配線済みの UI フォントを返す。
     *
     * @return フォント参照。
     */
    Font& GetFont() const noexcept { return *m_Font; }

    /**
     * 反射用シーンテクスチャを配線する。
     *
     * @details FScene2D が world をオフスクリーン RT に焼いて配線する。水が per-vertex
     * 鏡像 UV でこれをサンプルし planar reflection を出す。
     * @param tex 配線する反射テクスチャ (nullptr で解除)。
     */
    void _SetReflection(IRhiTexture* tex) noexcept { m_Reflection = tex; }

    /**
     * 反射テクスチャが配線済みかを返す。
     *
     * @return 配線済みなら true。
     */
    bool HasReflection() const noexcept { return m_Reflection != nullptr; }

    /**
     * 配線済みの反射テクスチャを返す。
     *
     * @return 反射テクスチャ参照。
     */
    IRhiTexture& Reflection() const noexcept { return *m_Reflection; }

    /**
     * 2D world→screen 変換パラメータを配線する。
     *
     * @details 変換は screen_px = (world - ViewCenter) * ViewScale + (Width/2, Height/2)。
     * 水の反射 UV などコンポーネントの screen 投影に使う。
     * @param center world 空間のビュー中心。
     * @param scale world→screen のスケール。
     */
    void _SetView2D(FVec2 center, f32 scale) noexcept { m_ViewCenter = center; m_ViewScale = scale; }

    /**
     * world 空間のビュー中心を返す。
     *
     * @return 配線済みのビュー中心。
     */
    FVec2 ViewCenter() const noexcept { return m_ViewCenter; }

    /**
     * world→screen のスケールを返す。
     *
     * @return 配線済みのスケール。
     */
    f32   ViewScale()  const noexcept { return m_ViewScale; }

    /**
     * ステンシルマスクが有効なパスかを設定する。
     *
     * @param b stencil 付き DSV を bind した world パスなら true。
     */
    void _SetStencilMaskActive(bool b) noexcept { m_StencilMaskActive = b; }

    /**
     * ステンシルマスクが有効なパスかを返す。
     *
     * @details clip コンポーネントはこれを見て、stencil が使えないパス (反射の RT パス等)
     * ではマスクせず素通しする。
     * @return stencil マスクが有効なら true。
     */
    bool StencilMaskActive() const noexcept { return m_StencilMaskActive; }

private:
    /** 描画に使う FRenderer (フレーム冒頭に配線)。 */
    FRenderer*        m_Renderer = nullptr;

    /** 現フレームのコマンドリスト (フレーム外では nullptr)。 */
    IRhiCommandList* m_Cmd      = nullptr;

    /** 現パスの 2D 描画バッチ (未配線なら nullptr)。 */
    FSpriteBatch*    m_Sprites  = nullptr;

    /** 全シーン共有の UI フォント (未配線なら nullptr)。 */
    Font*            m_Font     = nullptr;

    /** 反射用シーンテクスチャ (未配線なら nullptr)。 */
    IRhiTexture*     m_Reflection = nullptr;

    /** world 空間のビュー中心 (world→screen 変換用)。 */
    FVec2            m_ViewCenter{0.0f, 0.0f};

    /** world→screen のスケール。 */
    f32              m_ViewScale = 1.0f;

    /** ステンシルマスクが有効なパスかのフラグ。 */
    bool             m_StencilMaskActive = false;

    /** 画面の幅 (px)。 */
    u32              m_Width    = 0;

    /** 画面の高さ (px)。 */
    u32              m_Height   = 0;
};

} // namespace acs::game
