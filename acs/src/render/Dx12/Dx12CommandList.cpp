// SPDX-License-Identifier: Apache-2.0
// DX12 コマンドリスト実装
#include "render/Dx12/Dx12CommandList.h"
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12Swapchain.h"
#include "render/Dx12/Dx12Buffer.h"
#include "render/Dx12/Dx12Pipeline.h"
#include "render/Dx12/Dx12Texture.h"
#include "render/IRhiBuffer.h"
#include "render/IRhiTexture.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

namespace acs {

namespace {

// EPrimitiveTopology → D3D12_PRIMITIVE_TOPOLOGY 変換
D3D12_PRIMITIVE_TOPOLOGY ToD3DPrimitive(EPrimitiveTopology t) noexcept {
    switch (t) {
        case EPrimitiveTopology::PointList:     return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        case EPrimitiveTopology::LineList:      return D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        case EPrimitiveTopology::LineStrip:     return D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        case EPrimitiveTopology::TriangleList:  return D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        case EPrimitiveTopology::TriangleStrip: return D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
    }
    return D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
}

} // namespace

FDx12CommandList::~FDx12CommandList() noexcept {
    // 全 fence の完了を待ってから破棄しないとアロケータ再利用で UB
    if (_device) {
        for (u32 i = 0; i < FDx12Device::kFramesInFlight; ++i) {
            _device->WaitForFenceValue(_frame_fences[i]);
        }
    }
    ACS_SAFE_RELEASE(_cmd_list);
    for (u32 i = 0; i < FDx12Device::kFramesInFlight; ++i) {
        ACS_SAFE_RELEASE(_allocators[i]);
    }
}

FHrResult FDx12CommandList::Init(FDx12Device& device) noexcept {
    FHrResult r{};
    _device = &device;
    // フレームインフライト数ぶんアロケータを作成（GPU/CPU 並列実行のため）
    for (u32 i = 0; i < FDx12Device::kFramesInFlight; ++i) {
        r.hr = device.D3DDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[i]));
        if (r.IsErr()) return r;
        _frame_fences[i] = 0;
    }
    r.hr = device.D3DDevice()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
        IID_PPV_ARGS(&_cmd_list));
    if (r.IsErr()) return r;
    _cmd_list->Close();  // 作成時は Open 状態 → 閉じておく
    return r;
}

void FDx12CommandList::Begin() noexcept {
    const u32 slot = _device->CurrentFrameSlot();
    _allocators[slot]->Reset();
    _cmd_list->Reset(_allocators[slot], nullptr);

    // 共有 SRV ヒープを bind（テクスチャをバインドする際に必要）
    if (_device && _device->SrvHeap()) {
        ID3D12DescriptorHeap* heaps[] = { _device->SrvHeap() };
        _cmd_list->SetDescriptorHeaps(1, heaps);
    }
    _bound_pipe = nullptr;
    _open = true;
}

void FDx12CommandList::End() noexcept {
    if (!_open) return;
    _cmd_list->Close();
    _open = false;
}

void FDx12CommandList::Submit() noexcept {
    if (_open) End();
    ID3D12CommandList* lists[] = { _cmd_list };
    _device->GraphicsQueue()->ExecuteCommandLists(1, lists);

    const u32 cur_slot  = _device->CurrentFrameSlot();
    const u32 next_slot = (cur_slot + 1) % FDx12Device::kFramesInFlight;

    // 1) このフレームの GPU 完了を fence に Signal
    _frame_fences[cur_slot] = _device->SignalGraphicsQueue();

    // 2) 次に使うスロットが GPU で完了するまで待つ。Submit が戻った時点で
    //    「次フレームの OnUpdate で書き込む UPLOAD ヒープスロット」は
    //    GPU から見て開放済み = 競合しない。
    _device->WaitForFenceValue(_frame_fences[next_slot]);

    // 3) Device 側のスロットを切替（リングバッファ化された CB が次スロットを返すように）
    _device->AdvanceFrameSlot();
}

void FDx12CommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                              const FClearColor& clear,
                                              IRhiTexture* depth,
                                              f32 depth_clear) noexcept {
    auto& dx_sc = static_cast<FDx12Swapchain&>(sc);
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

    // 深度バッファのバインド + クリア
    FDx12Texture* dx_depth = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (depth) {
        dx_depth = static_cast<FDx12Texture*>(depth);
        if (dx_depth->IsDepth()) {
            dsv = dx_depth->DsvCpuHandle();
            _cmd_list->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            _cmd_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
                                              depth_clear, 0, 0, nullptr);
        } else {
            _cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
    } else {
        _cmd_list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }

    const FLOAT col[4] = { clear.r, clear.g, clear.b, clear.a };
    _cmd_list->ClearRenderTargetView(rtv, col, 0, nullptr);
}

void FDx12CommandList::EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept {
    auto& dx_sc = static_cast<FDx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    _cmd_list->ResourceBarrier(1, &b);
}

void FDx12CommandList::BeginShadowPass(IRhiTexture& depth, f32 depth_clear) noexcept {
    auto& dx_depth = static_cast<FDx12Texture&>(depth);
    if (!dx_depth.IsDepth()) return;

    // 必要なら状態を DEPTH_WRITE に遷移
    if (dx_depth.CurrentState() != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_depth.Resource();
        b.Transition.StateBefore = dx_depth.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _cmd_list->ResourceBarrier(1, &b);
        dx_depth.SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx_depth.DsvCpuHandle();
    _cmd_list->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    _cmd_list->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth_clear, 0, 0, nullptr);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = static_cast<f32>(dx_depth.Width());
    vp.Height   = static_cast<f32>(dx_depth.Height());
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    _cmd_list->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.left = 0; sr.top = 0;
    sr.right  = static_cast<i32>(dx_depth.Width());
    sr.bottom = static_cast<i32>(dx_depth.Height());
    _cmd_list->RSSetScissorRects(1, &sr);

    _bound_pipe = nullptr;
}

