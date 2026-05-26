// SPDX-License-Identifier: Apache-2.0
// FDiligentSwapchain 実装
#include "render/Diligent/DiligentSwapchain.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "platform/Window.h"
#include "foundation/Log.h"

namespace acs {

FDiligentSwapchain::~FDiligentSwapchain() noexcept {
    if (_swap) { _swap->Release(); _swap = nullptr; }
}

TResult<void> FDiligentSwapchain::Init(FDiligentDevice& device, const FSwapchainConfig& cfg) noexcept {
    _device = &device;
    _buffer_count = (cfg.buffer_count >= 2 && cfg.buffer_count <= 3) ? cfg.buffer_count : 2;
    _vsync  = cfg.vsync;
    _format = cfg.format;
    _width  = cfg.window ? cfg.window->Width()  : 0;
    _height = cfg.window ? cfg.window->Height() : 0;

    if (!cfg.window || !cfg.window->NativeHandle()) {
        return ACS_ERR(Render, 110, "FDiligentSwapchain::Init requires a valid window");
    }

    Diligent::SwapChainDesc sd;
    sd.Width             = _width;
    sd.Height            = _height;
    sd.ColorBufferFormat = diligent_detail::ToDiligent(_format);
    sd.DepthBufferFormat = Diligent::TEX_FORMAT_UNKNOWN;  // 深度は ACS 側で別途管理
    sd.BufferCount       = _buffer_count;
    sd.PreTransform      = Diligent::SURFACE_TRANSFORM_OPTIMAL;
    sd.Usage             = Diligent::SWAP_CHAIN_USAGE_RENDER_TARGET;

    Diligent::Win32NativeWindow win{};
    win.hWnd = cfg.window->NativeHandle();

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
        factory->CreateSwapChainVk(dev, ctx, sd, win, &_swap);
#else
        return ACS_ERR(Render, 114, "Vulkan backend not built");
#endif
    } else {
        auto* factory = device.Factory();
        if (!factory) return ACS_ERR(Render, 115, "D3D12 factory missing");
        factory->CreateSwapChainD3D12(dev, ctx, sd, fs, win, &_swap);
    }
    if (!_swap) {
        return ACS_ERR(Render, 112, "CreateSwapChain failed");
    }

    ACS_LOG_INFO("Diligent swapchain created: %ux%u, format=%d, buffers=%u",
                 _width, _height, static_cast<int>(_format), _buffer_count);
    return Ok();
}

u32 FDiligentSwapchain::AcquireNextImage() noexcept {
    // Diligent はバックバッファインデックスを内部管理する。
    // BeginRenderToSwapchain では SetRenderTargets(_swap->GetCurrentBackBufferRTV()) を呼ぶ。
    return 0;
}

void FDiligentSwapchain::Present() noexcept {
    if (!_swap) return;
    _swap->Present(_vsync ? 1 : 0);
}

void FDiligentSwapchain::Resize(u32 width, u32 height) noexcept {
    if (!_swap) return;
    if (width == 0 || height == 0) return;
    _width  = width;
    _height = height;
    _swap->Resize(width, height);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
