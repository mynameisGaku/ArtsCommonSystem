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

namespace acs {
class IRhiCommandList;
class FRenderer;
class FSpriteBatch;
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
    }
    void _EndFrame() noexcept {
        m_Cmd = nullptr;
        m_Sprites = nullptr;
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

private:
    FRenderer*        m_Renderer = nullptr;
    IRhiCommandList* m_Cmd      = nullptr;
    FSpriteBatch*    m_Sprites  = nullptr;
    u32              m_Width    = 0;
    u32              m_Height   = 0;
};

} // namespace acs::game
