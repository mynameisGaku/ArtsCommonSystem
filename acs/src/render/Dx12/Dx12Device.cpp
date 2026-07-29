// SPDX-License-Identifier: Apache-2.0
// DX12 デバイス実装
#include "render/Dx12/Dx12Device.h"
#include "render/FormatTraits.h"
#include "render/Dx12/Dx12Texture.h" // Dx12Texture (ReadTexture の cast)
#include "render/PipelineStateKeyCache.h"
#include "memory/New.h"
#include "memory/UniquePtr.h"

#include <cstring> // std::memcpy

#if ACS_BUILD_DEBUG
#    include "foundation/Log.h"
#    include "render/Dx12/Dx12LiveObjectDiagnosticsInternal.h"
#endif

namespace acs {

namespace {

/**
 * ReadTexture が密な CPU 行へ変換できる非圧縮 format の bytes-per-pixel。
 *
 * 0 は未対応を表す。D3D12 の copy footprint は実 format から算出されるため、
 * ここでは destination の密配置と一致する format だけを明示的に許可する。
 */
u32 ReadbackBytesPerPixel(EFormat format) noexcept
{
    return GetFormatTraits(format).bytes_per_block;
}

/** 例外を使わず SRWLOCK の排他範囲を確実に閉じる内部ガード。 */
class FExclusiveLockGuard final {
public:
    explicit FExclusiveLockGuard(SRWLOCK& lock) noexcept : m_Lock(lock)
    {
        ::AcquireSRWLockExclusive(&m_Lock);
    }
    ~FExclusiveLockGuard() noexcept
    {
        ::ReleaseSRWLockExclusive(&m_Lock);
    }

    FExclusiveLockGuard(const FExclusiveLockGuard&) = delete;
    FExclusiveLockGuard& operator=(const FExclusiveLockGuard&) = delete;

private:
    SRWLOCK& m_Lock;
};

} // namespace

/** key table・COM owner・lock を公開 device layout 外へ隔離する内部 cache owner。 */
struct FDx12Device::FPipelineCacheOwner {
    /** 確保元 allocator を記録して同じ所有者へ返す。 */
    explicit FPipelineCacheOwner(FAllocator& allocator) noexcept : allocator(&allocator) {}

