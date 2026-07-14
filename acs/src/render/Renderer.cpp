// SPDX-License-Identifier: Apache-2.0
// 高レベル FRenderer 実装
#include "render/Renderer.h"
#include "platform/Window.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

FRenderer::~FRenderer() noexcept {
    Shutdown();
}

TResult<void> FRenderer::Init(FWindow& w, bool enable_debug, bool enable_depth) noexcept {
    // 再初期化でも、前回の所有物や途中まで作られた状態を持ち越さない。
    Shutdown();

    const auto fail = [this](const FErrorCode& error) noexcept -> TResult<void> {
        Shutdown();
        return Err<void>(error);
    };

    m_EnableDepth = enable_depth;
    m_ColorFormat = EFormat::B8G8R8A8_UNorm;
    m_DepthFormat = EFormat::D32_Float;

    // デバイス作成
    DeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return fail(dr.Error());
    m_Device = Move(dr.Value());

    // スワップチェイン作成（ウィンドウに紐付け）
    SwapchainConfig scfg{};
    scfg.window = &w;
    scfg.format = m_ColorFormat;
    scfg.buffer_count = 2;
    scfg.vsync = true;
    auto sr = CreateRhiSwapchain(*m_Device, scfg);
    if (sr.IsErr()) return fail(sr.Error());
    m_Swapchain = Move(sr.Value());

    // コマンドリスト作成
    auto cr = CreateRhiCommandList(*m_Device);
    if (cr.IsErr()) return fail(cr.Error());
    m_Cmd = Move(cr.Value());

    // 深度バッファをスワップチェインのサイズで作成
    if (m_EnableDepth) {
        auto rd = RebuildDepth(m_Swapchain->Width(), m_Swapchain->Height());
        if (rd.IsErr()) return fail(rd.Error());
    }

    return Ok();
}

TResult<void> FRenderer::InitExternal(void* hwnd, u32 width, u32 height,
                                      bool enable_debug, bool enable_depth) noexcept {
    // 外部 HWND 経路も通常経路と同じく、失敗後は必ず空状態に戻す。
    Shutdown();

    const auto fail = [this](const FErrorCode& error) noexcept -> TResult<void> {
        Shutdown();
        return Err<void>(error);
    };

    m_EnableDepth = enable_depth;
    m_ColorFormat = EFormat::B8G8R8A8_UNorm;
    m_DepthFormat = EFormat::D32_Float;

    // デバイス作成
    DeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return fail(dr.Error());
    m_Device = Move(dr.Value());

    // スワップチェイン作成（外部 HWND に紐付け）
    SwapchainConfig scfg{};
    scfg.external_hwnd   = hwnd;
    scfg.external_width  = width;
    scfg.external_height = height;
    scfg.format = m_ColorFormat;
    scfg.buffer_count = 2;
    // vsync は OFF。エディタ (WPF 等) は UI スレッドの CompositionTarget.Rendering から
    // 描画を駆動するため、vsync 待ちで Present がブロックすると WPF 自身の合成が止まり
    // 画面が白くなる。即時 Present (tearing 許容) で UI スレッドをブロックしない。
    scfg.vsync = false;
    auto sr = CreateRhiSwapchain(*m_Device, scfg);
    if (sr.IsErr()) return fail(sr.Error());
    m_Swapchain = Move(sr.Value());

    // コマンドリスト作成
    auto cr = CreateRhiCommandList(*m_Device);
    if (cr.IsErr()) return fail(cr.Error());
    m_Cmd = Move(cr.Value());

    // 深度バッファをスワップチェインのサイズで作成
    if (m_EnableDepth) {
        auto rd = RebuildDepth(m_Swapchain->Width(), m_Swapchain->Height());
        if (rd.IsErr()) return fail(rd.Error());
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
    // SSGI / SSR / TAA で depth を SRV として読みたいので常時 ON。
    // 軽い overhead 程度 (TYPELESS で確保され、Diligent が内部で適切に view を作る)。
    td.shader_visible_depth = true;
    auto tr = CreateRhiTexture(*m_Device, td);
    if (tr.IsErr()) return Err<void>(tr.Error());
    m_Depth = Move(tr.Value());
    return Ok();
}

void FRenderer::Shutdown() noexcept {
    if (m_Device) m_Device->WaitIdle();  // GPU 完了を待ってから解放
    // Shutdown 中や直後に EndFrame が呼ばれても、解放済みコマンドリストへ触れない。
    m_bFrameOpen = false;
    m_CurrentBuffer = 0;
    m_Depth.Reset();
    m_Cmd.Reset();
    m_Swapchain.Reset();
    m_Device.Reset();
    m_EnableDepth = true;
    m_ColorFormat = EFormat::B8G8R8A8_UNorm;
    m_DepthFormat = EFormat::D32_Float;
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
    if (!m_Swapchain->Resize(width, height)) {
        // リサイズ失敗 (バックバッファ未取得状態)。深度再構築や本フレームの描画を行わず、
        // 次フレームの再 Resize に委ねる。BeginRenderToSwapchain 側も null バックバッファを
        // ガードするため、ここで早期 return しても安全に縮退する。
        ACS_LOG_WARN("FRenderer::OnResize: swapchain Resize に失敗 (%ux%u)。次フレームで再試行します。",
                     width, height);
        return;
    }
    if (m_EnableDepth) (void)RebuildDepth(width, height);
}

} // namespace acs
