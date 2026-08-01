// SPDX-License-Identifier: Apache-2.0
// CDiligentCommandList 実装
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

namespace {

f32 TimestampDeltaMilliseconds(
    const Diligent::QueryDataTimestamp& begin,
    const Diligent::QueryDataTimestamp& end) noexcept
{
    const u64 frequency =
        end.Frequency != 0 ? end.Frequency : begin.Frequency;
    if (frequency == 0 || end.Counter < begin.Counter) return -1.0f;
    return static_cast<f32>(
        (static_cast<double>(end.Counter - begin.Counter) * 1000.0) /
        static_cast<double>(frequency));
}

} // namespace

CDiligentCommandList::~CDiligentCommandList() noexcept {
    ResetGpuTiming();
}

TResult<void> CDiligentCommandList::Init(CDiligentDevice& device) noexcept {
    ResetGpuTiming();
    // 再初期化前に、旧 device/resource への借用参照をすべて失効させる。
    for (u32 index = 0; index < 16; ++index)
        m_BoundUavTex[index] = nullptr;
    m_BoundUavTexCount = 0;
    m_Pipeline = nullptr;
    m_bIsIndex32 = false;
    m_MainSwapchain = nullptr;
    m_MainDepth = nullptr;
    m_Device = &device;

    static_assert(
        kGpuTimingFrameSlots == CDiligentDevice::kFramesInFlight,
        "GPU timing slots must follow the Diligent frame ring");
    Diligent::IRenderDevice* render_device = device.RenderDev();
    if (render_device != nullptr &&
        render_device->GetDeviceInfo().Features.TimestampQueries ==
            Diligent::DEVICE_FEATURE_STATE_ENABLED) {
        Diligent::QueryDesc query_desc{Diligent::QUERY_TYPE_TIMESTAMP};
        query_desc.Name = "ACS profiler timestamp";
        m_GpuTimingSupported = true;
        for (u32 slot = 0; slot < kGpuTimingFrameSlots; ++slot) {
            for (u32 query = 0;
                 query < kGpuTimingQueriesPerSlot;
                 ++query) {
                render_device->CreateQuery(
                    query_desc,
                    &m_GpuTimestampQueries[slot][query]);
                if (m_GpuTimestampQueries[slot][query] == nullptr) {
                    ResetGpuTiming();
                    break;
                }
            }
            if (!m_GpuTimingSupported) break;
        }
    }
    return Ok();
}

void CDiligentCommandList::ResetGpuTiming() noexcept {
    for (u32 slot = 0; slot < kGpuTimingFrameSlots; ++slot) {
        for (u32 query = 0;
             query < kGpuTimingQueriesPerSlot;
             ++query) {
            if (m_GpuTimestampQueries[slot][query] != nullptr) {
                m_GpuTimestampQueries[slot][query]->Release();
                m_GpuTimestampQueries[slot][query] = nullptr;
            }
        }
        m_GpuTimingSlots[slot] = {};
    }
    m_LatestGpuTiming = {};
    m_GpuTimingRecordingSlot = 0;
    m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
    m_GpuTimingActivePass = ERhiGpuTimingPass::Opaque;
    m_GpuTimingSupported = false;
    m_GpuTimingRecording = false;
    m_GpuTimingScopeActive = false;
}

