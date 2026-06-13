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

Dx12CommandList::~Dx12CommandList() noexcept {
    // 全 fence の完了を待ってから破棄しないとアロケータ再利用で UB
    if (m_Device) {
        for (u32 i = 0; i < Dx12Device::kFramesInFlight; ++i) {
            m_Device->WaitForFenceValue(m_FrameFences[i]);
        }
    }
    ACS_SAFE_RELEASE(m_CmdList);
    for (u32 i = 0; i < Dx12Device::kFramesInFlight; ++i) {
        ACS_SAFE_RELEASE(_allocators[i]);
    }
}

HrResult Dx12CommandList::Init(Dx12Device& device) noexcept {
    HrResult r{};
    m_Device = &device;
    // フレームインフライト数ぶんアロケータを作成（GPU/CPU 並列実行のため）
    for (u32 i = 0; i < Dx12Device::kFramesInFlight; ++i) {
        r.hr = device.D3DDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[i]));
        if (r.IsErr()) return r;
        m_FrameFences[i] = 0;
    }
    r.hr = device.D3DDevice()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
        IID_PPV_ARGS(&m_CmdList));
    if (r.IsErr()) return r;
    m_CmdList->Close();  // 作成時は Open 状態 → 閉じておく
    return r;
}

void Dx12CommandList::Begin() noexcept {
    const u32 slot = m_Device->CurrentFrameSlot();
    _allocators[slot]->Reset();
    m_CmdList->Reset(_allocators[slot], nullptr);

    // 共有 SRV ヒープを bind（テクスチャをバインドする際に必要）
    if (m_Device && m_Device->SrvHeap()) {
        ID3D12DescriptorHeap* heaps[] = { m_Device->SrvHeap() };
        m_CmdList->SetDescriptorHeaps(1, heaps);
    }
    m_BoundPipe = nullptr;
    _open = true;
}

void Dx12CommandList::End() noexcept {
    if (!_open) return;
    m_CmdList->Close();
    _open = false;
}

void Dx12CommandList::Submit() noexcept {
    if (_open) End();
    ID3D12CommandList* lists[] = { m_CmdList };
    m_Device->GraphicsQueue()->ExecuteCommandLists(1, lists);

    const u32 cur_slot  = m_Device->CurrentFrameSlot();
    const u32 next_slot = (cur_slot + 1) % Dx12Device::kFramesInFlight;

    // 1) このフレームの GPU 完了を fence に Signal
    m_FrameFences[cur_slot] = m_Device->SignalGraphicsQueue();

    // 2) 次に使うスロットが GPU で完了するまで待つ。Submit が戻った時点で
    //    「次フレームの OnUpdate で書き込む UPLOAD ヒープスロット」は
    //    GPU から見て開放済み = 競合しない。
    m_Device->WaitForFenceValue(m_FrameFences[next_slot]);

    // 3) Device 側のスロットを切替（リングバッファ化された CB が次スロットを返すように）
    m_Device->AdvanceFrameSlot();
}

void Dx12CommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                              const ClearColor& clear,
                                              IRhiTexture* depth,
                                              f32 depth_clear) noexcept {
    auto& dx_sc = static_cast<Dx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);
    if (rt == nullptr) {
        // バックバッファ未取得 (Resize 失敗直後 / 範囲外 index)。null リソースで
        // barrier や RTV を積むとクラッシュするため、本フレームのスワップチェイン
        // 描画開始をスキップする。m_BackbufferIsRt は false のままなので End も整合する。
        return;
    }

    // Present → RenderTarget へバリア。ただし既に RT 状態なら skip する。
    // (同一フレーム内でオフスクリーン RT を挟んでから再バインドする —
    //  FScene2D の反射パス等 — を安全にするためのガード)。
    if (!m_BackbufferIsRt) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = rt;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &b);
        m_BackbufferIsRt = true;
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx_sc.BackBufferRTV(buffer_index);

    // 深度バッファのバインド + クリア
    Dx12Texture* dx_depth = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (depth) {
        dx_depth = static_cast<Dx12Texture*>(depth);
        if (dx_depth->IsDepth()) {
            dsv = dx_depth->DsvCpuHandle();
            m_CmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
            UINT cf = D3D12_CLEAR_FLAG_DEPTH;
            if (dx_depth->HasStencil()) cf |= D3D12_CLEAR_FLAG_STENCIL;
            m_CmdList->ClearDepthStencilView(dsv, static_cast<D3D12_CLEAR_FLAGS>(cf),
                                              depth_clear, 0, 0, nullptr);
        } else {
            m_CmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        }
    } else {
        m_CmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }

    const FLOAT col[4] = { clear.r, clear.g, clear.b, clear.a };
    m_CmdList->ClearRenderTargetView(rtv, col, 0, nullptr);
}

