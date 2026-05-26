// SPDX-License-Identifier: Apache-2.0
// DiligentCommandList 実装
#include "render/Diligent/DiligentCommandList.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "render/Diligent/DiligentSwapchain.h"
#include "render/Diligent/DiligentPipeline.h"
#include "render/Diligent/DiligentBuffer.h"
#include "render/Diligent/DiligentTexture.h"
#include "foundation/Log.h"

namespace acs {

DiligentCommandList::~DiligentCommandList() noexcept = default;

TResult<void> DiligentCommandList::Init(DiligentDevice& device) noexcept {
    _device = &device;
    return Ok();
}

void DiligentCommandList::Begin() noexcept {
    // Diligent は明示的な Begin/End が不要（IDeviceContext は即時実行）
}

void DiligentCommandList::End() noexcept {
    // Diligent はコマンドが即時積まれるので EOF も不要
}

void DiligentCommandList::Submit() noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    // フレーム終了タイミングで Flush + フェンス Signal
    ctx->Flush();
    _device->SignalGraphicsQueue();
    _device->AdvanceFrameSlot();
}

void DiligentCommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 /*buffer_index*/,
                                                  const ClearColor& clear,
                                                  IRhiTexture* depth,
                                                  f32 depth_clear) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;

    auto& dsc = static_cast<DiligentSwapchain&>(sc);
    auto* swap = dsc.SwapChain();
    if (!swap) return;

    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = depth ? static_cast<DiligentTexture*>(depth)->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    ctx->ClearRenderTarget(rtv, clr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (dsv) {
        ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    // フレーム内で shadow / off-screen pass を挟んだあと復帰するために記憶。
    _main_swapchain = &dsc;
    _main_depth     = depth ? static_cast<DiligentTexture*>(depth) : nullptr;
}

void DiligentCommandList::EndRenderToSwapchain(IRhiSwapchain& /*sc*/, u32 /*buffer_index*/) noexcept {
    // Diligent は Present 時に PRESENT 状態に自動遷移するので何もしない
}

void DiligentCommandList::BeginShadowPass(IRhiTexture& depth, f32 depth_clear) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& d = static_cast<DiligentTexture&>(depth);
    auto* dsv = d.DsvView();
    if (!dsv) return;

    ctx->SetRenderTargets(0, nullptr, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                           Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp;
    vp.width  = static_cast<f32>(d.Width());
    vp.height = static_cast<f32>(d.Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(d.Width());
    sr.bottom = static_cast<i32>(d.Height());
    SetScissor(sr);
}

void DiligentCommandList::EndShadowPass(IRhiTexture& /*depth*/) noexcept {
    // shadow texture の DSV → SRV 遷移は Diligent が次の SetTexture で
    // 自動で行う。ただし RT の復帰はやってくれないので、フレーム冒頭の
    // BeginRenderToSwapchain で記憶した swap chain RTV + main pass DSV を
    // 再 bind する。viewport / scissor も swap chain サイズへ戻す。
    if (!_device || !_main_swapchain) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto* swap = _main_swapchain->SwapChain();
    if (!swap) return;
    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = _main_depth ? _main_depth->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp;
    vp.width  = static_cast<f32>(_main_swapchain->Width());
    vp.height = static_cast<f32>(_main_swapchain->Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(_main_swapchain->Width());
    sr.bottom = static_cast<i32>(_main_swapchain->Height());
    SetScissor(sr);
}

void DiligentCommandList::BeginRenderToTexture(IRhiTexture& rt, const ClearColor& clear,
                                                IRhiTexture* depth, f32 depth_clear) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& t = static_cast<DiligentTexture&>(rt);
    auto* rtv = t.RtvView();
    if (!rtv) return;
    auto* dsv = depth ? static_cast<DiligentTexture*>(depth)->DsvView() : nullptr;

    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    ctx->ClearRenderTarget(rtv, clr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (dsv) {
        ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Viewport vp;
    vp.width  = static_cast<f32>(t.Width());
    vp.height = static_cast<f32>(t.Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(t.Width());
    sr.bottom = static_cast<i32>(t.Height());
    SetScissor(sr);
}

void DiligentCommandList::BeginRenderToTextureLoad(IRhiTexture& rt,
                                                    IRhiTexture* depth) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& t = static_cast<DiligentTexture&>(rt);
    auto* rtv = t.RtvView();
    if (!rtv) return;
    auto* dsv = depth ? static_cast<DiligentTexture*>(depth)->DsvView() : nullptr;

    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    // clear は行わない (load semantics)。viewport / scissor は dst RT サイズに揃える。
    Viewport vp;
    vp.width  = static_cast<f32>(t.Width());
    vp.height = static_cast<f32>(t.Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(t.Width());
    sr.bottom = static_cast<i32>(t.Height());
    SetScissor(sr);
}

void DiligentCommandList::BeginRenderToTextureMrt(IRhiTexture* const* rts, u32 rt_count,
                                                   const ClearColor& clear,
                                                   IRhiTexture* depth,
                                                   f32 depth_clear) noexcept {
    if (!_device || rt_count == 0 || rt_count > 8 || !rts) return;
    auto* ctx = _device->Context();
    if (!ctx) return;

    Diligent::ITextureView* rtvs[8] = {};
    u32 valid_count = 0;
    u32 ref_w = 0, ref_h = 0;
    for (u32 i = 0; i < rt_count; ++i) {
        if (!rts[i]) continue;
        auto* tex = static_cast<DiligentTexture*>(rts[i]);
        auto* rtv = tex->RtvView();
        if (!rtv) continue;
        // 全 RT が同サイズ前提 (Diligent / D3D12 では viewport が 1 つしか付かない、
        // ピクセル単位 raster 範囲は最小 RT のサイズで clip される)。debug build で
        // 検出して strict 違反を early-fail する。
        if (valid_count == 0) {
            ref_w = tex->Width();
            ref_h = tex->Height();
        } else if (tex->Width() != ref_w || tex->Height() != ref_h) {
            ACS_LOG_WARN("BeginRenderToTextureMrt: RT %u size %ux%u != ref %ux%u",
                         i, tex->Width(), tex->Height(), ref_w, ref_h);
        }
        rtvs[valid_count++] = rtv;
    }
    if (valid_count == 0) return;

    auto* dsv = depth ? static_cast<DiligentTexture*>(depth)->DsvView() : nullptr;
    ctx->SetRenderTargets(valid_count, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    for (u32 i = 0; i < valid_count; ++i) {
        ctx->ClearRenderTarget(rtvs[i], clr,
                                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    if (dsv) {
        ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    // Viewport は最初の RT のサイズに合わせる (全 RT が同サイズ前提)
    auto* first = static_cast<DiligentTexture*>(rts[0]);
    Viewport vp;
    vp.width  = static_cast<f32>(first->Width());
    vp.height = static_cast<f32>(first->Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(first->Width());
    sr.bottom = static_cast<i32>(first->Height());
    SetScissor(sr);
}

void DiligentCommandList::BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                                     const ClearColor& clear) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& t = static_cast<DiligentTexture&>(rt);
    auto* rtv = t.RtvSlice(slice, mip);
    if (!rtv) return;

    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, nullptr,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    ctx->ClearRenderTarget(rtv, clr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);

    // mip 描画では viewport も縮める
    u32 w = t.Width();
    u32 h = t.Height();
    for (u32 i = 0; i < mip; ++i) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
    }
    Viewport vp;
    vp.width  = static_cast<f32>(w);
    vp.height = static_cast<f32>(h);
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(w);
    sr.bottom = static_cast<i32>(h);
    SetScissor(sr);
}

void DiligentCommandList::EndRenderToTexture(IRhiTexture& /*rt*/) noexcept {
    // RT texture の RTV → SRV 遷移は Diligent が次の SetTexture で自動。
    // ただし RT 復帰はやってくれないので、main pass に戻したいときは
    // BeginRenderToSwapchain で記憶した swap chain RTV + main pass DSV +
    // viewport を再 bind する (EndShadowPass と同じパターン)。
    // 復帰先が無い場合 (= フレーム冒頭の swap chain bind 前 等) は noop。
    if (!_device || !_main_swapchain) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto* swap = _main_swapchain->SwapChain();
    if (!swap) return;
    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = _main_depth ? _main_depth->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    Viewport vp;
    vp.width  = static_cast<f32>(_main_swapchain->Width());
    vp.height = static_cast<f32>(_main_swapchain->Height());
    SetViewport(vp);
    ScissorRect sr;
    sr.right  = static_cast<i32>(_main_swapchain->Width());
    sr.bottom = static_cast<i32>(_main_swapchain->Height());
    SetScissor(sr);
}

void DiligentCommandList::SetViewport(const Viewport& vp) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    Diligent::Viewport dvp;
    dvp.TopLeftX = vp.x;
    dvp.TopLeftY = vp.y;
    dvp.Width    = vp.width;
    dvp.Height   = vp.height;
    dvp.MinDepth = vp.min_depth;
    dvp.MaxDepth = vp.max_depth;
    ctx->SetViewports(1, &dvp, 0, 0);
}

void DiligentCommandList::SetScissor(const ScissorRect& sr) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    Diligent::Rect r;
    r.left   = sr.left;
    r.top    = sr.top;
    r.right  = sr.right;
    r.bottom = sr.bottom;
    ctx->SetScissorRects(1, &r, 0, 0);
}

void DiligentCommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& p = static_cast<DiligentPipeline&>(pipeline);
    _pipeline = &p;
    if (p.Native()) ctx->SetPipelineState(p.Native());
}

void DiligentCommandList::SetVertexBuffer(IRhiBuffer& vb, u32 /*stride*/) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& b = static_cast<DiligentBuffer&>(vb);
    if (!b.Native()) return;
    Diligent::IBuffer* bufs[1] = { b.Native() };
    Diligent::Uint64   offs[1] = { 0 };
    ctx->SetVertexBuffers(0, 1, bufs, offs,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                          Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
}

void DiligentCommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    if (!_device) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    auto& b = static_cast<DiligentBuffer&>(ib);
    if (!b.Native()) return;
    _is_index32 = (b.Usage() == EBufferUsage::Index32);
    ctx->SetIndexBuffer(b.Native(), 0,
                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void DiligentCommandList::SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept {
    if (!_pipeline || !_device) return;
    auto* srb = _pipeline->Srb();
    if (!srb) return;
    auto& b = static_cast<DiligentBuffer&>(cb);
    if (!b.Native()) return;

    // Pipeline が保持してる名前 (cbuffer_names[slot]) で lookup
    const char* name = _pipeline->CbufferName(slot);
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_VERTEX, name);
    if (var) var->Set(b.Native());
    var = srb->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, name);
    if (var) var->Set(b.Native());
}

void DiligentCommandList::SetTexture(u32 slot, IRhiTexture& tex) noexcept {
    if (!_pipeline || !_device) return;
    auto* srb = _pipeline->Srb();
    if (!srb) return;
    auto& t = static_cast<DiligentTexture&>(tex);
    if (!t.SrvView()) return;

    const char* name = _pipeline->TextureName(slot);
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, name);
    if (var) var->Set(t.SrvView());
}

void DiligentCommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    if (!_device || !_pipeline) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    if (_pipeline->Srb()) {
        ctx->CommitShaderResources(_pipeline->Srb(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Diligent::DrawAttribs da;
    da.NumVertices  = vertex_count;
    da.StartVertexLocation = first_vertex;
    // SetVertexBuffers / SetIndexBuffer / CommitShaderResources を TRANSITION
    // モードで呼んでいるので resource state は自動遷移済。VERIFY_STATES は
    // 「事前 state チェック」で false positive を出す (実遷移より前に検査)
    // ため、production では NONE を使う。
    da.Flags = Diligent::DRAW_FLAG_NONE;
    ctx->Draw(da);
}

void DiligentCommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    if (!_device || !_pipeline) return;
    auto* ctx = _device->Context();
    if (!ctx) return;
    if (_pipeline->Srb()) {
        ctx->CommitShaderResources(_pipeline->Srb(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Diligent::DrawIndexedAttribs dia;
    dia.NumIndices   = index_count;
    dia.IndexType    = _is_index32 ? Diligent::VT_UINT32 : Diligent::VT_UINT16;
    dia.FirstIndexLocation = first_index;
    dia.BaseVertex   = base_vertex;
    dia.Flags = Diligent::DRAW_FLAG_NONE;
    ctx->DrawIndexed(dia);
}

void* DiligentCommandList::NativeHandle() noexcept {
    return _device ? _device->Context() : nullptr;
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
