// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の Swapchain
#pragma once

#include "render/IRhiSwapchain.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct ISwapChain;
}

namespace acs {

class DiligentDevice;

class DiligentSwapchain final : public IRhiSwapchain {
public:
    DiligentSwapchain() noexcept = default;
    ~DiligentSwapchain() noexcept override;

    DiligentSwapchain(const DiligentSwapchain&) = delete;
    DiligentSwapchain& operator=(const DiligentSwapchain&) = delete;

    Result<void> Init(DiligentDevice& device, const SwapchainConfig& cfg) noexcept;

    // ---- IRhiSwapchain ----
    u32  AcquireNextImage() noexcept override;
    void Present()          noexcept override;
    void Resize(u32 width, u32 height) noexcept override;
    u32  BufferCount() const noexcept override { return _buffer_count; }
    u32  Width()       const noexcept override { return _width; }
    u32  Height()      const noexcept override { return _height; }

    // 内部公開
    Diligent::ISwapChain* SwapChain() const noexcept { return _swap; }
    Format                ColorFormat() const noexcept { return _format; }

private:
    DiligentDevice*       _device       = nullptr;
    Diligent::ISwapChain* _swap         = nullptr;
    Format                _format       = Format::B8G8R8A8_UNorm;
    u32                   _width        = 0;
    u32                   _height       = 0;
    u32                   _buffer_count = 2;
    bool                  _vsync        = true;
};

} // namespace acs
