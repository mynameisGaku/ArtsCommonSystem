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
                                const ClearColor& clear) noexcept override;
    void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    void SetViewport(const Viewport& vp) noexcept override;
    void SetScissor (const ScissorRect& sr) noexcept override;

    void SetPipeline(IRhiPipeline& pipeline) noexcept override;
    void SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept override;
    void SetIndexBuffer(IRhiBuffer& ib) noexcept override;
    void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept override;
    void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept override;

    void* NativeHandle() noexcept override { return _cmd_list; }

private:
    Dx12Device*                     _device     = nullptr;
    ID3D12CommandAllocator*         _allocator  = nullptr;
    ID3D12GraphicsCommandList*      _cmd_list   = nullptr;
    bool                            _open       = false;
};

} // namespace acs
