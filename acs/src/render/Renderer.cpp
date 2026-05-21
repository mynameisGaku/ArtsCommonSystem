// SPDX-License-Identifier: Apache-2.0
// 高レベル Renderer 実装
#include "render/Renderer.h"
#include "platform/Window.h"
#include "foundation/Move.h"

namespace acs {

Renderer::~Renderer() noexcept {
    Shutdown();
}

Result<void> Renderer::Init(Window& w, bool enable_debug, bool enable_depth) noexcept {
    _enable_depth = enable_depth;
    _color_format = Format::B8G8R8A8_UNorm;
    _depth_format = Format::D32_Float;

    // デバイス作成
    DeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return Err<void>(dr.Error());
    _device = Move(dr.Value());

    // スワップチェイン作成（ウィンドウに紐付け）
    SwapchainConfig scfg{};
    scfg.window = &w;
    scfg.format = _color_format;
    scfg.buffer_count = 2;
    scfg.vsync = true;
    auto sr = CreateRhiSwapchain(*_device, scfg);
    if (sr.IsErr()) return Err<void>(sr.Error());
    _swapchain = Move(sr.Value());

    // コマンドリスト作成
    auto cr = CreateRhiCommandList(*_device);
    if (cr.IsErr()) return Err<void>(cr.Error());
    _cmd = Move(cr.Value());

    // 深度バッファをスワップチェインのサイズで作成
    if (_enable_depth) {
        auto rd = RebuildDepth(_swapchain->Width(), _swapchain->Height());
        if (rd.IsErr()) return rd;
    }

    return Ok();
}

Result<void> Renderer::RebuildDepth(u32 w, u32 h) noexcept {
    _depth.Reset();
    if (w == 0 || h == 0) return Ok();
    TextureDesc td{};
    td.width  = w;
    td.height = h;
    td.format = _depth_format;
    td.is_depth_target = true;
    // Phase 34d: SSGI / SSR / TAA で depth を SRV として読みたいので常時 ON。
    // 軽い overhead 程度 (TYPELESS で確保され、Diligent が内部で適切に view を作る)。
    td.shader_visible_depth = true;
    auto tr = CreateRhiTexture(*_device, td);
    if (tr.IsErr()) return Err<void>(tr.Error());
    _depth = Move(tr.Value());
    return Ok();
}

void Renderer::Shutdown() noexcept {
    if (_device) _device->WaitIdle();  // GPU 完了を待ってから解放
    _depth.Reset();
    _cmd.Reset();
    _swapchain.Reset();
    _device.Reset();
}

// フレーム開始: バックバッファ取得 → コマンド記録開始 → クリア
void Renderer::BeginFrame(const ClearColor& clear) noexcept {
    if (!_swapchain || !_cmd) return;
    _current_buffer = _swapchain->AcquireNextImage();
    _cmd->Begin();
    _cmd->BeginRenderToSwapchain(*_swapchain, _current_buffer, clear,
                                 _enable_depth ? _depth.Get() : nullptr,
                                 1.0f);

    // 全画面ビューポート / シザーを既定で設定（描画コードで上書き可能）
    Viewport vp{};
    vp.width  = static_cast<f32>(_swapchain->Width());
    vp.height = static_cast<f32>(_swapchain->Height());
    _cmd->SetViewport(vp);
    ScissorRect sr{};
    sr.right  = static_cast<i32>(_swapchain->Width());
    sr.bottom = static_cast<i32>(_swapchain->Height());
    _cmd->SetScissor(sr);

    _frame_open = true;
}

// フレーム終了: バックバッファを Present 状態に戻して GPU 投入 → 画面に提示
void Renderer::EndFrame() noexcept {
    if (!_frame_open) return;
    _cmd->EndRenderToSwapchain(*_swapchain, _current_buffer);
    _cmd->End();
    _cmd->Submit();
    _swapchain->Present();
    _frame_open = false;
}

void Renderer::OnResize(u32 width, u32 height) noexcept {
    if (!_swapchain) return;
    if (_device) _device->WaitIdle();
    _swapchain->Resize(width, height);
    if (_enable_depth) (void)RebuildDepth(width, height);
}

} // namespace acs