    /** 自身を確保した allocator。 */
    FAllocator* allocator = nullptr;
    /** PSO key を native 配列 index へ intern する表。 */
    TPipelineStateKeyCache<kPipelineCacheCapacity> key_cache;
    /** cache が所有する PSO。 */
    ID3D12PipelineState* pipeline_states[kPipelineCacheCapacity]{};
    /** cache が所有する root signature。 */
    ID3D12RootSignature* root_signatures[kPipelineCacheCapacity]{};
};

FDx12Device::~FDx12Device() noexcept
{
    Reset();
}

bool FDx12Device::FindCachedPipeline(const FPipelineStateKey& key, ID3D12PipelineState*& pipeline, ID3D12RootSignature*& root_signature) noexcept {
    pipeline = nullptr;
    root_signature = nullptr;
    /** owner の参照から COM AddRef までを Reset と直列化する lock。 */
    FExclusiveLockGuard cache_guard(m_PipelineCacheLock);
    /** outer lock 内で安定している現在の cache owner。 */
    FPipelineCacheOwner* owner = m_PipelineCacheOwner;
    if (owner == nullptr) return false;
    /** key table が返す native 配列 index。 */
    u32 identifier = 0u;
    if (!owner->key_cache.Find(key, identifier) || identifier >= kPipelineCacheCapacity) return false;
    /** cache が所有する PSO。 */
    ID3D12PipelineState* cached_pipeline = owner->pipeline_states[identifier];
    /** cache が所有する root signature。 */
    ID3D12RootSignature* cached_root_signature = owner->root_signatures[identifier];
    if (cached_pipeline == nullptr || cached_root_signature == nullptr) return false;
    cached_pipeline->AddRef();
    cached_root_signature->AddRef();
    pipeline = cached_pipeline;
    root_signature = cached_root_signature;
    return true;
}

void FDx12Device::StoreCachedPipeline(const FPipelineStateKey& key, ID3D12PipelineState* pipeline, ID3D12RootSignature* root_signature) noexcept {
    if (pipeline == nullptr || root_signature == nullptr) return;
    /** owner の遅延生成から登録までを Reset と直列化する lock。 */
    FExclusiveLockGuard cache_guard(m_PipelineCacheLock);
    /** outer lock 内で安定している現在の cache owner。 */
    FPipelineCacheOwner* owner = m_PipelineCacheOwner;
    if (owner == nullptr) {
        /** owner の確保と解放に使う allocator。 */
        FAllocator& allocator = DefaultAllocator();
        /** 初回登録用に確保する候補 owner。 */
        owner = New<FPipelineCacheOwner>(allocator, allocator);
        if (owner == nullptr) return;
        m_PipelineCacheOwner = owner;
    }
    /** key table が返す native 配列 index。 */
    u32 identifier = 0u;
    /** 同じ key が既に登録済みか。 */
    bool found = false;
    if (!owner->key_cache.FindOrIntern(key, identifier, found) || identifier >= kPipelineCacheCapacity || found) return;
    pipeline->AddRef();
    root_signature->AddRef();
    owner->pipeline_states[identifier] = pipeline;
    owner->root_signatures[identifier] = root_signature;
}

void FDx12Device::ResetPipelineCache(bool release_objects) noexcept {
    /** owner の切離しから metadata delete までを lookup/register と直列化する lock。 */
    FExclusiveLockGuard cache_guard(m_PipelineCacheLock);
    /** outer lock 内で切り離す cache owner。 */
    FPipelineCacheOwner* owner = m_PipelineCacheOwner;
    if (owner == nullptr) return;
    m_PipelineCacheOwner = nullptr;
    for (u32 index = 0u; index < kPipelineCacheCapacity; ++index) {
        if (release_objects) {
            ACS_SAFE_RELEASE(owner->pipeline_states[index]);
            ACS_SAFE_RELEASE(owner->root_signatures[index]);
        } else {
            owner->pipeline_states[index] = nullptr;
            owner->root_signatures[index] = nullptr;
        }
    }
    owner->key_cache.Reset();
    /** owner が記録した確保元 allocator。 */
    FAllocator& allocator = *owner->allocator;
    Delete(allocator, owner);
}

void FDx12Device::Reset() noexcept
{
#if ACS_BUILD_DEBUG
    // 初期化途中で Factory または Adapter だけを得た経路も DXGI 診断対象にする。
    const bool report_d3d12_live_objects = m_Device != nullptr;
    const bool report_dxgi_live_objects = m_Device || m_Adapter || m_Factory;
    render_internal::FD3D12DebugDeviceReportHandle debug_device = render_internal::CaptureD3D12DebugDeviceReportHandle(
        m_Device);
#endif
    // Final teardown signals and waits for every command list already submitted
    // to the queue. The owning renderer destroys its command list before the
    // device, so a completed final fence permits releasing both sealed and
    // still-pending retirement records without assigning an earlier one-off
    // fence to open main-list references.
    bool final_signal_completed = false;
    if (m_GfxQueue && m_IdleFence && m_IdleEvent) {
        const u64 final_fence = SignalGraphicsQueue();
        if (final_fence != 0u) {
            WaitForFenceValue(final_fence);
            const u64 completed = m_IdleFence->GetCompletedValue();
            final_signal_completed =
                completed == static_cast<u64>(-1) ||
                completed >= final_fence;
            if (final_signal_completed) {
                // Device teardown follows destruction of the owning command
                // list. Once every submitted queue item is complete, even
                // records which were never sealed by a main Submit are safe to
                // release before descriptor heaps and the device disappear.
                ReleaseAllRetiredResources();
            }
        }
    } else if (m_GfxQueue == nullptr) {
        // A partial Init which never created a queue cannot have submitted GPU
        // references, so direct teardown is safe and avoids rollback leaks.
        ReleaseAllRetiredResources();
    }

    // A failed Signal does not prove completion. Likewise, records retired
    // after the final Signal are not covered by it. Preserve their COM
    // ownership and descriptor indices (leak-safe device-failure fallback)
    // rather than risking a GPU use-after-free during shutdown.
    if (!final_signal_completed && m_GfxQueue != nullptr) {
        AbandonAllRetiredResources();
    } else if (RetiredResourceCount() != 0u) {
        AbandonAllRetiredResources();
    }
    // GPU 完了を証明できない場合は PSO と root signature を解放せず use-after-free を防ぐ。
    ResetPipelineCache(final_signal_completed || m_GfxQueue == nullptr);
    if (m_IdleEvent) {
        ::CloseHandle(m_IdleEvent);
        m_IdleEvent = nullptr;
    }
    ACS_SAFE_RELEASE(m_IdleFence);
    ACS_SAFE_RELEASE(m_RtvHeap);
    ACS_SAFE_RELEASE(m_DsvHeap);
    ACS_SAFE_RELEASE(m_SrvHeap);
    ACS_SAFE_RELEASE(m_GfxQueue);
    ACS_SAFE_RELEASE(m_Device);
    ACS_SAFE_RELEASE(m_Adapter);
    ACS_SAFE_RELEASE(m_Factory);

#if ACS_BUILD_DEBUG
    if (report_d3d12_live_objects) {
        render_internal::ReportD3D12LiveObjects(debug_device, "DX12");
        debug_device.Release();
    }
    if (report_dxgi_live_objects) {
        render_internal::ReportDxgiLiveObjects("DX12");
    }
#endif

    m_IdleValue = 0;
    m_FrameSlot = 0;
    m_AdapterName[0] = '\0';
    m_SrvHandleSize = 0;
    m_SrvSlots.Reset();
    m_DsvHandleSize = 0;
    m_DsvSlots.Reset();
    m_RtvHandleSize = 0;
    m_RtvSlots.Reset();
}

FHrResult FDx12Device::InitDescriptorHeaps() noexcept
{
    FHrResult r{};
    // SRV/CBV/UAV 用シェーダ可視ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCapacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_SrvHeap));
    if (r.IsErr() || !m_SrvHeap) {
        if (r.IsOk()) r.hr = E_FAIL;
        ACS_SAFE_RELEASE(m_SrvHeap);
        return r;
    }
    m_SrvHandleSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_SrvSlots.Reset();

    // DSV 用 CPU 専用ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{};
    dsv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_hd.NumDescriptors = kDsvCapacity;
    dsv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&m_DsvHeap));
    if (r.IsErr() || !m_DsvHeap) {
        if (r.IsOk()) r.hr = E_FAIL;
        ACS_SAFE_RELEASE(m_SrvHeap);
        m_SrvHandleSize = 0;
        return r;
    }
    m_DsvHandleSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_DsvSlots.Reset();

    // RTV 用 CPU 専用ヒープ（オフスクリーン RT 用）
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = kRtvCapacity;
    rtv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&m_RtvHeap));
    if (r.IsErr() || !m_RtvHeap) {
        if (r.IsOk()) r.hr = E_FAIL;
        ACS_SAFE_RELEASE(m_DsvHeap);
        ACS_SAFE_RELEASE(m_SrvHeap);
        m_DsvHandleSize = 0;
        m_SrvHandleSize = 0;
        return r;
    }
    m_RtvHandleSize = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_RtvSlots.Reset();
    return r;
}

