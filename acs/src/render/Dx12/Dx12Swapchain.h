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
    ID3D12Resource*             BackBuffer(u32 i)        const noexcept { return _back_buffers[i]; }
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV(u32 i)     const noexcept;
    ID3D12DescriptorHeap*       RtvHeap()                const noexcept { return _rtv_heap; }

    HrResult Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept;

    // IRhiSwapchain
    u32  AcquireNextImage() noexcept override;
    void Present() noexcept override;
    void Resize(u32 width, u32 height) noexcept override;
    u32  BufferCount() const noexcept override { return _buffer_count; }
    u32  Width()       const noexcept override { return _width; }
    u32  Height()      const noexcept override { return _height; }

private:
    void ReleaseBuffers() noexcept;
    HrResult AcquireBuffers(Dx12Device& device) noexcept;

    Dx12Device*          _device       = nullptr;
    IDXGISwapChain3*     _swapchain    = nullptr;
    ID3D12DescriptorHeap* _rtv_heap    = nullptr;
    u32                  _rtv_size     = 0;
    static constexpr u32 kMaxBuffers = 3;
    ID3D12Resource*      _back_buffers[kMaxBuffers] {};
    u32                  _buffer_count = 0;
    u32                  _width        = 0;
    u32                  _height       = 0;
    bool                 _vsync        = true;
};

} // namespace acs
