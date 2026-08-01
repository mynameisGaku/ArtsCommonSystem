// SPDX-License-Identifier: Apache-2.0
// DX12 コマンドリスト実装
#include "render/Dx12/Dx12CommandList.h"
#include "render/RhiPipelineBindPolicy.h"
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

void TransitionTexture(ID3D12GraphicsCommandList* cmd, FDx12Texture& texture,
                       D3D12_RESOURCE_STATES target) noexcept {
    if (!cmd || !texture.Resource() || texture.CurrentState() == target) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Resource();
    barrier.Transition.StateBefore = texture.CurrentState();
    barrier.Transition.StateAfter = target;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    texture.SetCurrentState(target);
}

} // namespace

CDx12CommandList::~CDx12CommandList() noexcept {
    Reset(true);
}

void CDx12CommandList::Reset(bool wait_for_gpu) noexcept
{
    // 全 fence の完了を待ってから破棄しないと、投入中のコマンドが
    // 解放済みアロケータを参照する。Init 途中の失敗時は待機不要。
    if (wait_for_gpu && m_Device) {
        for (u32 i = 0; i < CDx12Device::kFramesInFlight; ++i) {
            m_Device->WaitForFenceValue(m_FrameFences[i]);
        }
    }
    ResetGpuTiming();
    ACS_SAFE_RELEASE(m_CmdList);
    for (u32 i = 0; i < CDx12Device::kFramesInFlight; ++i) {
        ACS_SAFE_RELEASE(_allocators[i]);
        m_FrameFences[i] = 0;
    }
    m_Device = nullptr;
    m_BoundPipe = nullptr;
    _open = false;
    m_BackbufferIsRt = false;
}

void CDx12CommandList::ResetGpuTiming() noexcept {
    if (m_GpuTimestampReadback != nullptr &&
        m_GpuTimestampReadbackData != nullptr) {
        D3D12_RANGE written_range{0, 0};
        m_GpuTimestampReadback->Unmap(0, &written_range);
    }
    m_GpuTimestampReadbackData = nullptr;
    ACS_SAFE_RELEASE(m_GpuTimestampReadback);
    ACS_SAFE_RELEASE(m_GpuTimestampHeap);
    m_GpuTimestampFrequency = 0;
    for (u32 slot = 0; slot < kGpuTimingFrameSlots; ++slot)
        m_GpuTimingSlots[slot] = {};
    m_LatestGpuTiming = {};
    m_GpuTimingRecordingSlot = 0;
    m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
    m_GpuTimingActivePass = ERhiGpuTimingPass::Opaque;
    m_GpuTimingSupported = false;
    m_GpuTimingRecording = false;
    m_GpuTimingScopeActive = false;
}

