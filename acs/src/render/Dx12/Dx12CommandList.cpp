// DX12 コマンドリスト実装
#include "render/Dx12/Dx12CommandList.h"
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12Swapchain.h"
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Pipeline.h"
#include "render/IRhiBuffer.h"
#include "memory/UniquePtr.h"

namespace acs {

namespace {

// PrimitiveTopology → D3D12_PRIMITIVE_TOPOLOGY 変換
D3D12_PRIMITIVE_TOPOLOGY ToD3DPrimitive(PrimitiveTopology t) noexcept {
    switch (t) {
        case PrimitiveTopology::PointList:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case PrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case PrimitiveTopology::LineStrip:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case PrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case PrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

} // namespace

Dx12CommandList::~Dx12CommandList() noexcept {
    ACS_SAFE_RELEASE(_cmd_list);
    ACS_SAFE_RELEASE(_allocator);
}

HrResult Dx12CommandList::Init(Dx12Device& device) noexcept {
    HrResult r{};
    _device = &device;
    r.hr = device.D3DDevice()->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocator));
    if (r.IsErr()) return r;
    r.hr = device.D3DDevice()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocator, nullptr,
        IID_PPV_ARGS(&_cmd_list));
    if (r.IsErr()) return r;
    _cmd_list->Close();  // 作成時は Open 状態 → 閉じておく
    return r;
}

void Dx12CommandList::Begin() noexcept {
    _allocator->Reset();
    _cmd_list->Reset(_allocator, nullptr);
    _open = true;
}

void Dx12CommandList::End() noexcept {
    if (!_open) return;
    _cmd_list->Close();
    _open = false;
}

void Dx12CommandList::Submit() noexcept {
    if (_open) End();
    ID3D12CommandList* lists[] = { _cmd_list };
    _device->GraphicsQueue()->ExecuteCommandLists(1, lists);
    _device->WaitIdle();  // 簡易: GPU 完了を待つ。Fence 化は v2
}

void Dx12CommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                              const ClearColor& clear) noexcept {
    auto& dx_sc = static_cast<Dx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);

    // Present → RenderTarget へバリア
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    _cmd_list->ResourceBarrier(1, &b);

    D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx_sc.BackBufferRTV(buffer_index);
    _cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const FLOAT col[4] = { clear.r, clear.g, clear.b, clear.a };
    _cmd_list->ClearRenderTargetView(rtv, col, 0, nullptr);
}

void Dx12CommandList::EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept {
    auto& dx_sc = static_cast<Dx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    _cmd_list->ResourceBarrier(1, &b);
}

void Dx12CommandList::SetViewport(const Viewport& vp) noexcept {
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.min_depth;
    v.MaxDepth = vp.max_depth;
    _cmd_list->RSSetViewports(1, &v);
}

void Dx12CommandList::SetScissor(const ScissorRect& sr) noexcept {
    D3D12_RECT r{};
    r.left = sr.left; r.top = sr.top;
    r.right = sr.right; r.bottom = sr.bottom;
    _cmd_list->RSSetScissorRects(1, &r);
}

void Dx12CommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    auto& p = static_cast<Dx12Pipeline&>(pipeline);
    _cmd_list->SetPipelineState(p.Pso());
    _cmd_list->SetGraphicsRootSignature(p.RootSignature());
    _cmd_list->IASetPrimitiveTopology(ToD3DPrimitive(p.Topology()));
}

void Dx12CommandList::SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept {
    auto& b = static_cast<Dx12Buffer&>(vb);
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.StrideInBytes  = stride;
    _cmd_list->IASetVertexBuffers(0, 1, &v);
}

void Dx12CommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    auto& b = static_cast<Dx12Buffer&>(ib);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.Format = (b.Usage() == BufferUsage::Index32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    _cmd_list->IASetIndexBuffer(&v);
}

void Dx12CommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    _cmd_list->DrawInstanced(vertex_count, 1, first_vertex, 0);
}

void Dx12CommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    _cmd_list->DrawIndexedInstanced(index_count, 1, first_index, base_vertex, 0);
}

// ファクトリ関数
Result<UniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 20, "CreateRhiCommandList: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto cl = MakeUnique<Dx12CommandList>();
    HrResult r = cl->Init(*dxd);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 21, "Dx12CommandList::Init failed", static_cast<u32>(r.hr));
    }
    UniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return Result<UniquePtr<IRhiCommandList>>(OkInit, Move(base));
}

} // namespace acs