i32 FDx12Device::AllocateSrvSlot() noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_SrvSlots.Allocate();
}

bool FDx12Device::AllocateSrvSlots(i32* output, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_SrvSlots.AllocateBatch(output, count);
}

void FDx12Device::FreeSrvSlot(i32 index) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_SrvSlots.Free(index);
}

void FDx12Device::FreeSrvSlots(const i32* slots, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_SrvSlots.FreeBatch(slots, count);
}

D3D12_CPU_DESCRIPTOR_HANDLE FDx12Device::SrvCpuHandle(i32 index) const noexcept
{
    // Handle math only needs the immutable heap capacity. Reading the mutable
    // high-water mark here raced with free-threaded background allocation.
    if (!m_SrvHeap || index < 0 || static_cast<u32>(index) >= kSrvCapacity) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_SrvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE FDx12Device::SrvGpuHandle(i32 index) const noexcept
{
    if (!m_SrvHeap || index < 0 || static_cast<u32>(index) >= kSrvCapacity) return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(m_SrvHandleSize) * static_cast<UINT64>(index);
    return h;
}

i32 FDx12Device::AllocateDsvSlot() noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_DsvSlots.Allocate();
}

bool FDx12Device::AllocateDsvSlots(i32* output, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_DsvSlots.AllocateBatch(output, count);
}