FHrResult CDx12CommandList::Init(CDx12Device& device) noexcept {
    FHrResult r{};
    Reset(true);
    if (!device.D3DDevice() || !device.GraphicsQueue()) {
        r.hr = E_INVALIDARG;
        return r;
    }
    m_Device = &device;
    // フレームインフライト数ぶんアロケータを作成（GPU/CPU 並列実行のため）
    for (u32 i = 0; i < CDx12Device::kFramesInFlight; ++i) {
        r.hr = device.D3DDevice()->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&_allocators[i]));
        if (r.IsErr() || !_allocators[i]) {
            if (r.IsOk()) r.hr = E_FAIL;
            Reset(false);
            return r;
        }
        m_FrameFences[i] = 0;
    }
    r.hr = device.D3DDevice()->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, _allocators[0], nullptr,
        IID_PPV_ARGS(&m_CmdList));
    if (r.IsErr() || !m_CmdList) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset(false);
        return r;
    }
    r.hr = m_CmdList->Close(); // 作成時は Open 状態 → 閉じておく
    if (r.IsErr()) {
        Reset(false);
        return r;
    }

    static_assert(
        kGpuTimingFrameSlots == CDx12Device::kFramesInFlight,
        "GPU timing slots must follow the DX12 frame ring");
    D3D12_QUERY_HEAP_DESC query_heap_desc{};
    query_heap_desc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    query_heap_desc.Count =
        kGpuTimingFrameSlots * kGpuTimingQueriesPerSlot;
    if (SUCCEEDED(device.D3DDevice()->CreateQueryHeap(
            &query_heap_desc,
            __uuidof(ID3D12QueryHeap),
            reinterpret_cast<void**>(&m_GpuTimestampHeap)))) {
        D3D12_HEAP_PROPERTIES heap_properties{};
        heap_properties.Type = D3D12_HEAP_TYPE_READBACK;
        heap_properties.CPUPageProperty =
            D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_properties.MemoryPoolPreference =
            D3D12_MEMORY_POOL_UNKNOWN;
        heap_properties.CreationNodeMask = 1;
        heap_properties.VisibleNodeMask = 1;

        D3D12_RESOURCE_DESC readback_desc{};
        readback_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        readback_desc.Width =
            static_cast<u64>(query_heap_desc.Count) * sizeof(u64);
        readback_desc.Height = 1;
        readback_desc.DepthOrArraySize = 1;
        readback_desc.MipLevels = 1;
        readback_desc.Format = DXGI_FORMAT_UNKNOWN;
        readback_desc.SampleDesc.Count = 1;
        readback_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        if (SUCCEEDED(device.D3DDevice()->CreateCommittedResource(
                &heap_properties,
                D3D12_HEAP_FLAG_NONE,
                &readback_desc,
                D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr,
                __uuidof(ID3D12Resource),
                reinterpret_cast<void**>(
                    &m_GpuTimestampReadback))) &&
            SUCCEEDED(device.GraphicsQueue()->GetTimestampFrequency(
                &m_GpuTimestampFrequency)) &&
            m_GpuTimestampFrequency != 0) {
            D3D12_RANGE read_range{
                0,
                static_cast<SIZE_T>(readback_desc.Width)};
            if (SUCCEEDED(m_GpuTimestampReadback->Map(
                    0,
                    &read_range,
                    reinterpret_cast<void**>(
                        &m_GpuTimestampReadbackData)))) {
                m_GpuTimingSupported = true;
            }
        }
    }
    if (!m_GpuTimingSupported) ResetGpuTiming();
    return r;
}

