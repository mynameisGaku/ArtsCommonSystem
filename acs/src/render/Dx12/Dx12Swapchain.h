// SPDX-License-Identifier: Apache-2.0
// DX12 スワップチェイン実装
#pragma once

#include "render/IRhiSwapchain.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

class Dx12Swapchain final : public IRhiSwapchain {
public:
    Dx12Swapchain() noexcept = default;
    ~Dx12Swapchain() noexcept override;

    // 内部使用: バックバッファのリソースおよび RTV ハンドルを取得
    ID3D12Resource*             BackBuffer(u32 i)        const noexcept { return m_BackBuffers[i]; }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV(u32 i)     const noexcept;
    ID3D12DescriptorHeap*       RtvHeap()                const noexcept { return m_RtvHeap; }

    HrResult Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept;

    // IRhiSwapchain
    u32  AcquireNextImage() noexcept override;
    void Present() noexcept override;
    void Resize(u32 width, u32 height) noexcept override;
    u32  BufferCount() const noexcept override { return m_BufferCount; }
    u32  Width()       const noexcept override { return m_Width; }
    u32  Height()      const noexcept override { return m_Height; }

private:
    void ReleaseBuffers() noexcept;
    HrResult AcquireBuffers(Dx12Device& device) noexcept;

    Dx12Device*          m_Device       = nullptr;
    IDXGISwapChain3*     m_Swapchain    = nullptr;
    ID3D12DescriptorHeap* m_RtvHeap    = nullptr;
    u32                  m_RtvSize     = 0;
    static constexpr u32 kMaxBuffers = 3;
    ID3D12Resource*      m_BackBuffers[kMaxBuffers] {};
    u32                  m_BufferCount = 0;
    u32                  m_Width        = 0;
    u32                  m_Height       = 0;
    bool                 m_bVsync        = true;
};

} // namespace acs
