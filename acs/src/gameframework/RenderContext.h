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
        _renderer = &r;
        _cmd      = &cl;
        _width    = w;
        _height   = h;
    }
    void _EndFrame() noexcept {
        _cmd = nullptr;
    }

    // 現フレームの IRhiCommandList (nullptr の可能性は OnRender 外でのみ起きる)。
    IRhiCommandList& Cmd() const noexcept { return *_cmd; }
    FRenderer&        GetRenderer() const noexcept { return *_renderer; }
    u32              Width()  const noexcept { return _width; }
    u32              Height() const noexcept { return _height; }
    bool             IsFrameActive() const noexcept { return _cmd != nullptr; }

private:
    FRenderer*        _renderer = nullptr;
    IRhiCommandList* _cmd      = nullptr;
    u32              _width    = 0;
    u32              _height   = 0;
};

} // namespace acs::game
