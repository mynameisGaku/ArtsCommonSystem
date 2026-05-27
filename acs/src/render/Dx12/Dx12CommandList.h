// SPDX-License-Identifier: Apache-2.0
// DX12 コマンドリスト実装
#pragma once

#include "render/IRhiCommandList.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

class Dx12CommandList final : public IRhiCommandList {
public:
    Dx12CommandList() noexcept = default;
    ~Dx12CommandList() noexcept override;

    HrResult Init(Dx12Device& device) noexcept;

    void Begin() noexcept override;
    void End()   noexcept override;
    void Submit() noexcept override;

    void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                const ClearColor& clear,
                                IRhiTexture* depth = nullptr,
                                f32 depth_clear = 1.0f) noexcept override;
    void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    void BeginShadowPass(IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept override;
    void EndShadowPass(IRhiTexture& depth) noexcept override;

    void BeginRenderToTexture(IRhiTexture& rt, const ClearColor& clear,
                              IRhiTexture* depth = nullptr,
                              f32 depth_clear = 1.0f) noexcept override;
    void EndRenderToTexture(IRhiTexture& rt) noexcept override;
    void BeginRenderToTextureLoad(IRhiTexture& rt,
                                   IRhiTexture* depth = nullptr) noexcept override;
    void BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                    const ClearColor& clear) noexcept override;
    void BeginRenderToTextureMrt(IRhiTexture* const* rts, u32 rt_count,
                                  const ClearColor& clear,
                                  IRhiTexture* depth = nullptr,
                                  f32 depth_clear = 1.0f) noexcept override;

    void SetViewport(const FViewport& vp) noexcept override;
    void SetScissor (const FScissorRect& sr) noexcept override;

    void SetPipeline(IRhiPipeline& pipeline) noexcept override;
    void SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept override;
    void SetIndexBuffer(IRhiBuffer& ib) noexcept override;
    void SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept override;
    void SetTexture(u32 slot, IRhiTexture& tex) noexcept override;
    void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept override;
    void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept override;

    void* NativeHandle() noexcept override { return m_CmdList; }

private:
    Dx12Device*                     m_Device      = nullptr;
    // kFramesInFlight 個のアロケータ + 各スロットの GPU 完了 fence 値
    ID3D12CommandAllocator*         _allocators[2] {};   // kFramesInFlight=2
    u64                             m_FrameFences[2] {};
    ID3D12GraphicsCommandList*      m_CmdList    = nullptr;
    class Dx12Pipeline*             m_BoundPipe  = nullptr;
    bool                            _open        = false;
};

} // namespace acs
