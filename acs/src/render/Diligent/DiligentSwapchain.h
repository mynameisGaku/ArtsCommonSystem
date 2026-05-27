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

    TResult<void> Init(DiligentDevice& device, const SwapchainConfig& cfg) noexcept;

    // ---- IRhiSwapchain ----
    u32  AcquireNextImage() noexcept override;
    void Present()          noexcept override;
    void Resize(u32 width, u32 height) noexcept override;
    u32  BufferCount() const noexcept override { return m_BufferCount; }
    u32  Width()       const noexcept override { return m_Width; }
    u32  Height()      const noexcept override { return m_Height; }

    // 内部公開
    Diligent::ISwapChain* SwapChain() const noexcept { return m_Swap; }
    EFormat                ColorFormat() const noexcept { return m_Format; }

private:
    DiligentDevice*       m_Device       = nullptr;
    Diligent::ISwapChain* m_Swap         = nullptr;
    EFormat                m_Format       = EFormat::B8G8R8A8_UNorm;
    u32                   m_Width        = 0;
    u32                   m_Height       = 0;
    u32                   m_BufferCount = 2;
    bool                  m_bVsync        = true;
};

} // namespace acs
