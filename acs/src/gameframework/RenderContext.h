// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — RenderContext (Phase 1 着手)
//
// 全シーン共有の描画コンテキスト。Phase 1 では「現フレームの IRhiCommandList
// と画面サイズ」を保持するだけの軽量参照ホルダ。Phase 2 で FSpriteBatch /
// Font / 共通シェーダを足して「シーン切替でパイプライン再構築しない」を実現する
// (docs/GameFramework.md §3 末尾)。
//
// Scene 側はこれを OnRender(rc) で受け取り、必要なら rc.Cmd()/Width()/Height()
// から描画コマンドを発行する。素の RHI を直接叩いてもよいし、ユーザーが自分の
// FSpriteBatch を持ってもよい。Phase 1 はそのレベルの最小契約に留める。
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

class RenderContext {
public:
    RenderContext() noexcept = default;
    ~RenderContext() noexcept = default;

    RenderContext(const RenderContext&)            = delete;
    RenderContext& operator=(const RenderContext&) = delete;

    // FGame がフレーム冒頭で配線する。Scene からは Cmd()/FRenderer() で取得。
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
    void _EndFrame() noexcept {
        m_Cmd = nullptr;
        m_Sprites = nullptr;
        m_Font = nullptr;
        m_Reflection = nullptr;
        m_StencilMaskActive = false;
    }

    // 現フレームの IRhiCommandList (nullptr の可能性は OnRender 外でのみ起きる)。
    IRhiCommandList& Cmd() const noexcept { return *m_Cmd; }
    FRenderer&        GetRenderer() const noexcept { return *m_Renderer; }
    u32              Width()  const noexcept { return m_Width; }
    u32              Height() const noexcept { return m_Height; }
    bool             IsFrameActive() const noexcept { return m_Cmd != nullptr; }

    // Optional 2D draw batch installed by FScene2D or a custom host for the
    // current render pass. Components can use HasSprites()/Sprites() without
    // owning a SpriteBatch themselves.
    void _SetSpriteBatch(FSpriteBatch* sb) noexcept { m_Sprites = sb; }
    bool HasSprites() const noexcept { return m_Sprites != nullptr; }
    FSpriteBatch& Sprites() const noexcept { return *m_Sprites; }

    // 全シーン共有の UI フォント (FGame がロードして毎フレーム配線)。
    // OnDrawHud から `sb.DrawString(rc.GetFont(), ...)` でテキストを描ける。
    // フォントが読めなかった環境では HasFont()==false (テキストは単に描かれない)。
    void _SetFont(Font* f) noexcept { m_Font = f; }
    bool HasFont() const noexcept { return m_Font != nullptr; }
    Font& GetFont() const noexcept { return *m_Font; }

    // 反射用シーンテクスチャ (FScene2D が world をオフスクリーン RT に焼いて配線)。
    // 水が per-vertex 鏡像 UV でこれをサンプルして planar reflection を出す。
    void _SetReflection(IRhiTexture* tex) noexcept { m_Reflection = tex; }
    bool HasReflection() const noexcept { return m_Reflection != nullptr; }
    IRhiTexture& Reflection() const noexcept { return *m_Reflection; }

    // 2D world→screen 変換パラメータ (FScene2D が world パスで配線)。
    //   screen_px = (world - ViewCenter) * ViewScale + (Width/2, Height/2)
    // 水の反射 UV など、コンポーネントが screen 投影するのに使う。
    void _SetView2D(FVec2 center, f32 scale) noexcept { m_ViewCenter = center; m_ViewScale = scale; }
    FVec2 ViewCenter() const noexcept { return m_ViewCenter; }
    f32   ViewScale()  const noexcept { return m_ViewScale; }

    // ステンシルマスクが有効なパスか (FScene2D が stencil 付き DSV を bind した
    // world パスでのみ true)。clip コンポーネントはこれを見て、stencil が使えない
    // パス (反射の RT パス等) ではマスクせずに素通しする。
    void _SetStencilMaskActive(bool b) noexcept { m_StencilMaskActive = b; }
    bool StencilMaskActive() const noexcept { return m_StencilMaskActive; }

private:
    FRenderer*        m_Renderer = nullptr;
    IRhiCommandList* m_Cmd      = nullptr;
    FSpriteBatch*    m_Sprites  = nullptr;
    Font*            m_Font     = nullptr;
    IRhiTexture*     m_Reflection = nullptr;
    FVec2            m_ViewCenter{0.0f, 0.0f};
    f32              m_ViewScale = 1.0f;
    bool             m_StencilMaskActive = false;
    u32              m_Width    = 0;
    u32              m_Height   = 0;
};

} // namespace acs::game