void CDiligentCommandList::CollectGpuTiming(u32 slot) noexcept {
    if (!m_GpuTimingSupported || slot >= kGpuTimingFrameSlots) return;
    FGpuTimingSlot& timing_slot = m_GpuTimingSlots[slot];
    if (!timing_slot.pending ||
        timing_slot.query_count < 2 ||
        timing_slot.query_count > kGpuTimingQueriesPerSlot) {
        return;
    }

    // Check every query before invalidating any of them. This keeps the whole
    // frame-slot reusable as one transaction if a driver reports late data.
    for (u32 query = 0; query < timing_slot.query_count; ++query) {
        if (!m_GpuTimestampQueries[slot][query]->GetData(nullptr, 0))
            return;
    }

    Diligent::QueryDataTimestamp
        data[kGpuTimingQueriesPerSlot]{};
    for (u32 query = 0; query < timing_slot.query_count; ++query) {
        if (!m_GpuTimestampQueries[slot][query]->GetData(
                &data[query], sizeof(data[query]), false)) {
            return;
        }
    }
    for (u32 query = 0; query < timing_slot.query_count; ++query)
        m_GpuTimestampQueries[slot][query]->Invalidate();

    FRhiGpuTimingSnapshot completed{};
    completed.frame_index = timing_slot.frame_index;
    completed.frame_ms = TimestampDeltaMilliseconds(
        data[0], data[timing_slot.query_count - 1]);
    if (completed.frame_ms >= 0.0f) {
        completed.valid = true;
        completed.opaque_ms = 0.0f;
        completed.atmosphere_ms = 0.0f;
        completed.cloud_ms = 0.0f;
        completed.fog_ms = 0.0f;
        completed.post_ms = 0.0f;

        for (u32 segment_index = 0;
             segment_index < timing_slot.segment_count;
             ++segment_index) {
            const FGpuTimingSegment& segment =
                timing_slot.segments[segment_index];
            if (segment.begin_query >= timing_slot.query_count ||
                segment.end_query >= timing_slot.query_count) {
                continue;
            }
            const f32 elapsed = TimestampDeltaMilliseconds(
                data[segment.begin_query], data[segment.end_query]);
            if (elapsed < 0.0f) continue;
            switch (segment.pass) {
                case ERhiGpuTimingPass::Opaque:
                    completed.opaque_ms += elapsed;
                    break;
                case ERhiGpuTimingPass::Atmosphere:
                    completed.atmosphere_ms += elapsed;
                    break;
                case ERhiGpuTimingPass::Cloud:
                    completed.cloud_ms += elapsed;
                    break;
                case ERhiGpuTimingPass::Fog:
                    completed.fog_ms += elapsed;
                    break;
                case ERhiGpuTimingPass::Post:
                    completed.post_ms += elapsed;
                    break;
                case ERhiGpuTimingPass::Count:
                    break;
            }
        }
        m_LatestGpuTiming = completed;
    }

    timing_slot.pending = false;
    timing_slot.query_count = 0;
    timing_slot.segment_count = 0;
}

u32 CDiligentCommandList::EmitGpuTimestamp() noexcept {
    if (!m_GpuTimingRecording ||
        m_GpuTimingRecordingSlot >= kGpuTimingFrameSlots) {
        return kInvalidGpuTimingQuery;
    }
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    if (slot.query_count >= kGpuTimingQueriesPerSlot) {
        return kInvalidGpuTimingQuery;
    }
    Diligent::IDeviceContext* context =
        m_Device != nullptr ? m_Device->Context() : nullptr;
    Diligent::IQuery* query =
        m_GpuTimestampQueries[m_GpuTimingRecordingSlot]
                             [slot.query_count];
    if (context == nullptr || query == nullptr) {
        return kInvalidGpuTimingQuery;
    }
    const u32 index = slot.query_count++;
    context->EndQuery(query);
    return index;
}

bool CDiligentCommandList::BeginGpuTimingFrame(
    u64 frame_index) noexcept
{
    if (!m_GpuTimingSupported || m_GpuTimingRecording ||
        m_Device == nullptr || m_Device->Context() == nullptr) {
        return false;
    }
    const u32 slot = m_Device->CurrentFrameSlot();
    if (slot >= kGpuTimingFrameSlots) return false;
    CollectGpuTiming(slot);
    FGpuTimingSlot& timing_slot = m_GpuTimingSlots[slot];
    if (timing_slot.pending) return false;

    timing_slot = {};
    timing_slot.frame_index = frame_index;
    m_GpuTimingRecordingSlot = slot;
    m_GpuTimingRecording = true;
    m_GpuTimingScopeActive = false;
    m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
    if (EmitGpuTimestamp() == kInvalidGpuTimingQuery) {
        m_GpuTimingRecording = false;
        return false;
    }
    return true;
}

bool CDiligentCommandList::BeginGpuTimingPass(
    ERhiGpuTimingPass pass) noexcept
{
    if (!m_GpuTimingRecording || m_GpuTimingScopeActive ||
        pass == ERhiGpuTimingPass::Count) {
        return false;
    }
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    if (slot.segment_count >= kGpuTimingSegmentsPerSlot ||
        slot.query_count + 3u > kGpuTimingQueriesPerSlot) {
        return false;
    }
    const u32 begin = EmitGpuTimestamp();
    if (begin == kInvalidGpuTimingQuery) return false;
    m_GpuTimingActiveBegin = begin;
    m_GpuTimingActivePass = pass;
    m_GpuTimingScopeActive = true;
    return true;
}

