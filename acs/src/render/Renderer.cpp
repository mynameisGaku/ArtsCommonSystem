// SPDX-License-Identifier: Apache-2.0
// 高レベル FRenderer 実装
#include "render/Renderer.h"
#include "platform/Window.h"
#include "foundation/Move.h"

namespace acs {

FRenderer::~FRenderer() noexcept {
    Shutdown();
}

TResult<void> FRenderer::Init(FWindow& w, bool enable_debug, bool enable_depth) noexcept {
    m_EnableDepth = enable_depth;
    m_ColorFormat = EFormat::B8G8R8A8_UNorm;
    m_DepthFormat = EFormat::D32_Float;

    // デバイス作成
    DeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return Err<void>(dr.Error());
    m_Device = Move(dr.Value());

    // スワップチェイン作成（ウィンドウに紐付け）
    SwapchainConfig scfg{};
    scfg.window = &w;
    scfg.format = m_ColorFormat;
    scfg.buffer_count = 2;
    scfg.vsync = true;
    auto sr = CreateRhiSwapchain(*m_Device, scfg);
    if (sr.IsErr()) return Err<void>(sr.Error());
    m_Swapchain = Move(sr.Value());

    // コマンドリスト作成
    auto cr = CreateRhiCommandList(*m_Device);
    if (cr.IsErr()) return Err<void>(cr.Error());
    m_Cmd = Move(cr.Value());

    // 深度バッファをスワップチェインのサイズで作成
    if (m_EnableDepth) {
        auto rd = RebuildDepth(m_Swapchain->Width(), m_Swapchain->Height());
        if (rd.IsErr()) return rd;
    }

    return Ok();
}

TResult<void> FRenderer::RebuildDepth(u32 w, u32 h) noexcept {
    m_Depth.Reset();
    if (w == 0 || h == 0) return Ok();
    FTextureDesc td{};
    td.width  = w;
    td.height = h;
    td.format = m_DepthFormat;
    td.is_depth_target = true;
    // Phase 34d: SSGI / SSR / TAA で depth を SRV として読みたいので常時 ON。
    // 軽い overhead 程度 (TYPELESS で確保され、Diligent が内部で適切に view を作る)。
    td.shader_visible_depth = true;
    auto tr = CreateRhiTexture(*m_Device, td);
    if (tr.IsErr()) return Err<void>(tr.Error());
    m_Depth = Move(tr.Value());
    return Ok();
}

void FRenderer::Shutdown() noexcept {
    if (m_Device) m_Device->WaitIdle();  // GPU 完了を待ってから解放
    m_Depth.Reset();
    m_Cmd.Reset();
    m_Swapchain.Reset();
    m_Device.Reset();
}

// フレーム開始: バックバッファ取得 → コマンド記録開始 → クリア
void FRenderer::BeginFrame(const ClearColor& clear) noexcept {
    if (!m_Swapchain || !m_Cmd) return;
    m_CurrentBuffer = m_Swapchain->AcquireNextImage();
    m_Cmd->Begin();
    m_Cmd->BeginRenderToSwapchain(*m_Swapchain, m_CurrentBuffer, clear,
                                 m_EnableDepth ? m_Depth.Get() : nullptr,
                                 1.0f);

    // 全画面ビューポート / シザーを既定で設定（描画コードで上書き可能）
    FViewport vp{};
    vp.width  = static_cast<f32>(m_Swapchain->Width());
    vp.height = static_cast<f32>(m_Swapchain->Height());
    m_Cmd->SetViewport(vp);
    FScissorRect sr{};
    sr.right  = static_cast<i32>(m_Swapchain->Width());
    sr.bottom = static_cast<i32>(m_Swapchain->Height());
    m_Cmd->SetScissor(sr);

    m_bFrameOpen = true;
}

// フレーム終了: バックバッファを Present 状態に戻して GPU 投入 → 画面に提示
void FRenderer::EndFrame() noexcept {
    if (!m_bFrameOpen) return;
    m_Cmd->EndRenderToSwapchain(*m_Swapchain, m_CurrentBuffer);
    m_Cmd->End();
    m_Cmd->Submit();
    m_Swapchain->Present();
    m_bFrameOpen = false;
}

void FRenderer::OnResize(u32 width, u32 height) noexcept {
    if (!m_Swapchain) return;
    if (m_Device) m_Device->WaitIdle();
    m_Swapchain->Resize(width, height);
    if (m_EnableDepth) (void)RebuildDepth(width, height);
}

} // namespace acs