void Dx12CommandList::EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept {
    if (!m_BackbufferIsRt) return;   // 既に PRESENT 状態 (二重 End 防止)
    auto& dx_sc = static_cast<Dx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);
    if (rt == nullptr) {             // 念のため: バックバッファ未取得なら状態だけ戻す
        m_BackbufferIsRt = false;
        return;
    }

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = rt;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CmdList->ResourceBarrier(1, &b);
    m_BackbufferIsRt = false;
}

void Dx12CommandList::BeginShadowPass(IRhiTexture& depth, f32 depth_clear) noexcept {
    auto& dx_depth = static_cast<Dx12Texture&>(depth);
    if (!dx_depth.IsDepth()) return;

    // 必要なら状態を DEPTH_WRITE に遷移
    if (dx_depth.CurrentState() != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_depth.Resource();
        b.Transition.StateBefore = dx_depth.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &b);
        dx_depth.SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx_depth.DsvCpuHandle();
    m_CmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
    m_CmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, depth_clear, 0, 0, nullptr);

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = static_cast<f32>(dx_depth.Width());
    vp.Height   = static_cast<f32>(dx_depth.Height());
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    m_CmdList->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.left = 0; sr.top = 0;
    sr.right  = static_cast<i32>(dx_depth.Width());
    sr.bottom = static_cast<i32>(dx_depth.Height());
    m_CmdList->RSSetScissorRects(1, &sr);

    m_BoundPipe = nullptr;
}

