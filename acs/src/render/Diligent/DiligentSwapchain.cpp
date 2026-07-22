// SPDX-License-Identifier: Apache-2.0
// FDiligentSwapchain 実装
#include "render/Diligent/DiligentSwapchain.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "platform/Window.h"
#include "foundation/Log.h"

namespace acs {

/** Diligent スワップチェインを解放する。 */
FDiligentSwapchain::~FDiligentSwapchain() noexcept {
    Reset();
}

void FDiligentSwapchain::Reset() noexcept
{
    if (m_Swap) { m_Swap->Release(); m_Swap = nullptr; }
    m_Device = nullptr;
    m_Format = EFormat::B8G8R8A8_UNorm;
    m_Width = 0;
    m_Height = 0;
    m_BufferCount = 2;
    m_bVsync = true;
}

/** ウィンドウに対してバックエンド別 factory でスワップチェインを生成する。 */
TResult<void> FDiligentSwapchain::Init(FDiligentDevice& device, const FSwapchainConfig& cfg) noexcept {
    Reset();
    m_Device = &device;
    m_BufferCount = (cfg.buffer_count >= 2 && cfg.buffer_count <= 3) ? cfg.buffer_count : 2;
    m_bVsync  = cfg.vsync;
    m_Format = cfg.format;
    // window オブジェクト優先。無ければ external_hwnd 経路 (エディタの WPF child HWND attach) を使う。
    void* native_hwnd = cfg.window ? cfg.window->NativeHandle() : cfg.external_hwnd;
    m_Width  = cfg.window ? cfg.window->Width()  : cfg.external_width;
    m_Height = cfg.window ? cfg.window->Height() : cfg.external_height;

    if (!native_hwnd) {
        return ACS_ERR(Render, 110, "FDiligentSwapchain::Init requires a valid window or external_hwnd");
    }

    Diligent::SwapChainDesc sd;
    sd.Width             = m_Width;
    sd.Height            = m_Height;
    sd.ColorBufferFormat = diligent_detail::ToDiligent(m_Format);
    sd.DepthBufferFormat = Diligent::TEX_FORMAT_UNKNOWN;  // 深度は ACS 側で別途管理
    sd.BufferCount       = m_BufferCount;
    sd.PreTransform      = Diligent::SURFACE_TRANSFORM_OPTIMAL;
    sd.Usage             = Diligent::SWAP_CHAIN_USAGE_RENDER_TARGET;
    // Present() が Diligent の FinishFrame()/ReleaseStaleResources() を担う契約を
    // ACS 側でも明示する。off-screen submission は FDiligentDevice が別途閉じる。
    sd.IsPrimary         = true;

    Diligent::Win32NativeWindow win{};
    win.hWnd = native_hwnd;

    Diligent::FullScreenModeDesc fs{};

    auto* dev     = device.RenderDev();
    auto* ctx     = device.Context();
    if (!dev || !ctx) {
        return ACS_ERR(Render, 111, "FDiligentSwapchain: device not initialized");
    }

    if (device.ActualBackend() == ERhiBackendKind::Vulkan) {
#if WITH_RENDER_DILIGENT_VULKAN
        auto* factory = device.FactoryVk();
        if (!factory) return ACS_ERR(Render, 113, "Vulkan factory missing");
        factory->CreateSwapChainVk(dev, ctx, sd, win, &m_Swap);
#else
        return ACS_ERR(Render, 114, "Vulkan backend not built");
#endif
    } else {
        auto* factory = device.Factory();
        if (!factory) return ACS_ERR(Render, 115, "D3D12 factory missing");
        factory->CreateSwapChainD3D12(dev, ctx, sd, fs, win, &m_Swap);
    }
    if (!m_Swap) {
        return ACS_ERR(Render, 112, "CreateSwapChain failed");
    }

    ACS_LOG_INFO("Diligent swapchain created: %ux%u, format=%d, buffers=%u",
                 m_Width, m_Height, static_cast<int>(m_Format), m_BufferCount);
    return Ok();
}

/** バックバッファインデックスを返す (Diligent が内部管理するため常に 0)。 */
u32 FDiligentSwapchain::AcquireNextImage() noexcept {
    // Diligent はバックバッファインデックスを内部管理する。
    // BeginRenderToSwapchain では SetRenderTargets(m_Swap->GetCurrentBackBufferRTV()) を呼ぶ。
    return 0;
}

/** 現在のバックバッファを vsync 設定に従って提示する。 */
void FDiligentSwapchain::Present() noexcept {
    if (!m_Swap) return;
    m_Swap->Present(m_bVsync ? 1 : 0);
    // IsPrimary=true の Present は Diligent 内部で FinishFrame() 済み。
    // 次の Begin/WaitIdle が二重に FinishFrame() しないよう pending を消す。
    if (m_Device) m_Device->NotifyPrimaryPresentFinished();
}

/** スワップチェインを新しいサイズに作り直す。 */
bool FDiligentSwapchain::Resize(u32 width, u32 height) noexcept {
    if (!m_Swap) return false;               // スワップチェイン未初期化 = リサイズ不能
    if (width == 0 || height == 0) return true;  // 無効サイズ要求は no-op
    m_Width  = width;
    m_Height = height;
    m_Swap->Resize(width, height);
    return true;
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