void FDx12Device::FreeDsvSlot(i32 index) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_DsvSlots.Free(index);
}

void FDx12Device::FreeDsvSlots(const i32* slots, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_DsvSlots.FreeBatch(slots, count);
}

D3D12_CPU_DESCRIPTOR_HANDLE FDx12Device::DsvCpuHandle(i32 index) const noexcept
{
    if (!m_DsvHeap || index < 0 || static_cast<u32>(index) >= kDsvCapacity) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_DsvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

i32 FDx12Device::AllocateRtvSlot() noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_RtvSlots.Allocate();
}

bool FDx12Device::AllocateRtvSlots(i32* output, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    return m_RtvSlots.AllocateBatch(output, count);
}

void FDx12Device::FreeRtvSlot(i32 index) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_RtvSlots.Free(index);
}

void FDx12Device::FreeRtvSlots(const i32* slots, u32 count) noexcept
{
    FExclusiveLockGuard guard(m_DescriptorLock);
    m_RtvSlots.FreeBatch(slots, count);
}

void FDx12Device::ReleaseRetiredResource(
    FRetiredResource& retired) noexcept
{
    // Lock order is retirement -> descriptor.  Callers hold (or exclusively
    // own teardown of) m_RetirementLock; descriptor allocation never enters
    // the retirement lock, so this direction cannot deadlock.
    if (retired.srv_slot >= 0) FreeSrvSlot(retired.srv_slot);
    if (retired.uav_slot >= 0) FreeSrvSlot(retired.uav_slot);
    if (retired.dsv_slot >= 0) FreeDsvSlot(retired.dsv_slot);
    for (usize i = 0; i < retired.rtv_slots.Size(); ++i) {
        if (retired.rtv_slots[i] >= 0)
            FreeRtvSlot(retired.rtv_slots[i]);
    }
    retired.srv_slot = -1;
    retired.uav_slot = -1;
    retired.dsv_slot = -1;
    retired.rtv_slots.ReleaseStorage();
    ACS_SAFE_RELEASE(retired.resource);
    retired.fence_value = 0u;
}

void FDx12Device::ReleaseAllRetiredResources() noexcept
{
    FExclusiveLockGuard retirement_guard(m_RetirementLock);
    for (usize i = 0; i < m_RetiredResources.Size(); ++i)
        ReleaseRetiredResource(m_RetiredResources[i]);
    m_RetiredResources.Clear();
    for (u32 i = 0u; i < m_EmergencyRetiredResourceCount; ++i)
        ReleaseRetiredResource(m_EmergencyRetiredResources[i]);
    m_EmergencyRetiredResourceCount = 0u;
}

void FDx12Device::AbandonAllRetiredResources() noexcept
{
    FExclusiveLockGuard retirement_guard(m_RetirementLock);
    auto abandon = [](FRetiredResource& retired) noexcept {
        // Deliberately drop only CPU-side bookkeeping. The raw COM reference
        // and descriptor indices are not released/recycled because completion
        // is unknown. The enclosing device teardown owns this fail-safe leak.
        retired.resource = nullptr;
        retired.srv_slot = -1;
        retired.uav_slot = -1;
        retired.dsv_slot = -1;
        retired.rtv_slots.ReleaseStorage();
        retired.fence_value = 0u;
    };
    for (usize i = 0; i < m_RetiredResources.Size(); ++i)
        abandon(m_RetiredResources[i]);
    m_RetiredResources.ReleaseStorage();
    for (u32 i = 0u; i < m_EmergencyRetiredResourceCount; ++i)
        abandon(m_EmergencyRetiredResources[i]);
    m_EmergencyRetiredResourceCount = 0u;
}