// オフスクリーン RT 用 API。RT を RENDER_TARGET に遷移し OMSetRenderTargets で bind、
// viewport / scissor を RT サイズに合わせる。do_clear で clear の有無を切替 (load 版)。
namespace {
void BindOffscreenRT(ID3D12GraphicsCommandList* cmd, Dx12Texture& rt, IRhiTexture* depth,
                     bool do_clear, const ClearColor& clear, f32 depth_clear) noexcept {
    if (rt.CurrentState() != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = rt.Resource();
        b.Transition.StateBefore = rt.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(1, &b);
        rt.SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt.RtvCpuHandle();
    Dx12Texture* dx_depth = depth ? static_cast<Dx12Texture*>(depth) : nullptr;
    if (dx_depth && dx_depth->IsDepth()) {
        if (dx_depth->CurrentState() != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource   = dx_depth->Resource();
            b.Transition.StateBefore = dx_depth->CurrentState();
            b.Transition.StateAfter  = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmd->ResourceBarrier(1, &b);
            dx_depth->SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = dx_depth->DsvCpuHandle();
        cmd->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        if (do_clear) {
            UINT cf = D3D12_CLEAR_FLAG_DEPTH;
            if (dx_depth->HasStencil()) cf |= D3D12_CLEAR_FLAG_STENCIL;
            cmd->ClearDepthStencilView(dsv, static_cast<D3D12_CLEAR_FLAGS>(cf),
                                       depth_clear, 0, 0, nullptr);
        }
    } else {
        cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    }

    if (do_clear) {
        const FLOAT col[4] = { clear.r, clear.g, clear.b, clear.a };
        cmd->ClearRenderTargetView(rtv, col, 0, nullptr);
    }

    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = static_cast<f32>(rt.Width());
    vp.Height   = static_cast<f32>(rt.Height());
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    cmd->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.left = 0; sr.top = 0;
    sr.right  = static_cast<i32>(rt.Width());
    sr.bottom = static_cast<i32>(rt.Height());
    cmd->RSSetScissorRects(1, &sr);
}
} // namespace

void Dx12CommandList::BeginRenderToTexture(IRhiTexture& rt, const ClearColor& clear,
                                            IRhiTexture* depth, f32 depth_clear) noexcept {
    auto& dx_rt = static_cast<Dx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;       // is_render_target=true で作成された RT のみ
    BindOffscreenRT(m_CmdList, dx_rt, depth, true, clear, depth_clear);
    m_BoundPipe = nullptr;             // パイプライン再 bind を強制
}

void Dx12CommandList::EndRenderToTexture(IRhiTexture& rt) noexcept {
    auto& dx_rt = static_cast<Dx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;
    if (dx_rt.CurrentState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_rt.Resource();
        b.Transition.StateBefore = dx_rt.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &b);
        dx_rt.SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

// MSAA RT をバックバッファへ ResolveSubresource で解決する。
// 解決後バックバッファは RENDER_TARGET へ戻し、EndRenderToSwapchain (RT→PRESENT) と整合させる。
void Dx12CommandList::ResolveToSwapchain(IRhiTexture& src, IRhiSwapchain& sc, u32 buffer_index) noexcept {
    auto& dx_src = static_cast<Dx12Texture&>(src);
    auto& dx_sc  = static_cast<Dx12Swapchain&>(sc);
    ID3D12Resource* bb = dx_sc.BackBuffer(buffer_index);
    if (bb == nullptr || dx_src.Resource() == nullptr || dx_src.SampleCount() <= 1) return;

    D3D12_RESOURCE_BARRIER b[2]{};
    u32 n = 0;
    if (dx_src.CurrentState() != D3D12_RESOURCE_STATE_RESOLVE_SOURCE) {
        b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b[n].Transition.pResource   = dx_src.Resource();
        b[n].Transition.StateBefore = dx_src.CurrentState();
        b[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
        b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++n;
        dx_src.SetCurrentState(D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    }
    b[n].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b[n].Transition.pResource   = bb;
    b[n].Transition.StateBefore = m_BackbufferIsRt ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                                   : D3D12_RESOURCE_STATE_PRESENT;
    b[n].Transition.StateAfter  = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    b[n].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    ++n;
    m_CmdList->ResourceBarrier(n, b);

    m_CmdList->ResolveSubresource(bb, 0, dx_src.Resource(), 0, ToDxgiFormat(dx_src.EPixelFormat()));

    D3D12_RESOURCE_BARRIER back{};
    back.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    back.Transition.pResource   = bb;
    back.Transition.StateBefore = D3D12_RESOURCE_STATE_RESOLVE_DEST;
    back.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    back.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CmdList->ResourceBarrier(1, &back);
    m_BackbufferIsRt = true;
}

// SS 屈折用の load 版 (clear せず再 bind)。
void Dx12CommandList::BeginRenderToTextureLoad(IRhiTexture& rt,
                                                IRhiTexture* depth) noexcept {
    auto& dx_rt = static_cast<Dx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;
    BindOffscreenRT(m_CmdList, dx_rt, depth, false, ClearColor{0, 0, 0, 1}, 1.0f);
    m_BoundPipe = nullptr;
}

// cubemap 1 面 / 配列 1 スライス / 1 mip に描画する (per_slice_rtv=true で作成された RT 用)。
// IBL の env/irradiance/prefilter cube の各面・各 roughness mip を焼くのに使う。
void Dx12CommandList::BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                                 const ClearColor& clear) noexcept {
    auto& dx_rt = static_cast<Dx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;  // per_slice_rtv=true で作成された RT のみ

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = dx_rt.RtvCpuHandleForSlice(slice, mip);
    if (rtv.ptr == 0) return;     // 範囲外 slice/mip (作成されていない) は安全にスキップ

    // リソース全体 (全 face/mip サブリソース) を RENDER_TARGET へ遷移。複数 face を続けて
    // 焼く間は 2 回目以降このバリアは skip される (CurrentState が既に RT)。
    if (dx_rt.CurrentState() != D3D12_RESOURCE_STATE_RENDER_TARGET) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_rt.Resource();
        b.Transition.StateBefore = dx_rt.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &b);
        dx_rt.SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    m_CmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const FLOAT col[4] = { clear.r, clear.g, clear.b, clear.a };
    m_CmdList->ClearRenderTargetView(rtv, col, 0, nullptr);

    // viewport / scissor は描画先 mip の寸法に合わせる (prefilter は mip ごとに解像度が下がる)。
    const u32 mw = dx_rt.Width()  >> mip;
    const u32 mh = dx_rt.Height() >> mip;
    const f32 vw = static_cast<f32>(mw > 0u ? mw : 1u);
    const f32 vh = static_cast<f32>(mh > 0u ? mh : 1u);
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width = vw; vp.Height = vh; vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    m_CmdList->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.left = 0; sr.top = 0;
    sr.right = static_cast<i32>(vw); sr.bottom = static_cast<i32>(vh);
    m_CmdList->RSSetScissorRects(1, &sr);

    m_BoundPipe = nullptr;  // パイプライン再 bind を強制 (BeginRenderToTexture と同様)
}

// MRT も同様、Diligent 専用。Dx12 raw では stub。
// 誤って Dx12 raw backend で MRT を呼んだケースを log で検出可能にする。
void Dx12CommandList::BeginRenderToTextureMrt(IRhiTexture* const* /*rts*/, u32 /*rt_count*/,
                                                const ClearColor& /*clear*/,
                                                IRhiTexture* /*depth*/, f32 /*depth_clear*/) noexcept {
    static bool warned_once = false;
    if (!warned_once) {
        ACS_LOG_WARN("Dx12CommandList::BeginRenderToTextureMrt is not implemented for raw DX12 "
                     "backend (Phase 34d-2 is Diligent-only). Build with -DACS_RENDER_DILIGENT=ON.");
        warned_once = true;
    }
}

void Dx12CommandList::EndShadowPass(IRhiTexture& depth) noexcept {
    auto& dx_depth = static_cast<Dx12Texture&>(depth);
    if (!dx_depth.IsDepth() || !dx_depth.HasSrv()) return;

    if (dx_depth.CurrentState() != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = dx_depth.Resource();
        b.Transition.StateBefore = dx_depth.CurrentState();
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &b);
        dx_depth.SetCurrentState(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
}

void Dx12CommandList::SetViewport(const FViewport& vp) noexcept {
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.min_depth;
    v.MaxDepth = vp.max_depth;
    m_CmdList->RSSetViewports(1, &v);
}

void Dx12CommandList::SetScissor(const FScissorRect& sr) noexcept {
    D3D12_RECT r{};
    r.left = sr.left; r.top = sr.top;
    r.right = sr.right; r.bottom = sr.bottom;
    m_CmdList->RSSetScissorRects(1, &r);
}

void Dx12CommandList::SetStencilRef(u32 ref) noexcept {
    m_CmdList->OMSetStencilRef(ref);
}

void Dx12CommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    auto& p = static_cast<Dx12Pipeline&>(pipeline);
    m_CmdList->SetPipelineState(p.Pso());
    m_CmdList->SetGraphicsRootSignature(p.RootSignature());
    m_CmdList->IASetPrimitiveTopology(ToD3DPrimitive(p.Topology()));
    m_BoundPipe = &p;
}

void Dx12CommandList::SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept {
    auto& b = static_cast<Dx12Buffer&>(vb);
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.StrideInBytes  = stride;
    m_CmdList->IASetVertexBuffers(0, 1, &v);
}

void Dx12CommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    auto& b = static_cast<Dx12Buffer&>(ib);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.Format = (b.Usage() == EBufferUsage::Index32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    m_CmdList->IASetIndexBuffer(&v);
}

void Dx12CommandList::SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept {
    if (!m_BoundPipe || slot >= m_BoundPipe->CBufferSlots()) return;
    auto& b = static_cast<Dx12Buffer&>(cb);
    // ルートパラメータ index = slot（CBV はパラメータの先頭側に並べてある）
    m_CmdList->SetGraphicsRootConstantBufferView(slot, b.Gpu());
}

void Dx12CommandList::SetTexture(u32 slot, IRhiTexture& tex) noexcept {
    if (!m_BoundPipe || slot >= m_BoundPipe->TextureSlots()) return;
    auto& t = static_cast<Dx12Texture&>(tex);
    // ルートパラメータ index = cbuffer_slots + slot
    const u32 root_index = m_BoundPipe->CBufferSlots() + slot;
    m_CmdList->SetGraphicsRootDescriptorTable(root_index, t.SrvGpuHandle());
}

void Dx12CommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    m_CmdList->DrawInstanced(vertex_count, 1, first_vertex, 0);
}

void Dx12CommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    m_CmdList->DrawIndexedInstanced(index_count, 1, first_index, base_vertex, 0);
}

// ファクトリ関数
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 20, "CreateRhiCommandList: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto cl = MakeUnique<Dx12CommandList>();
    const HrResult r = cl->Init(*dxd);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 21, "Dx12CommandList::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return TResult<TUniquePtr<IRhiCommandList>>(OkInit, Move(base));
}
#endif

} // namespace acs