void CDiligentCommandList::EndGpuTimingPass() noexcept {
    if (!m_GpuTimingRecording || !m_GpuTimingScopeActive) return;
    const u32 end = EmitGpuTimestamp();
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    if (end != kInvalidGpuTimingQuery &&
        slot.segment_count < kGpuTimingSegmentsPerSlot) {
        slot.segments[slot.segment_count++] = FGpuTimingSegment{
            m_GpuTimingActivePass,
            m_GpuTimingActiveBegin,
            end};
    }
    m_GpuTimingScopeActive = false;
    m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
}

void CDiligentCommandList::EndGpuTimingFrame() noexcept {
    if (!m_GpuTimingRecording) return;
    if (m_GpuTimingScopeActive) EndGpuTimingPass();
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    const u32 end = EmitGpuTimestamp();
    slot.pending =
        end != kInvalidGpuTimingQuery && slot.query_count >= 2u;
    if (!slot.pending) {
        slot.query_count = 0;
        slot.segment_count = 0;
    }
    m_GpuTimingRecording = false;
}

bool CDiligentCommandList::TryGetGpuTiming(
    FRhiGpuTimingSnapshot& out_snapshot) const noexcept
{
    out_snapshot = m_LatestGpuTiming;
    return out_snapshot.valid;
}

void CDiligentCommandList::Begin() noexcept {
    if (m_Device) m_Device->PrepareCommandRecording();
    // PrepareCommandRecording は保留中の off-screen submission を FinishFrame で閉じる。
    // FinishFrame は Diligent context の PSO 状態を消すため、ACS 側の借用キャッシュも
    // 同じ記録境界で失効させ、同一 pipeline を次フレームで必ず再束縛する。
    m_Pipeline = nullptr;
    // This marker is per submission. Keeping an old swapchain here would make a
    // later off-screen preview wait for a Present that will never occur.
    m_MainSwapchain = nullptr;
    m_MainDepth = nullptr;
    // Diligent は明示的な Begin/End が不要（IDeviceContext は即時実行）。
}

bool CDiligentCommandList::CanBeginWithoutGpuWait() const noexcept {
    return m_Device != nullptr &&
           m_Device->CanPrepareCommandRecordingWithoutWait();
}

bool CDiligentCommandList::TryBeginWithoutGpuWait() noexcept {
    if (!CanBeginWithoutGpuWait()) return false;
    Begin();
    return true;
}

void CDiligentCommandList::End() noexcept {
    // Diligent はコマンドが即時積まれるので EOF も不要
}

bool CDiligentCommandList::Submit() noexcept {
    if (!m_Device || !m_Device->IsDeviceHealthy()) return false;
    auto* ctx = m_Device->Context();
    if (!ctx) return false;
    // Flush the real commands first. FinishFrame and its completion fence must
    // happen after this Flush so dynamic allocations are retired in GPU order.
    ctx->Flush();
    if (!m_Device->IsDeviceHealthy()) return false;
    m_Device->MarkFrameSubmitted();
    if (m_MainSwapchain == nullptr)
        return m_Device->FinishPendingSubmittedFrame();
    return true;
}

void CDiligentCommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 /*buffer_index*/,
                                                  const FClearColor& clear,
                                                  IRhiTexture* depth,
                                                  f32 depth_clear) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;

    auto& dsc = static_cast<FDiligentSwapchain&>(sc);
    auto* swap = dsc.SwapChain();
    if (!swap) return;

    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = depth ? static_cast<FDiligentTexture*>(depth)->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    ctx->ClearRenderTarget(rtv, clr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (dsv) {
        const auto cf = (depth && static_cast<FDiligentTexture*>(depth)->HasStencil())
            ? (Diligent::CLEAR_DEPTH_FLAG | Diligent::CLEAR_STENCIL_FLAG)
            : Diligent::CLEAR_DEPTH_FLAG;
        ctx->ClearDepthStencil(dsv, cf, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }

    // フレーム内で shadow / off-screen pass を挟んだあと復帰するために記憶。
    m_MainSwapchain = &dsc;
    m_MainDepth     = depth ? static_cast<FDiligentTexture*>(depth) : nullptr;
}

void CDiligentCommandList::BeginRenderToSwapchainLoad(
    IRhiSwapchain& sc, u32 /*buffer_index*/) noexcept {
    if (!m_Device) return;
    auto* context = m_Device->Context();
    if (!context) return;
    auto& diligent_swapchain =
        static_cast<FDiligentSwapchain&>(sc);
    auto* swapchain = diligent_swapchain.SwapChain();
    if (!swapchain) return;
    auto* render_target =
        swapchain->GetCurrentBackBufferRTV();
    Diligent::ITextureView* render_targets[1] = {
        render_target};
    context->SetRenderTargets(
        1, render_targets, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m_MainSwapchain = &diligent_swapchain;
    m_MainDepth = nullptr;
}

void CDiligentCommandList::EndRenderToSwapchain(IRhiSwapchain& /*sc*/, u32 /*buffer_index*/) noexcept {
    // Diligent は Present 時に PRESENT 状態に自動遷移するので何もしない
}

void CDiligentCommandList::BeginShadowPass(IRhiTexture& depth, f32 depth_clear) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& d = static_cast<FDiligentTexture&>(depth);
    auto* dsv = d.DsvView();
    if (!dsv) return;

    ctx->SetRenderTargets(0, nullptr, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                           Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    FViewport vp;
    vp.width  = static_cast<f32>(d.Width());
    vp.height = static_cast<f32>(d.Height());
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(d.Width());
    sr.bottom = static_cast<i32>(d.Height());
    SetScissor(sr);
}

void CDiligentCommandList::EndShadowPass(IRhiTexture& /*depth*/) noexcept {
    // shadow texture の DSV → SRV 遷移は Diligent が次の SetTexture で
    // 自動で行う。ただし RT の復帰はやってくれないので、フレーム冒頭の
    // BeginRenderToSwapchain で記憶した swap chain RTV + main pass DSV を
    // 再 bind する。viewport / scissor も swap chain サイズへ戻す。
    if (!m_Device || !m_MainSwapchain) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto* swap = m_MainSwapchain->SwapChain();
    if (!swap) return;
    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = m_MainDepth ? m_MainDepth->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    FViewport vp;
    vp.width  = static_cast<f32>(m_MainSwapchain->Width());
    vp.height = static_cast<f32>(m_MainSwapchain->Height());
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(m_MainSwapchain->Width());
    sr.bottom = static_cast<i32>(m_MainSwapchain->Height());
    SetScissor(sr);
}

void CDiligentCommandList::BeginRenderToTexture(IRhiTexture& rt, const FClearColor& clear,
                                                IRhiTexture* depth, f32 depth_clear) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& t = static_cast<FDiligentTexture&>(rt);
    auto* rtv = t.RtvView();
    if (!rtv) return;
    auto* dsv = depth ? static_cast<FDiligentTexture*>(depth)->DsvView() : nullptr;

    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    ctx->ClearRenderTarget(rtv, clr, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    if (dsv) {
        const auto cf = (depth && static_cast<FDiligentTexture*>(depth)->HasStencil())
            ? (Diligent::CLEAR_DEPTH_FLAG | Diligent::CLEAR_STENCIL_FLAG)
            : Diligent::CLEAR_DEPTH_FLAG;
        ctx->ClearDepthStencil(dsv, cf, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    FViewport vp;
    vp.width  = static_cast<f32>(t.Width());
    vp.height = static_cast<f32>(t.Height());
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(t.Width());
    sr.bottom = static_cast<i32>(t.Height());
    SetScissor(sr);
}

void CDiligentCommandList::BeginRenderToTextureLoad(IRhiTexture& rt,
                                                    IRhiTexture* depth) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& t = static_cast<FDiligentTexture&>(rt);
    auto* rtv = t.RtvView();
    if (!rtv) return;
    auto* dsv = depth ? static_cast<FDiligentTexture*>(depth)->DsvView() : nullptr;

    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    // clear は行わない (load semantics)。viewport / scissor は dst RT サイズに揃える。
    FViewport vp;
    vp.width  = static_cast<f32>(t.Width());
    vp.height = static_cast<f32>(t.Height());
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(t.Width());
    sr.bottom = static_cast<i32>(t.Height());
    SetScissor(sr);
}

bool CDiligentCommandList::BeginRenderToTextureMrt(
    IRhiTexture* const* rts, u32 rt_count,
    const FClearColor& clear,
    IRhiTexture* depth,
    f32 depth_clear) noexcept {
    const u32 clear_mask =
        rt_count >= 32u ? 0xffffffffu : ((1u << rt_count) - 1u);
    return BeginRenderToTextureMrtLoad(
        rts, rt_count, clear, clear_mask, depth, true, depth_clear);
}

bool CDiligentCommandList::BeginRenderToTextureMrtLoad(
    IRhiTexture* const* rts, u32 rt_count,
    const FClearColor& clear, u32 clear_mask,
    IRhiTexture* depth, bool clear_depth,
    f32 depth_clear) noexcept {
    if (!m_Device || rt_count == 0 || rt_count > 8 || !rts)
        return false;
    auto* ctx = m_Device->Context();
    if (!ctx) return false;

    Diligent::ITextureView* rtvs[8] = {};
    u32 ref_w = 0, ref_h = 0;
    u32 ref_samples = 0;
    for (u32 i = 0; i < rt_count; ++i) {
        if (!rts[i]) return false;
        auto* tex = static_cast<FDiligentTexture*>(rts[i]);
        auto* rtv = tex->RtvView();
        if (!rtv) return false;
        // 全 RT が同サイズ前提 (Diligent / D3D12 では viewport が 1 つしか付かない、
        // ピクセル単位 raster 範囲は最小 RT のサイズで clip される)。debug build で
        // 検出して strict 違反を early-fail する。
        if (i == 0u) {
            ref_w = tex->Width();
            ref_h = tex->Height();
            ref_samples = tex->SampleCount();
        } else if (tex->Width() != ref_w || tex->Height() != ref_h) {
            ACS_LOG_WARN("BeginRenderToTextureMrt: RT %u size %ux%u != ref %ux%u",
                         i, tex->Width(), tex->Height(), ref_w, ref_h);
            return false;
        } else if (tex->SampleCount() != ref_samples) {
            ACS_LOG_WARN(
                "BeginRenderToTextureMrt: RT %u sample count %u != ref %u",
                i, tex->SampleCount(), ref_samples);
            return false;
        }
        rtvs[i] = rtv;
    }

    auto* depth_texture =
        depth ? static_cast<FDiligentTexture*>(depth) : nullptr;
    auto* dsv = depth_texture ? depth_texture->DsvView() : nullptr;
    if (depth_texture != nullptr
        && (dsv == nullptr
            || depth_texture->Width() != ref_w
            || depth_texture->Height() != ref_h
            || depth_texture->SampleCount() != ref_samples)) {
        return false;
    }
    ctx->SetRenderTargets(rt_count, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    const float clr[4] = { clear.r, clear.g, clear.b, clear.a };
    for (u32 i = 0; i < rt_count; ++i) {
        if ((clear_mask & (1u << i)) != 0u) {
            ctx->ClearRenderTarget(
                rtvs[i], clr,
                Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }
    if (dsv && clear_depth) {
        ctx->ClearDepthStencil(dsv, Diligent::CLEAR_DEPTH_FLAG, depth_clear, 0,
                               Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    // FViewport は最初の有効 RT のサイズに合わせる (全 RT が同サイズ前提)。
    // rts[0] を無条件 deref すると、rts[0] が null / RTV 無しで後続 RT のみ
    // 有効なケース (valid_count>0 だが rts[0] は bind されていない) で
    // null-deref / 不整合サイズになる。ループで既に算出した最初の有効 RT の
    // 寸法 (ref_w/ref_h) を再利用して安全かつ正しい viewport を設定する。
    FViewport vp;
    vp.width  = static_cast<f32>(ref_w);
    vp.height = static_cast<f32>(ref_h);
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(ref_w);
    sr.bottom = static_cast<i32>(ref_h);
    SetScissor(sr);
    return true;
}

void CDiligentCommandList::EndRenderToTextureMrt(
    IRhiTexture* const* /*rts*/, u32 /*rt_count*/) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    // Remove every color/depth output in one call. Diligent transitions each
    // texture to SRV on the next SetTexture with TRANSITION mode.
    ctx->SetRenderTargets(
        0u, nullptr, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m_Pipeline = nullptr;
}

void CDiligentCommandList::BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                                     const FClearColor& clear) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& t = static_cast<FDiligentTexture&>(rt);
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
    FViewport vp;
    vp.width  = static_cast<f32>(w);
    vp.height = static_cast<f32>(h);
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(w);
    sr.bottom = static_cast<i32>(h);
    SetScissor(sr);
}

void CDiligentCommandList::EndRenderToTexture(IRhiTexture& /*rt*/) noexcept {
    // RT texture の RTV → SRV 遷移は Diligent が次の SetTexture で自動。
    // ただし RT 復帰はやってくれないので、main pass に戻したいときは
    // BeginRenderToSwapchain で記憶した swap chain RTV + main pass DSV +
    // viewport を再 bind する (EndShadowPass と同じパターン)。
    // 復帰先が無い場合 (= フレーム冒頭の swap chain bind 前 等) は noop。
    if (!m_Device || !m_MainSwapchain) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto* swap = m_MainSwapchain->SwapChain();
    if (!swap) return;
    auto* rtv = swap->GetCurrentBackBufferRTV();
    auto* dsv = m_MainDepth ? m_MainDepth->DsvView() : nullptr;
    Diligent::ITextureView* rtvs[1] = { rtv };
    ctx->SetRenderTargets(1, rtvs, dsv,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    FViewport vp;
    vp.width  = static_cast<f32>(m_MainSwapchain->Width());
    vp.height = static_cast<f32>(m_MainSwapchain->Height());
    SetViewport(vp);
    FScissorRect sr;
    sr.right  = static_cast<i32>(m_MainSwapchain->Width());
    sr.bottom = static_cast<i32>(m_MainSwapchain->Height());
    SetScissor(sr);
}

void CDiligentCommandList::SetViewport(const FViewport& vp) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
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

void CDiligentCommandList::SetScissor(const FScissorRect& sr) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    Diligent::Rect r;
    r.left   = sr.left;
    r.top    = sr.top;
    r.right  = sr.right;
    r.bottom = sr.bottom;
    ctx->SetScissorRects(1, &r, 0, 0);
}

void CDiligentCommandList::SetStencilRef(u32 ref) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (ctx) ctx->SetStencilRef(ref);
}

void CDiligentCommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& p = static_cast<FDiligentPipeline&>(pipeline);
    m_Pipeline = &p;
    // 全 command list が同じ immediate context を共有する。Diligent 自身の
    // context-global cache に通知し、別 list の bind や Flush 後も正しく再束縛する。
    if (p.Native()) ctx->SetPipelineState(p.Native());
}

void CDiligentCommandList::SetVertexBuffer(IRhiBuffer& vb, u32 /*stride*/) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& b = static_cast<FDiligentBuffer&>(vb);
    if (!b.Native()) return;
    Diligent::IBuffer* bufs[1] = { b.Native() };
    Diligent::Uint64   offs[1] = { 0 };
    ctx->SetVertexBuffers(0, 1, bufs, offs,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION,
                          Diligent::SET_VERTEX_BUFFERS_FLAG_RESET);
}

void CDiligentCommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& b = static_cast<FDiligentBuffer&>(ib);
    if (!b.Native()) return;
    m_bIsIndex32 = (b.Usage() == EBufferUsage::Index32);
    ctx->SetIndexBuffer(b.Native(), 0,
                        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
}

void CDiligentCommandList::SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept {
    if (!m_Pipeline || !m_Device) return;
    auto* srb = m_Pipeline->Srb();
    if (!srb) return;
    /** 論理sliceを解決した実Diligentバッファ。 */
    IRhiBuffer& binding_buffer = cb.BindingBuffer();
    /** 実バッファ先頭からの定数範囲offset。 */
    const usize binding_offset = cb.BindingOffset();
    if (cb.Usage() != EBufferUsage::Uniform || binding_buffer.Usage() != EBufferUsage::Uniform || binding_offset > binding_buffer.Size() || cb.Size() > binding_buffer.Size() - binding_offset || (binding_offset & 255u) != 0u) return;
    /** resource変数へ渡すbackendバッファ。 */
    auto& b = static_cast<FDiligentBuffer&>(binding_buffer);
    if (!b.Native()) return;

    // Pipeline が保持してる名前 (cbuffer_names[slot]) で lookup
    const char* name = m_Pipeline->CbufferName(slot);
    if (m_Pipeline->IsCompute()) {
        auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, name);
        if (var) {
            if (binding_offset == 0u && cb.Size() == binding_buffer.Size()) var->Set(b.Native());
            else var->SetBufferRange(b.Native(), static_cast<Diligent::Uint64>(binding_offset), static_cast<Diligent::Uint64>(cb.Size()));
        }
        return;
    }
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_VERTEX, name);
    if (var) {
        if (binding_offset == 0u && cb.Size() == binding_buffer.Size()) var->Set(b.Native());
        else var->SetBufferRange(b.Native(), static_cast<Diligent::Uint64>(binding_offset), static_cast<Diligent::Uint64>(cb.Size()));
    }
    var = srb->GetVariableByName(Diligent::SHADER_TYPE_PIXEL, name);
    if (var) {
        if (binding_offset == 0u && cb.Size() == binding_buffer.Size()) var->Set(b.Native());
        else var->SetBufferRange(b.Native(), static_cast<Diligent::Uint64>(binding_offset), static_cast<Diligent::Uint64>(cb.Size()));
    }
}

void CDiligentCommandList::SetTexture(u32 slot, IRhiTexture& tex) noexcept {
    if (!m_Pipeline || !m_Device) return;
    auto* srb = m_Pipeline->Srb();
    if (!srb) return;
    auto& t = static_cast<FDiligentTexture&>(tex);
    if (!t.SrvView()) return;

    const char* name = m_Pipeline->TextureName(slot);
    const auto stage = m_Pipeline->IsCompute() ? Diligent::SHADER_TYPE_COMPUTE
                                               : Diligent::SHADER_TYPE_PIXEL;
    auto* var = srb->GetVariableByName(stage, name);
    if (var) var->Set(t.SrvView());
}

bool CDiligentCommandList::CopyDepthTexture(
    IRhiTexture& source,
    IRhiTexture& destination) noexcept {
    if (!m_Device ||
        !IsDepthTextureCopyCompatible(source, destination)) {
        return false;
    }
    auto* context = m_Device->Context();
    if (!context) return false;

    auto& diligent_source =
        static_cast<FDiligentTexture&>(source);
    auto& diligent_destination =
        static_cast<FDiligentTexture&>(destination);
    if (!diligent_source.Native() ||
        !diligent_destination.Native() ||
        diligent_source.Native() == diligent_destination.Native() ||
        !diligent_source.DsvView() ||
        !diligent_destination.DsvView() ||
        !diligent_destination.SrvView()) {
        return false;
    }

    // EndRenderToTexture may restore the main scene DSV. Release all output
    // bindings before asking Diligent to transition it to COPY_SOURCE.
    context->SetRenderTargets(
        0u, nullptr, nullptr,
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    m_Pipeline = nullptr;

    Diligent::CopyTextureAttribs copy{};
    copy.pSrcTexture = diligent_source.Native();
    copy.SrcTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    copy.pDstTexture = diligent_destination.Native();
    copy.DstTextureTransitionMode =
        Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    context->CopyTexture(copy);

    Diligent::StateTransitionDesc after[2] = {
        Diligent::StateTransitionDesc{
            diligent_source.Native(),
            Diligent::RESOURCE_STATE_COPY_SOURCE,
            Diligent::RESOURCE_STATE_DEPTH_WRITE,
            Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE},
        Diligent::StateTransitionDesc{
            diligent_destination.Native(),
            Diligent::RESOURCE_STATE_COPY_DEST,
            Diligent::RESOURCE_STATE_SHADER_RESOURCE,
            Diligent::STATE_TRANSITION_FLAG_UPDATE_STATE},
    };
    context->TransitionResourceStates(2u, after);
    return true;
}

void CDiligentCommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    if (vertex_count == 0u || !m_Device || !m_Pipeline) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    if (m_Pipeline->Srb()) {
        ctx->CommitShaderResources(m_Pipeline->Srb(),
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
    RecordDraw(m_CommandStatistics, vertex_count);
    ctx->Draw(da);
}

void CDiligentCommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    if (index_count == 0u || !m_Device || !m_Pipeline) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    if (m_Pipeline->Srb()) {
        ctx->CommitShaderResources(m_Pipeline->Srb(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Diligent::DrawIndexedAttribs dia;
    dia.NumIndices   = index_count;
    dia.IndexType    = m_bIsIndex32 ? Diligent::VT_UINT32 : Diligent::VT_UINT16;
    dia.FirstIndexLocation = first_index;
    dia.BaseVertex   = base_vertex;
    dia.Flags = Diligent::DRAW_FLAG_NONE;
    RecordDraw(m_CommandStatistics, index_count);
    ctx->DrawIndexed(dia);
}

// ---- Compute (Phase 0) ----

void CDiligentCommandList::SetComputePipeline(IRhiPipeline& pipeline) noexcept {
    if (!m_Device) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& p = static_cast<FDiligentPipeline&>(pipeline);
    m_Pipeline = &p;
    m_BoundUavTexCount = 0;
    // graphics と同じ共有 context のため、Diligent 側を唯一の bind cache とする。
    if (p.Native()) ctx->SetPipelineState(p.Native());
}

void CDiligentCommandList::BindUav(u32 slot, IRhiTexture& tex) noexcept {
    if (!m_Pipeline || !m_Device) return;
    auto* srb = m_Pipeline->Srb();
    if (!srb) return;
    auto& t = static_cast<FDiligentTexture&>(tex);
    if (!t.UavView()) return;
    const char* name = m_Pipeline->UavName(slot);
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, name);
    if (var) var->Set(t.UavView());
}

void CDiligentCommandList::BindUav(u32 slot, IRhiBuffer& buf) noexcept {
    if (!m_Pipeline || !m_Device) return;
    auto* srb = m_Pipeline->Srb();
    if (!srb) return;
    auto& b = static_cast<FDiligentBuffer&>(buf);
    if (!b.UavView()) return;
    const char* name = m_Pipeline->UavName(slot);
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, name);
    if (var) var->Set(b.UavView());
}

void CDiligentCommandList::BindStructuredSrv(u32 slot, IRhiBuffer& buf) noexcept {
    if (!m_Pipeline || !m_Device) return;
    auto* srb = m_Pipeline->Srb();
    if (!srb) return;
    auto& b = static_cast<FDiligentBuffer&>(buf);
    if (!b.SrvView()) return;
    const char* name = m_Pipeline->TextureName(slot);   // SRV は texture スロットと同管理
    auto* var = srb->GetVariableByName(Diligent::SHADER_TYPE_COMPUTE, name);
    if (var) var->Set(b.SrvView());
}

void CDiligentCommandList::Dispatch(u32 gx, u32 gy, u32 gz) noexcept {
    if (gx == 0u || gy == 0u || gz == 0u ||
        !m_Device || !m_Pipeline) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    // TRANSITION モードで commit → Diligent が UAV を UNORDERED_ACCESS、SRV を SHADER_RESOURCE へ
    // 自動遷移する。手動遷移と混ぜると UAV<->SRV state race (旧 SSAO-WIP 全黒) を招くので統一する。
    if (m_Pipeline->Srb()) {
        ctx->CommitShaderResources(m_Pipeline->Srb(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Diligent::DispatchComputeAttribs dca;
    dca.ThreadGroupCountX = gx;
    dca.ThreadGroupCountY = gy;
    dca.ThreadGroupCountZ = gz;
    RecordDispatch(m_CommandStatistics);
    ctx->DispatchCompute(dca);
}

void CDiligentCommandList::DispatchIndirect(IRhiBuffer& args, u32 byte_offset) noexcept {
    if (!m_Device || !m_Pipeline) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;
    auto& a = static_cast<FDiligentBuffer&>(args);
    if (!a.Native()) return;
    // Dispatch と同じく TRANSITION モードで commit → UAV/SRV state を自動整合。
    if (m_Pipeline->Srb()) {
        ctx->CommitShaderResources(m_Pipeline->Srb(),
                                   Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
    Diligent::DispatchComputeIndirectAttribs dcia;
    dcia.pAttribsBuffer                   = a.Native();
    dcia.AttribsBufferStateTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    dcia.DispatchArgsByteOffset           = static_cast<Diligent::Uint64>(byte_offset);
    RecordDispatch(m_CommandStatistics);
    ctx->DispatchComputeIndirect(dcia);
}

void* CDiligentCommandList::NativeHandle() noexcept {
    return m_Device ? m_Device->Context() : nullptr;
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