void FDx12Device::QueueRetiredResource(
    FRetiredResource&& retired) noexcept
{
    if (retired.resource == nullptr &&
        retired.srv_slot < 0 && retired.uav_slot < 0 &&
        retired.dsv_slot < 0 && retired.rtv_slots.IsEmpty()) {
        return;
    }

    {
        FExclusiveLockGuard retirement_guard(m_RetirementLock);
        if (m_RetiredResources.TryPushBack(Move(retired))) return;
        if (m_EmergencyRetiredResourceCount <
            kEmergencyRetirementCapacity) {
            m_EmergencyRetiredResources[
                m_EmergencyRetiredResourceCount++] = Move(retired);
            return;
        }
    }

    // Do not WaitIdle or directly release here: this resource may already be
    // referenced by the currently open/recorded command list, which has not
    // reached ExecuteCommandLists and therefore cannot be covered by a fence
    // yet. Exhausting both metadata stores intentionally leaks the transferred
    // ownership and descriptor indices; a leak is safer than GPU UAF.
    retired.resource = nullptr;
    retired.srv_slot = -1;
    retired.uav_slot = -1;
    retired.dsv_slot = -1;
    retired.rtv_slots.ReleaseStorage();
    retired.fence_value = 0u;
}

void FDx12Device::RetireResource(ID3D12Resource* resource) noexcept
{
    if (resource == nullptr) return;
    FRetiredResource retired{};
    retired.resource = resource;
    QueueRetiredResource(Move(retired));
}

void FDx12Device::RetireTextureResource(
    ID3D12Resource* resource,
    i32 srv_slot, i32 uav_slot, i32 dsv_slot,
    TArray<i32>&& rtv_slots) noexcept
{
    FRetiredResource retired{};
    retired.resource = resource;
    retired.srv_slot = srv_slot;
    retired.uav_slot = uav_slot;
    retired.dsv_slot = dsv_slot;
    retired.rtv_slots = Move(rtv_slots);
    QueueRetiredResource(Move(retired));
}

void FDx12Device::CollectRetiredResources() noexcept
{
    if (m_IdleFence == nullptr) return;
    const u64 completed = m_IdleFence->GetCompletedValue();
    FExclusiveLockGuard retirement_guard(m_RetirementLock);
    for (usize i = m_RetiredResources.Size(); i-- > 0u;) {
        FRetiredResource& retired = m_RetiredResources[i];
        if (retired.fence_value == 0u ||
            (completed != static_cast<u64>(-1) &&
             retired.fence_value > completed)) {
            continue;
        }
        ReleaseRetiredResource(retired);
        m_RetiredResources.RemoveAtSwap(i);
    }
    for (u32 i = m_EmergencyRetiredResourceCount; i-- > 0u;) {
        FRetiredResource& retired = m_EmergencyRetiredResources[i];
        if (retired.fence_value == 0u ||
            (completed != static_cast<u64>(-1) &&
             retired.fence_value > completed)) {
            continue;
        }
        ReleaseRetiredResource(retired);
        --m_EmergencyRetiredResourceCount;
        if (i != m_EmergencyRetiredResourceCount) {
            retired = Move(m_EmergencyRetiredResources[
                m_EmergencyRetiredResourceCount]);
        }
    }
}

usize FDx12Device::RetiredResourceCount() noexcept
{
    FExclusiveLockGuard retirement_guard(m_RetirementLock);
    return m_RetiredResources.Size() +
           static_cast<usize>(m_EmergencyRetiredResourceCount);
}

