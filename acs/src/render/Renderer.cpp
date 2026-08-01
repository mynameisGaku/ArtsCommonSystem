// SPDX-License-Identifier: Apache-2.0
// 高レベル CRenderer 実装
#include "render/Renderer.h"
#include "platform/Window.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

CRenderer::~CRenderer() noexcept {
    Shutdown();
}

TResult<void> CRenderer::Init(FWindow& w, bool enable_debug, bool enable_depth) noexcept {
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
    FDeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return fail(dr.Error());
    m_Device = Move(dr.Value());

    // スワップチェイン作成（ウィンドウに紐付け）
    FSwapchainConfig scfg{};
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

TResult<void> CRenderer::InitExternal(void* hwnd, u32 width, u32 height,
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
    FDeviceConfig dcfg{};
    dcfg.enable_debug_layer = enable_debug;
    auto dr = CreateRhiDevice(dcfg);
    if (dr.IsErr()) return fail(dr.Error());
    m_Device = Move(dr.Value());

    // スワップチェイン作成（外部 HWND に紐付け）
    FSwapchainConfig scfg{};
    scfg.external_hwnd   = hwnd;
    scfg.external_width  = width;
    scfg.external_height = height;
    scfg.format = m_ColorFormat;
    scfg.buffer_count = 2;
    // vsync は OFF。エディタは公平性を持たせた専用 Win32 message pump から
    // native frame を uncapped で駆動する。Present の垂直同期待ちで UI dispatcher と
    // profiler 更新を止めないよう、外部 HWND は即時 Present (tearing 許容) にする。
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

TResult<void> CRenderer::RebuildDepth(u32 w, u32 h) noexcept {
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

void CRenderer::Shutdown() noexcept {
    // A removed device cannot make forward progress. Waiting in that state can
    // turn an already-reported render failure into an application hang.
    if (m_Device && m_Device->IsOperational())
        m_Device->WaitIdle();  // GPU 完了を待ってから解放
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
bool CRenderer::BeginFrameInternal(
    const FClearColor& clear, bool avoid_gpu_wait) noexcept {
    if (!m_Swapchain || !m_Cmd || m_bFrameOpen) return false;
    if (avoid_gpu_wait) {
        if (!m_Cmd->TryBeginWithoutGpuWait()) return false;
    } else {
        m_Cmd->Begin();
    }
    m_CurrentBuffer = m_Swapchain->AcquireNextImage();
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
    return true;
}

void CRenderer::BeginFrame(const FClearColor& clear) noexcept {
    (void)BeginFrameInternal(clear, false);
}

bool CRenderer::CanBeginFrameWithoutGpuWait() const noexcept {
    return !m_bFrameOpen && m_Swapchain && m_Cmd &&
           m_Cmd->CanBeginWithoutGpuWait();
}

bool CRenderer::IsOperational() const noexcept {
    return m_Device && m_Swapchain && m_Cmd &&
           m_Device->IsOperational();
}

bool CRenderer::TryBeginFrameWithoutGpuWait(
    const FClearColor& clear) noexcept {
    return BeginFrameInternal(clear, true);
}

// フレーム終了: バックバッファを Present 状態に戻して GPU 投入 → 画面に提示
bool CRenderer::EndFrameInternal(bool avoid_gpu_wait) noexcept {
    if (!m_bFrameOpen) return false;
    if (!m_Cmd || !m_Swapchain) {
        m_bFrameOpen = false;
        ACS_LOG_ERROR("CRenderer::EndFrame: frame resources are unavailable");
        return false;
    }
    m_Cmd->EndRenderToSwapchain(*m_Swapchain, m_CurrentBuffer);
    m_Cmd->End();
    const bool submitted = avoid_gpu_wait
        ? m_Cmd->SubmitWithoutGpuWait()
        : m_Cmd->Submit();
    m_bFrameOpen = false;
    if (!submitted) {
        ACS_LOG_ERROR("CRenderer::EndFrame: command submission failed");
        return false;
    }
    if (!m_Swapchain->Present()) {
        ACS_LOG_ERROR("CRenderer::EndFrame: swapchain present failed");
        return false;
    }
    return true;
}

bool CRenderer::EndFrame() noexcept {
    return EndFrameInternal(false);
}

bool CRenderer::EndFrameWithoutGpuWait() noexcept {
    return EndFrameInternal(true);
}

bool CRenderer::OnResize(u32 width, u32 height) noexcept {
    if (!m_Swapchain) return false;
    if (m_Device) m_Device->WaitIdle();
    if (!m_Swapchain->Resize(width, height)) {
        // リサイズ失敗 (バックバッファ未取得状態)。深度再構築や本フレームの描画を行わず、
        // 次フレームの再 Resize に委ねる。BeginRenderToSwapchain 側も null バックバッファを
        // ガードするため、ここで早期 return しても安全に縮退する。
        ACS_LOG_WARN("CRenderer::OnResize: swapchain Resize に失敗 (%ux%u)。次フレームで再試行します。",
                     width, height);
        return false;
    }
    if (m_EnableDepth) {
        const auto depth_result = RebuildDepth(width, height);
        if (depth_result.IsErr()) {
            ACS_LOG_ERROR(
                "CRenderer::OnResize: depth rebuild failed (%ux%u): %s",
                width, height, depth_result.Error().message);
            return false;
        }
    }
    return true;
}

} // namespace acs