// オフスクリーン RT 用 API（Dx12 raw backend 未実装。Diligent backend を使うこと）
void FDx12CommandList::BeginRenderToTexture(IRhiTexture& /*rt*/, const FClearColor& /*clear*/,
                                            IRhiTexture* /*depth*/, f32 /*depth_clear*/) noexcept {
    // Phase 19 (HDR/Bloom/Tonemap) は Diligent backend 専用機能。
    // -DACS_RENDER_DILIGENT=ON で有効化すること。
}

void FDx12CommandList::EndRenderToTexture(IRhiTexture& /*rt*/) noexcept {}

// Phase 35-3b: SS 屈折用の load 版。Dx12 raw backend は Diligent と同じく未実装。
void FDx12CommandList::BeginRenderToTextureLoad(IRhiTexture& /*rt*/,
                                                IRhiTexture* /*depth*/) noexcept {}

// IBL / cubemap 描画は Diligent backend 専用機能 (Phase 31)。
// Dx12 raw backend には実装しない。
void FDx12CommandList::BeginRenderToTextureSlice(IRhiTexture& /*rt*/,
                                                 u32 /*slice*/, u32 /*mip*/,
                                                 const FClearColor& /*clear*/) noexcept {}

// MRT (Phase 34d-2) も同様、Diligent 専用。Dx12 raw では stub。
// 誤って Dx12 raw backend で MRT を呼んだケースを log で検出可能にする。
void FDx12CommandList::BeginRenderToTextureMrt(IRhiTexture* const* /*rts*/, u32 /*rt_count*/,
                                                const FClearColor& /*clear*/,
                                                IRhiTexture* /*depth*/, f32 /*depth_clear*/) noexcept {
    static bool warned_once = false;
    if (!warned_once) {
        ACS_LOG_WARN("FDx12CommandList::BeginRenderToTextureMrt is not implemented for raw DX12 "
                     "backend (Phase 34d-2 is Diligent-only). Build with -DACS_RENDER_DILIGENT=ON.");
        warned_once = true;
    }
}

void FDx12CommandList::EndShadowPass(IRhiTexture& depth) noexcept {
    auto& dx_depth = static_cast<FDx12Texture&>(depth);
    if (!dx_depth.IsDepth() || !dx_depth.HasSrv()) return;

    if (dx_depth.CurrentState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_depth.Resource();
        b.Transition.StateBefore = dx_depth.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        _cmd_list->ResourceBarrier(1, &b);
        dx_depth.SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void FDx12CommandList::SetViewport(const FViewport& vp) noexcept {
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.min_depth;
    v.MaxDepth = vp.max_depth;
    _cmd_list->RSSetViewports(1, &v);
}

void FDx12CommandList::SetScissor(const FScissorRect& sr) noexcept {
    D3D12_RECT r{};
    r.left = sr.left; r.top = sr.top;
    r.right = sr.right; r.bottom = sr.bottom;
    _cmd_list->RSSetScissorRects(1, &r);
}

void FDx12CommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    auto& p = static_cast<FDx12Pipeline&>(pipeline);
    _cmd_list->SetPipelineState(p.Pso());
    _cmd_list->SetGraphicsRootSignature(p.RootSignature());
    _cmd_list->IASetPrimitiveTopology(ToD3DPrimitive(p.Topology()));
    _bound_pipe = &p;
}

void FDx12CommandList::SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept {
    auto& b = static_cast<FDx12Buffer&>(vb);
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.StrideInBytes  = stride;
    _cmd_list->IASetVertexBuffers(0, 1, &v);
}

void FDx12CommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    auto& b = static_cast<FDx12Buffer&>(ib);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.Format = (b.Usage() == EBufferUsage::Index32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    _cmd_list->IASetIndexBuffer(&v);
}

void FDx12CommandList::SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept {
    if (!_bound_pipe || slot >= _bound_pipe->CBufferSlots()) return;
    auto& b = static_cast<FDx12Buffer&>(cb);
    // ルートパラメータ index = slot（CBV はパラメータの先頭側に並べてある）
    _cmd_list->SetGraphicsRootConstantBufferView(slot, b.Gpu());
}

void FDx12CommandList::SetTexture(u32 slot, IRhiTexture& tex) noexcept {
    if (!_bound_pipe || slot >= _bound_pipe->TextureSlots()) return;
    auto& t = static_cast<FDx12Texture&>(tex);
    // ルートパラメータ index = cbuffer_slots + slot
    const u32 root_index = _bound_pipe->CBufferSlots() + slot;
    _cmd_list->SetGraphicsRootDescriptorTable(root_index, t.SrvGpuHandle());
}

void FDx12CommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    _cmd_list->DrawInstanced(vertex_count, 1, first_vertex, 0);
}

void FDx12CommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    _cmd_list->DrawIndexedInstanced(index_count, 1, first_index, base_vertex, 0);
}

// ファクトリ関数
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 20, "CreateRhiCommandList: device is not DX12");
    FDx12Device* dxd = static_cast<FDx12Device*>(&device);
    auto cl = MakeUnique<FDx12CommandList>();
    FHrResult r = cl->Init(*dxd);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 21, "FDx12CommandList::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return TResult<TUniquePtr<IRhiCommandList>>(OkInit, Move(base));
}
#endif

} // namespace acs