D3D12_CPU_DESCRIPTOR_HANDLE FDx12Device::RtvCpuHandle(i32 index) const noexcept
{
    if (!m_RtvHeap || index < 0 || static_cast<u32>(index) >= kRtvCapacity) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_RtvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

FHrResult FDx12Device::Init(const FDeviceConfig& configuration) noexcept
{
    FHrResult r{};
    Reset();

    // デバッグレイヤー有効化（ID3D12Debug を取得して EnableDebugLayer）
    if (configuration.enable_debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }

    // DXGI ファクトリ作成
    const UINT factory_flags = configuration.enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    r.hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&m_Factory));
    if (r.IsErr() || !m_Factory) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }

    // 適切なアダプタ（GPU）を列挙して選ぶ
    const DXGI_GPU_PREFERENCE pref = configuration.prefer_high_perf ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
                                                                    : DXGI_GPU_PREFERENCE_UNSPECIFIED;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT enum_hr = m_Factory->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&adapter));
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) {
            ACS_SAFE_RELEASE(adapter);
            continue;
        }
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        // ソフトウェアアダプタ（WARP）はスキップ
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            adapter->Release();
            continue;
        }
        // この GPU で D3D12 デバイス作成を試みる
        ID3D12Device* candidate = nullptr;
        if (SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&candidate)))) {
            m_Device = candidate;
            m_Adapter = adapter;
            // GPU 名（UTF-16 → UTF-8 簡易変換）
            for (int j = 0; j < 127 && desc.Description[j]; ++j) {
                const wchar_t c = desc.Description[j];
                m_AdapterName[j] = (c < 128) ? static_cast<char>(c) : '?';
                m_AdapterName[j + 1] = 0;
            }
            break;
        }
        ACS_SAFE_RELEASE(candidate);
        adapter->Release();
    }
    if (!m_Device) {
        r.hr = E_FAIL;
        Reset();
        return r;
    }

    // グラフィックスコマンドキュー作成
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    r.hr = m_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_GfxQueue));
    if (r.IsErr() || !m_GfxQueue) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }

    // WaitIdle 用フェンス
    r.hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_IdleFence));
    if (r.IsErr() || !m_IdleFence) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }
    // CreateEventW は失敗時 NULL を返す。未チェックだと m_IdleEvent==NULL のまま
    // WaitIdle()/WaitForFenceValue() の WaitForSingleObject(NULL,…) が即 WAIT_FAILED で
    // 返り、GPU 完了を待たずにリソースを破棄して GPU 側 use-after-free を招く。
    // ここで正直に失敗を伝播し、偽の成功を返さない。
    m_IdleEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_IdleEvent) {
        r.hr = HRESULT_FROM_WIN32(::GetLastError());
        if (!r.IsErr()) r.hr = E_FAIL; // GetLastError が 0 を返した場合の保険
        Reset();
        return r;
    }

    // 共有 SRV ヒープ
    r = InitDescriptorHeaps();
    if (r.IsErr()) Reset();
    return r;
}

// GPU が現在のキューに積まれた全コマンドを完了するまで待つ
void FDx12Device::WaitIdle() noexcept
{
    if (!m_GfxQueue || !m_IdleFence || !m_IdleEvent) return;
    const u64 value = SignalGraphicsQueue();
    if (value == 0) return;
    WaitForFenceValue(value);
    CollectRetiredResources();
}

bool FDx12Device::IsOperational() const noexcept
{
    return m_Device != nullptr &&
           m_GfxQueue != nullptr &&
           SUCCEEDED(m_Device->GetDeviceRemovedReason());
}

u64 FDx12Device::SignalGraphicsQueueLocked() noexcept
{
    if (!m_GfxQueue || !m_IdleFence) return 0;
    const u64 next_value = m_IdleValue + 1u;
    if (next_value == 0u ||
        FAILED(m_GfxQueue->Signal(m_IdleFence, next_value))) {
        return 0u;
    }
    m_IdleValue = next_value;
    return next_value;
}

void FDx12Device::SealPendingRetirements(u64 fence_value) noexcept
{
    if (fence_value == 0u) return;
    for (usize i = 0; i < m_RetiredResources.Size(); ++i) {
        if (m_RetiredResources[i].fence_value == 0u)
            m_RetiredResources[i].fence_value = fence_value;
    }
    for (u32 i = 0u; i < m_EmergencyRetiredResourceCount; ++i) {
        if (m_EmergencyRetiredResources[i].fence_value == 0u)
            m_EmergencyRetiredResources[i].fence_value = fence_value;
    }
}