void CDx12CommandList::CollectGpuTiming(u32 slot) noexcept {
    if (!m_GpuTimingSupported || slot >= kGpuTimingFrameSlots ||
        m_GpuTimestampReadbackData == nullptr ||
        m_GpuTimestampFrequency == 0) {
        return;
    }
    FGpuTimingSlot& timing_slot = m_GpuTimingSlots[slot];
    if (!timing_slot.pending ||
        timing_slot.query_count < 2 ||
        timing_slot.query_count > kGpuTimingQueriesPerSlot) {
        return;
    }

    const u32 base = slot * kGpuTimingQueriesPerSlot;
    const u64* data = m_GpuTimestampReadbackData + base;
    auto elapsed_ms = [this, data](u32 begin, u32 end) noexcept {
        if (end < begin || data[end] < data[begin])
            return -1.0f;
        return static_cast<f32>(
            (static_cast<double>(data[end] - data[begin]) * 1000.0) /
            static_cast<double>(m_GpuTimestampFrequency));
    };

    FRhiGpuTimingSnapshot completed{};
    completed.frame_index = timing_slot.frame_index;
    completed.frame_ms =
        elapsed_ms(0, timing_slot.query_count - 1);
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
            const f32 elapsed =
                elapsed_ms(segment.begin_query, segment.end_query);
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

u32 CDx12CommandList::EmitGpuTimestamp() noexcept {
    if (!m_GpuTimingRecording || !_open ||
        m_CmdList == nullptr ||
        m_GpuTimestampHeap == nullptr ||
        m_GpuTimingRecordingSlot >= kGpuTimingFrameSlots) {
        return kInvalidGpuTimingQuery;
    }
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    if (slot.query_count >= kGpuTimingQueriesPerSlot)
        return kInvalidGpuTimingQuery;
    const u32 local_index = slot.query_count++;
    const u32 query_index =
        m_GpuTimingRecordingSlot * kGpuTimingQueriesPerSlot +
        local_index;
    m_CmdList->EndQuery(
        m_GpuTimestampHeap,
        D3D12_QUERY_TYPE_TIMESTAMP,
        query_index);
    return local_index;
}

bool CDx12CommandList::BeginGpuTimingFrame(
    u64 frame_index) noexcept
{
    if (!m_GpuTimingSupported || m_GpuTimingRecording ||
        !_open || m_Device == nullptr) {
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

bool CDx12CommandList::BeginGpuTimingPass(
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

void CDx12CommandList::EndGpuTimingPass() noexcept {
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

void CDx12CommandList::EndGpuTimingFrame() noexcept {
    if (!m_GpuTimingRecording) return;
    if (m_GpuTimingScopeActive) EndGpuTimingPass();
    FGpuTimingSlot& slot =
        m_GpuTimingSlots[m_GpuTimingRecordingSlot];
    const u32 end = EmitGpuTimestamp();
    slot.pending =
        end != kInvalidGpuTimingQuery && slot.query_count >= 2u;
    if (slot.pending) {
        const u32 base =
            m_GpuTimingRecordingSlot * kGpuTimingQueriesPerSlot;
        m_CmdList->ResolveQueryData(
            m_GpuTimestampHeap,
            D3D12_QUERY_TYPE_TIMESTAMP,
            base,
            slot.query_count,
            m_GpuTimestampReadback,
            static_cast<u64>(base) * sizeof(u64));
    } else {
        slot.query_count = 0;
        slot.segment_count = 0;
    }
    m_GpuTimingRecording = false;
}

bool CDx12CommandList::TryGetGpuTiming(
    FRhiGpuTimingSnapshot& out_snapshot) const noexcept
{
    out_snapshot = m_LatestGpuTiming;
    return out_snapshot.valid;
}

bool CDx12CommandList::BeginCurrentSlot() noexcept {
    if (!m_Device || !m_CmdList) return false;
    const u32 slot = m_Device->CurrentFrameSlot();
    if (slot >= CDx12Device::kFramesInFlight || !_allocators[slot])
        return false;

    m_BoundPipe = nullptr;
    m_BackbufferIsRt = false;
    _open = false;
    if (FAILED(_allocators[slot]->Reset())) return false;
    if (FAILED(m_CmdList->Reset(_allocators[slot], nullptr))) return false;

    // 共有 SRV ヒープを bind（テクスチャをバインドする際に必要）
    if (m_Device && m_Device->SrvHeap()) {
        ID3D12DescriptorHeap* heaps[] = { m_Device->SrvHeap() };
        m_CmdList->SetDescriptorHeaps(1, heaps);
    }
    _open = true;
    return true;
}

void CDx12CommandList::Begin() noexcept {
    (void)BeginCurrentSlot();
}

bool CDx12CommandList::CanBeginWithoutGpuWait() const noexcept {
    if (!m_Device || !m_CmdList) return false;
    const u32 slot = m_Device->CurrentFrameSlot();
    return slot < CDx12Device::kFramesInFlight &&
           _allocators[slot] != nullptr &&
           m_Device->IsFenceComplete(m_FrameFences[slot]);
}

bool CDx12CommandList::TryBeginWithoutGpuWait() noexcept {
    if (!CanBeginWithoutGpuWait()) return false;
    return BeginCurrentSlot();
}

void CDx12CommandList::End() noexcept {
    if (!_open || !m_CmdList) return;
    (void)m_CmdList->Close();
    _open = false;
}

bool CDx12CommandList::SubmitInternal(bool wait_for_next_slot) noexcept {
    if (!m_Device || !m_CmdList || !m_Device->GraphicsQueue()) return false;
    if (_open) End();
    ID3D12CommandList* lists[] = { m_CmdList };

    const u32 cur_slot  = m_Device->CurrentFrameSlot();
    const u32 next_slot = (cur_slot + 1) % CDx12Device::kFramesInFlight;

    // 1) このフレームの GPU 完了を fence に Signal
    // Execute + Signal + retirement sealing are one queue-order transaction.
    // A one-off upload submitted while this list was open cannot claim these
    // retirements; only this main fence covers its recorded references.
    const u64 submitted_fence =
        m_Device->SubmitGraphicsCommandLists(lists, 1u);
    if (submitted_fence == 0u) {
        ACS_LOG_ERROR(
            "CDx12CommandList::Submit: Execute/Signal failed; "
            "frame slot was not advanced");
        return false;
    }
    m_FrameFences[cur_slot] = submitted_fence;

    // 2) 次に使うスロットが GPU で完了するまで待つ。Submit が戻った時点で
    //    「次フレームの OnUpdate で書き込む UPLOAD ヒープスロット」は
    //    GPU から見て開放済み = 競合しない。
    if (wait_for_next_slot)
        m_Device->WaitForFenceValue(m_FrameFences[next_slot]);
    m_Device->CollectRetiredResources();

    // 3) Device 側のスロットを切替（リングバッファ化された CB が次スロットを返すように）
    m_Device->AdvanceFrameSlot();
    return true;
}

bool CDx12CommandList::Submit() noexcept {
    return SubmitInternal(true);
}

bool CDx12CommandList::SubmitWithoutGpuWait() noexcept {
    return SubmitInternal(false);
}

void CDx12CommandList::BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                              const FClearColor& clear,
                                              IRhiTexture* depth,
                                              f32 depth_clear) noexcept {
    auto& dx_sc = static_cast<FDx12Swapchain&>(sc);
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
    FDx12Texture* dx_depth = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (depth) {
        dx_depth = static_cast<FDx12Texture*>(depth);
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

    // ビューポート/シザーをバックバッファ全体へ設定する (BeginShadowPass / BindOffscreenRT と同様)。
    // これが無いと直前パスの viewport が残り、CPostProcess の最終合成 (Pass_Tonemap) 等が
    // 画面の一部にしか描かれない。raw DX12 backend のみこの設定が抜けていた。
    D3D12_VIEWPORT vp{};
    vp.TopLeftX = 0; vp.TopLeftY = 0;
    vp.Width    = static_cast<f32>(dx_sc.Width());
    vp.Height   = static_cast<f32>(dx_sc.Height());
    vp.MinDepth = 0.0f; vp.MaxDepth = 1.0f;
    m_CmdList->RSSetViewports(1, &vp);
    D3D12_RECT sr{};
    sr.left = 0; sr.top = 0;
    sr.right  = static_cast<i32>(dx_sc.Width());
    sr.bottom = static_cast<i32>(dx_sc.Height());
    m_CmdList->RSSetScissorRects(1, &sr);
}

void CDx12CommandList::BeginRenderToSwapchainLoad(
    IRhiSwapchain& sc, u32 buffer_index) noexcept {
    auto& dx_sc = static_cast<FDx12Swapchain&>(sc);
    ID3D12Resource* rt = dx_sc.BackBuffer(buffer_index);
    if (rt == nullptr) return;
    if (!m_BackbufferIsRt) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rt;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_CmdList->ResourceBarrier(1, &barrier);
        m_BackbufferIsRt = true;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        dx_sc.BackBufferRTV(buffer_index);
    m_CmdList->OMSetRenderTargets(
        1, &rtv, FALSE, nullptr);
    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<f32>(dx_sc.Width());
    viewport.Height = static_cast<f32>(dx_sc.Height());
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_CmdList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{};
    scissor.right = static_cast<i32>(dx_sc.Width());
    scissor.bottom = static_cast<i32>(dx_sc.Height());
    m_CmdList->RSSetScissorRects(1, &scissor);
}

void CDx12CommandList::EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept {
    if (!m_BackbufferIsRt) return;   // 既に PRESENT 状態 (二重 End 防止)
    auto& dx_sc = static_cast<FDx12Swapchain&>(sc);
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

void CDx12CommandList::BeginShadowPass(IRhiTexture& depth, f32 depth_clear) noexcept {
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
void BindOffscreenRT(ID3D12GraphicsCommandList* cmd, FDx12Texture& rt, IRhiTexture* depth,
                     bool do_clear, const FClearColor& clear, f32 depth_clear) noexcept {
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
    FDx12Texture* dx_depth = depth ? static_cast<FDx12Texture*>(depth) : nullptr;
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

void CDx12CommandList::BeginRenderToTexture(IRhiTexture& rt, const FClearColor& clear,
                                            IRhiTexture* depth, f32 depth_clear) noexcept {
    auto& dx_rt = static_cast<FDx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;       // is_render_target=true で作成された RT のみ
    BindOffscreenRT(m_CmdList, dx_rt, depth, true, clear, depth_clear);
    m_BoundPipe = nullptr;             // パイプライン再 bind を強制
}

void CDx12CommandList::EndRenderToTexture(IRhiTexture& rt) noexcept {
    auto& dx_rt = static_cast<FDx12Texture&>(rt);
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
void CDx12CommandList::ResolveToSwapchain(IRhiTexture& src, IRhiSwapchain& sc, u32 buffer_index) noexcept {
    auto& dx_src = static_cast<FDx12Texture&>(src);
    auto& dx_sc  = static_cast<FDx12Swapchain&>(sc);
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

    m_CmdList->ResolveSubresource(bb, 0, dx_src.Resource(), 0, ToDxgiFormat(dx_src.PixelFormat()));

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
void CDx12CommandList::BeginRenderToTextureLoad(IRhiTexture& rt,
                                                IRhiTexture* depth) noexcept {
    auto& dx_rt = static_cast<FDx12Texture&>(rt);
    if (!dx_rt.HasRtv()) return;
    BindOffscreenRT(m_CmdList, dx_rt, depth, false, FClearColor{0, 0, 0, 1}, 1.0f);
    m_BoundPipe = nullptr;
}

// cubemap 1 面 / 配列 1 スライス / 1 mip に描画する (per_slice_rtv=true で作成された RT 用)。
// IBL の env/irradiance/prefilter cube の各面・各 roughness mip を焼くのに使う。
void CDx12CommandList::BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                                 const FClearColor& clear) noexcept {
    auto& dx_rt = static_cast<FDx12Texture&>(rt);
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

// 複数 RT を同時に bind する。各 RT は同一寸法で、有効な RTV を持つことが必須。
bool CDx12CommandList::BeginRenderToTextureMrt(
    IRhiTexture* const* rts, u32 rt_count,
    const FClearColor& clear,
    IRhiTexture* depth, f32 depth_clear) noexcept {
    const u32 clear_mask =
        rt_count >= 32u ? 0xffffffffu : ((1u << rt_count) - 1u);
    return BeginRenderToTextureMrtLoad(
        rts, rt_count, clear, clear_mask, depth, true, depth_clear);
}

bool CDx12CommandList::BeginRenderToTextureMrtLoad(
    IRhiTexture* const* rts, u32 rt_count,
    const FClearColor& clear, u32 clear_mask,
    IRhiTexture* depth, bool clear_depth,
    f32 depth_clear) noexcept {
    if (!m_CmdList || !_open || !rts || rt_count == 0u || rt_count > 8u)
        return false;

    FDx12Texture* textures[8]{};
    D3D12_CPU_DESCRIPTOR_HANDLE rtvs[8]{};
    D3D12_RESOURCE_BARRIER barriers[9]{};
    u32 barrier_count = 0u;
    u32 width = 0u;
    u32 height = 0u;
    u32 sample_count = 0u;

    // Validate the complete output set before recording any barriers or
    // changing backend state. A false result is therefore safe for callers
    // that immediately select a fallback pass.
    for (u32 i = 0; i < rt_count; ++i) {
        if (!rts[i]) return false;
        auto& rt = static_cast<FDx12Texture&>(*rts[i]);
        if (!rt.HasRtv() || !rt.Resource()) return false;
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = rt.RtvCpuHandle();
        if (rtv.ptr == 0u) return false;
        if (i == 0u) {
            width = rt.Width();
            height = rt.Height();
            sample_count = rt.SampleCount();
        } else if (rt.Width() != width || rt.Height() != height) {
            ACS_LOG_WARN("Dx12CommandList::BeginRenderToTextureMrt: RT %u size "
                         "%ux%u != RT0 %ux%u",
                         i, rt.Width(), rt.Height(), width, height);
            return false;
        } else if (rt.SampleCount() != sample_count) {
            ACS_LOG_WARN(
                "Dx12CommandList::BeginRenderToTextureMrt: RT %u sample "
                "count %u != RT0 sample count %u",
                i, rt.SampleCount(), sample_count);
            return false;
        }
        textures[i] = &rt;
        rtvs[i] = rtv;
    }

    FDx12Texture* dx_depth = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
    if (depth != nullptr) {
        dx_depth = static_cast<FDx12Texture*>(depth);
        if (!dx_depth->IsDepth() || !dx_depth->Resource()) return false;
        dsv = dx_depth->DsvCpuHandle();
        if (dsv.ptr == 0u) return false;
        if (dx_depth->Width() != width || dx_depth->Height() != height
            || dx_depth->SampleCount() != sample_count) {
            ACS_LOG_WARN(
                "Dx12CommandList::BeginRenderToTextureMrt: depth "
                "%ux%u samples=%u != RT0 %ux%u samples=%u",
                dx_depth->Width(), dx_depth->Height(),
                dx_depth->SampleCount(), width, height, sample_count);
            return false;
        }
    }

    for (u32 i = 0; i < rt_count; ++i) {
        auto& rt = *textures[i];
        if (rt.CurrentState() != D3D12_RESOURCE_STATE_RENDER_TARGET) {
            auto& barrier = barriers[barrier_count++];
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = rt.Resource();
            barrier.Transition.StateBefore = rt.CurrentState();
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            rt.SetCurrentState(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    if (dx_depth) {
        if (dx_depth->CurrentState() != D3D12_RESOURCE_STATE_DEPTH_WRITE) {
            auto& barrier = barriers[barrier_count++];
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = dx_depth->Resource();
            barrier.Transition.StateBefore = dx_depth->CurrentState();
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            dx_depth->SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
        }
    }

    if (barrier_count > 0u) {
        m_CmdList->ResourceBarrier(barrier_count, barriers);
    }
    m_CmdList->OMSetRenderTargets(
        rt_count, rtvs, FALSE, dx_depth ? &dsv : nullptr);

    const FLOAT color[4] = {clear.r, clear.g, clear.b, clear.a};
    for (u32 i = 0; i < rt_count; ++i) {
        if ((clear_mask & (1u << i)) != 0u) {
            m_CmdList->ClearRenderTargetView(rtvs[i], color, 0, nullptr);
        }
    }
    if (dx_depth && clear_depth) {
        UINT clear_flags = D3D12_CLEAR_FLAG_DEPTH;
        if (dx_depth->HasStencil()) clear_flags |= D3D12_CLEAR_FLAG_STENCIL;
        m_CmdList->ClearDepthStencilView(
            dsv, static_cast<D3D12_CLEAR_FLAGS>(clear_flags),
            depth_clear, 0, 0, nullptr);
    }

    D3D12_VIEWPORT viewport{};
    viewport.Width = static_cast<f32>(width);
    viewport.Height = static_cast<f32>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    m_CmdList->RSSetViewports(1, &viewport);
    D3D12_RECT scissor{};
    scissor.right = static_cast<i32>(width);
    scissor.bottom = static_cast<i32>(height);
    m_CmdList->RSSetScissorRects(1, &scissor);
    m_BoundPipe = nullptr;
    return true;
}

void CDx12CommandList::EndRenderToTextureMrt(
    IRhiTexture* const* rts, u32 rt_count) noexcept {
    if (!m_CmdList || !rts || rt_count == 0u || rt_count > 8u) return;

    // Drop the complete OM binding before transitioning any member of it.
    // Sequential EndRenderToTexture calls leave the other descriptors bound
    // while RT0 has already become an SRV, which is invalid under the D3D12
    // output-merger state contract.
    m_CmdList->OMSetRenderTargets(0u, nullptr, FALSE, nullptr);

    D3D12_RESOURCE_BARRIER barriers[8]{};
    u32 barrier_count = 0u;
    for (u32 i = 0u; i < rt_count; ++i) {
        if (!rts[i]) continue;
        auto& rt = static_cast<FDx12Texture&>(*rts[i]);
        if (!rt.HasRtv() || !rt.Resource() ||
            rt.CurrentState() ==
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE) {
            continue;
        }
        auto& barrier = barriers[barrier_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = rt.Resource();
        barrier.Transition.StateBefore = rt.CurrentState();
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        rt.SetCurrentState(
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    if (barrier_count > 0u) {
        m_CmdList->ResourceBarrier(barrier_count, barriers);
    }
    m_BoundPipe = nullptr;
}

void CDx12CommandList::EndShadowPass(IRhiTexture& depth) noexcept {
    auto& dx_depth = static_cast<FDx12Texture&>(depth);
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

void CDx12CommandList::SetViewport(const FViewport& vp) noexcept {
    D3D12_VIEWPORT v{};
    v.TopLeftX = vp.x;
    v.TopLeftY = vp.y;
    v.Width    = vp.width;
    v.Height   = vp.height;
    v.MinDepth = vp.min_depth;
    v.MaxDepth = vp.max_depth;
    m_CmdList->RSSetViewports(1, &v);
}

void CDx12CommandList::SetScissor(const FScissorRect& sr) noexcept {
    D3D12_RECT r{};
    r.left = sr.left; r.top = sr.top;
    r.right = sr.right; r.bottom = sr.bottom;
    m_CmdList->RSSetScissorRects(1, &r);
}

void CDx12CommandList::SetStencilRef(u32 ref) noexcept {
    m_CmdList->OMSetStencilRef(ref);
}

void CDx12CommandList::SetPipeline(IRhiPipeline& pipeline) noexcept {
    auto& p = static_cast<FDx12Pipeline&>(pipeline);
    if (p.IsCompute()) {
        SetComputePipeline(pipeline);
        return;
    }
    if (!TRhiPipelineBindPolicy<ERhiPipelineBindDomain::Graphics>::NeedsBind(m_BoundPipe, &p)) {
        return;
    }
    m_CmdList->SetPipelineState(p.Pso());
    m_CmdList->SetGraphicsRootSignature(p.RootSignature());
    m_CmdList->IASetPrimitiveTopology(ToD3DPrimitive(p.Topology()));
    m_BoundPipe = &p;
}

void CDx12CommandList::SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept {
    auto& b = static_cast<FDx12Buffer&>(vb);
    D3D12_VERTEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.StrideInBytes  = stride;
    m_CmdList->IASetVertexBuffers(0, 1, &v);
}

void CDx12CommandList::SetIndexBuffer(IRhiBuffer& ib) noexcept {
    auto& b = static_cast<FDx12Buffer&>(ib);
    D3D12_INDEX_BUFFER_VIEW v{};
    v.BufferLocation = b.Gpu();
    v.SizeInBytes    = static_cast<UINT>(b.Size());
    v.Format = (b.Usage() == EBufferUsage::Index32) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R16_UINT;
    m_CmdList->IASetIndexBuffer(&v);
}

void CDx12CommandList::SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept {
    if (!m_BoundPipe || slot >= m_BoundPipe->CBufferSlots()) return;
    /** 論理sliceを解決した実DX12バッファ。 */
    IRhiBuffer& binding_buffer = cb.BindingBuffer();
    /** 実バッファ先頭からの定数範囲offset。 */
    const usize binding_offset = cb.BindingOffset();
    if (cb.Usage() != EBufferUsage::Uniform || binding_buffer.Usage() != EBufferUsage::Uniform || binding_offset > binding_buffer.Size() || cb.Size() > binding_buffer.Size() - binding_offset || (binding_offset & 255u) != 0u) return;
    /** root CBVへ渡すbackendバッファ。 */
    auto& b = static_cast<FDx12Buffer&>(binding_buffer);
    // 両シグネチャとも CBV が先頭に並ぶため、ルートパラメーター index は slot と一致する。
    if (m_BoundPipe->IsCompute())
        m_CmdList->SetComputeRootConstantBufferView(slot, b.Gpu() + binding_offset);
    else
        m_CmdList->SetGraphicsRootConstantBufferView(slot, b.Gpu() + binding_offset);
}

void CDx12CommandList::SetTexture(u32 slot, IRhiTexture& tex) noexcept {
    if (!m_BoundPipe || slot >= m_BoundPipe->TextureSlots()) return;
    auto& t = static_cast<FDx12Texture&>(tex);
    if (!t.HasSrv()) return;
    const D3D12_RESOURCE_STATES state = m_BoundPipe->IsCompute()
        ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
        : D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionTexture(m_CmdList, t, state);
    const u32 root_index = m_BoundPipe->CBufferSlots() + slot;
    if (m_BoundPipe->IsCompute())
        m_CmdList->SetComputeRootDescriptorTable(root_index, t.SrvGpuHandle());
    else
        m_CmdList->SetGraphicsRootDescriptorTable(root_index, t.SrvGpuHandle());
}

void CDx12CommandList::SetComputePipeline(IRhiPipeline& pipeline) noexcept {
    auto& p = static_cast<FDx12Pipeline&>(pipeline);
    using FPolicy = TRhiPipelineBindPolicy<ERhiPipelineBindDomain::Compute>;
    if (!FPolicy::Accepts(p.IsCompute()) || !p.Pso() || !p.RootSignature() || !FPolicy::NeedsBind(m_BoundPipe, &p)) return;
    m_CmdList->SetPipelineState(p.Pso());
    m_CmdList->SetComputeRootSignature(p.RootSignature());
    m_BoundPipe = &p;
}

void CDx12CommandList::BindUav(u32 slot, IRhiTexture& tex) noexcept {
    if (!m_BoundPipe || !m_BoundPipe->IsCompute() ||
        slot >= m_BoundPipe->UavSlots()) return;
    auto& t = static_cast<FDx12Texture&>(tex);
    if (!t.HasUav()) return;
    TransitionTexture(m_CmdList, t, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const u32 root_index =
        m_BoundPipe->CBufferSlots() + m_BoundPipe->TextureSlots() + slot;
    m_CmdList->SetComputeRootDescriptorTable(root_index, t.UavGpuHandle());
}

void CDx12CommandList::Dispatch(u32 gx, u32 gy, u32 gz) noexcept {
    if (!m_BoundPipe || !m_BoundPipe->IsCompute() ||
        gx == 0 || gy == 0 || gz == 0) return;
    RecordDispatch(m_CommandStatistics);
    m_CmdList->Dispatch(gx, gy, gz);
    // 後続の dispatch、transition、draw より前に全 UAV write を順序付ける。
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = nullptr;
    m_CmdList->ResourceBarrier(1, &barrier);
}

bool CDx12CommandList::CopyDepthTexture(
    IRhiTexture& source,
    IRhiTexture& destination) noexcept {
    if (!m_CmdList || !_open ||
        !IsDepthTextureCopyCompatible(source, destination)) {
        return false;
    }

    auto& dx_source = static_cast<FDx12Texture&>(source);
    auto& dx_destination = static_cast<FDx12Texture&>(destination);
    if (!dx_source.Resource() || !dx_destination.Resource() ||
        dx_source.Resource() == dx_destination.Resource() ||
        !dx_source.IsDepth() || !dx_destination.IsDepth() ||
        !dx_destination.HasSrv()) {
        return false;
    }

    // Closing an offscreen pass may rebind the main DSV. Drop output-merger
    // bindings before transitioning that allocation to COPY_SOURCE.
    m_CmdList->OMSetRenderTargets(0u, nullptr, FALSE, nullptr);
    m_BoundPipe = nullptr;

    D3D12_RESOURCE_BARRIER before[2]{};
    u32 before_count = 0u;
    if (dx_source.CurrentState() != D3D12_RESOURCE_STATE_COPY_SOURCE) {
        auto& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dx_source.Resource();
        barrier.Transition.StateBefore = dx_source.CurrentState();
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    if (dx_destination.CurrentState() != D3D12_RESOURCE_STATE_COPY_DEST) {
        auto& barrier = before[before_count++];
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = dx_destination.Resource();
        barrier.Transition.StateBefore = dx_destination.CurrentState();
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    if (before_count > 0u) {
        m_CmdList->ResourceBarrier(before_count, before);
    }

    m_CmdList->CopyResource(
        dx_destination.Resource(), dx_source.Resource());

    D3D12_RESOURCE_BARRIER after[2]{};
    after[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    after[0].Transition.pResource = dx_source.Resource();
    after[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    after[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    after[0].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    after[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    after[1].Transition.pResource = dx_destination.Resource();
    after[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    after[1].Transition.StateAfter =
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    after[1].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_CmdList->ResourceBarrier(2u, after);
    dx_source.SetCurrentState(D3D12_RESOURCE_STATE_DEPTH_WRITE);
    dx_destination.SetCurrentState(
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

void CDx12CommandList::Draw(u32 vertex_count, u32 first_vertex) noexcept {
    if (vertex_count == 0u) return;
    RecordDraw(m_CommandStatistics, vertex_count);
    m_CmdList->DrawInstanced(vertex_count, 1, first_vertex, 0);
}

void CDx12CommandList::DrawIndexed(u32 index_count, u32 first_index, i32 base_vertex) noexcept {
    if (index_count == 0u) return;
    RecordDraw(m_CommandStatistics, index_count);
    m_CmdList->DrawIndexedInstanced(index_count, 1, first_index, base_vertex, 0);
}

// ファクトリ関数
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 20, "CreateRhiCommandList: device is not DX12");
    CDx12Device* dxd = static_cast<CDx12Device*>(&device);
    auto cl = MakeUnique<CDx12CommandList>();
    const FHrResult r = cl->Init(*dxd);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 21, "Dx12CommandList::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiCommandList> base(cl.Release(), cl.GetAllocator());
    return TResult<TUniquePtr<IRhiCommandList>>(OkInit, Move(base));
}
#endif

} // namespace acs