u64 FDx12Device::ExecuteGraphicsCommandListsAndSignal(
    ID3D12CommandList* const* command_lists,
    u32 command_list_count,
    bool seal_retirements) noexcept
{
    if (!m_GfxQueue || !m_IdleFence || command_lists == nullptr ||
        command_list_count == 0u) {
        return 0u;
    }
    for (u32 i = 0u; i < command_list_count; ++i) {
        if (command_lists[i] == nullptr) return 0u;
    }

    if (seal_retirements) {
        // Insertion, Execute, Signal, and sealing are one ordered transaction.
        // A retirement queued before this lock is covered by this main list;
        // one queued afterward remains pending for the next main submission.
        FExclusiveLockGuard retirement_guard(m_RetirementLock);
        FExclusiveLockGuard submission_guard(m_QueueSubmissionLock);
        m_GfxQueue->ExecuteCommandLists(
            command_list_count, command_lists);
        const u64 fence_value = SignalGraphicsQueueLocked();
        if (fence_value != 0u) {
            SealPendingRetirements(fence_value);
        }
        // On Signal failure the executed work has no completion proof. Keep
        // every retirement unsealed and owned instead of releasing early.
        return fence_value;
    }

    // Upload/readback lists are queue ordered with main submissions, but must
    // not seal resources referenced by a main list which is still only
    // recorded in CPU memory.
    FExclusiveLockGuard submission_guard(m_QueueSubmissionLock);
    m_GfxQueue->ExecuteCommandLists(command_list_count, command_lists);
    return SignalGraphicsQueueLocked();
}

u64 FDx12Device::SubmitGraphicsCommandLists(
    ID3D12CommandList* const* command_lists,
    u32 command_list_count) noexcept
{
    return ExecuteGraphicsCommandListsAndSignal(
        command_lists, command_list_count, true);
}

u64 FDx12Device::ExecuteOneOffGraphicsCommandList(
    ID3D12CommandList* command_list) noexcept
{
    ID3D12CommandList* lists[1] = {command_list};
    return ExecuteGraphicsCommandListsAndSignal(
        lists, 1u, false);
}

// Generic queue waits never seal retirement records. Only the main renderer
// Submit path proves that its currently recorded references have been queued.
u64 FDx12Device::SignalGraphicsQueue() noexcept
{
    if (!m_GfxQueue || !m_IdleFence) return 0u;
    FExclusiveLockGuard submission_guard(m_QueueSubmissionLock);
    return SignalGraphicsQueueLocked();
}

void FDx12Device::WaitForFenceValue(u64 value) noexcept
{
    if (!m_IdleFence || !m_IdleEvent || value == 0) return;
    FExclusiveLockGuard guard(m_FenceWaitLock);
    if (m_IdleFence->GetCompletedValue() >= value) return;
    if (SUCCEEDED(m_IdleFence->SetEventOnCompletion(value, m_IdleEvent)) &&
        ::WaitForSingleObject(m_IdleEvent, INFINITE) == WAIT_OBJECT_0)
        return;

    // イベント登録または待機自体が失敗しても、GPU 完了前に一時リソースを
    // 解放しない。デバイスロスト値 (UINT64_MAX) まで短い yield で確認する。
    for (;;) {
        const u64 completed = m_IdleFence->GetCompletedValue();
        if (completed >= value || completed == static_cast<u64>(-1)) return;
        ::Sleep(0);
    }
}

// Texture の mip0/slice0 (3D は depth slice 0) を密な CPU 行へ読み戻す。
bool FDx12Device::ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept
{
    if (!m_Device || !m_GfxQueue || destination_pixels == nullptr) return false;
    auto* dtex = static_cast<FDx12Texture*>(&texture);
    ID3D12Resource* res = dtex->Resource();
    if (res == nullptr) return false;
    const u32 w = texture.Width(), h = texture.Height();
    const u32 bpp = ReadbackBytesPerPixel(texture.PixelFormat());
    if (bpp == 0u) return false;
    if (w == 0u || h == 0u) return false;
    const u64 dense_row_bytes = static_cast<u64>(w) * bpp;
    if (dense_row_bytes > destination_size ||
        static_cast<u64>(h) >
            static_cast<u64>(destination_size) / dense_row_bytes)
        return false;

    // コピー可能フットプリント (行ピッチは 256B 整列されることがある)。
    D3D12_RESOURCE_DESC desc = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_size = 0, total = 0;
    m_Device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_size, &total);
    if (desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER ||
        desc.SampleDesc.Count != 1u ||
        total == 0u ||
        fp.Footprint.Depth == 0u ||
        num_rows < h ||
        row_size < dense_row_bytes ||
        fp.Footprint.RowPitch < dense_row_bytes ||
        fp.Offset > total)
        return false;
    const u64 remaining_after_offset = total - fp.Offset;
    const u64 row_offset =
        static_cast<u64>(h - 1u) * fp.Footprint.RowPitch;
    if (row_offset > remaining_after_offset ||
        dense_row_bytes > remaining_after_offset - row_offset)
        return false;

    // readback ヒープのバッファを確保。
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* rb = nullptr;
    if (FAILED(m_Device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&rb))) ||
        rb == nullptr)
        return false;

    // 一度きりの allocator + command list を作って texture→buffer コピーを記録。
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    bool ok = SUCCEEDED(m_Device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
              SUCCEEDED(
                  m_Device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
    if (ok) {
        const D3D12_RESOURCE_STATES cur = dtex->CurrentState();
        auto transition = [&](D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = res;
            b.Transition.StateBefore = from;
            b.Transition.StateAfter = to;
            b.Transition.Subresource = 0;
            cl->ResourceBarrier(1, &b);
        };
        const bool need = (cur != D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (need) transition(cur, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = res;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        if (need) transition(D3D12_RESOURCE_STATE_COPY_SOURCE, cur); // 元の状態へ戻す
        ok = SUCCEEDED(cl->Close());
        if (ok) {
            const u64 fence_value =
                ExecuteOneOffGraphicsCommandList(cl);
            ok = fence_value != 0;
            if (ok) {
                WaitForFenceValue(fence_value);
            } else {
                // The copy may have executed even though no completion fence
                // was produced. Preserve all objects referenced by that list
                // rather than freeing them under potentially active GPU work.
                cl = nullptr;
                alloc = nullptr;
                rb = nullptr;
            }
        }

        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
        if (ok && SUCCEEDED(rb->Map(0, &read_range, &mapped)) && mapped != nullptr) {
            auto* dstp = static_cast<u8*>(destination_pixels);
            const auto* srcp =
                static_cast<const u8*>(mapped) + static_cast<usize>(fp.Offset);
            const u32 row_pitch = fp.Footprint.RowPitch;
            for (u32 y = 0; y < h; ++y)
                std::memcpy(dstp + static_cast<usize>(y) * w * bpp, srcp + static_cast<usize>(y) * row_pitch, w * bpp);
            D3D12_RANGE no_write{0, 0};
            rb->Unmap(0, &no_write);
        } else
            ok = false;
    }
    if (cl) cl->Release();
    if (alloc) alloc->Release();
    if (rb) rb->Release();
    return ok;
}

// ファクトリ関数: CreateRhiDevice の DX12 実装
// Diligent バックエンドが有効化されている場合は RhiBackend.cpp が実装を提供する。
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const FDeviceConfig& configuration) noexcept
{
    auto d = MakeUnique<FDx12Device>();
    if (!d) return ACS_ERR(Memory, 200, "Dx12Device alloc failed");
    const FHrResult r = d->Init(configuration);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 1, "Dx12Device::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiDevice> base(d.Release(), d.GetAllocator());
    return TResult<TUniquePtr<IRhiDevice>>(OkInit, Move(base));
}

void PrewarmRhiProcessSingletons() noexcept
{
    // raw DX12 backend は CRT ヒープへ遅延構築されるプロセス寿命シングルトンを
    // 持たないため no-op (Diligent 側の実装コメント参照)。
}
#endif

} // namespace acs
