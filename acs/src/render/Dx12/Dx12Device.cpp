// SPDX-License-Identifier: Apache-2.0
// DX12 デバイス実装
#include "render/Dx12/Dx12Device.h"
#include "render/Dx12/Dx12Texture.h" // Dx12Texture (ReadTexture の cast)
#include "memory/UniquePtr.h"

#include <cstring> // std::memcpy

#if ACS_BUILD_DEBUG
#    include "foundation/Log.h"
#    include "render/Dx12/Dx12LiveObjectDiagnosticsInternal.h"
#endif

namespace acs {

namespace {

/** 例外を使わず SRWLOCK の排他範囲を確実に閉じる内部ガード。 */
class ExclusiveLockGuard final {
public:
    explicit ExclusiveLockGuard(SRWLOCK& lock) noexcept : m_Lock(lock)
    {
        ::AcquireSRWLockExclusive(&m_Lock);
    }
    ~ExclusiveLockGuard() noexcept
    {
        ::ReleaseSRWLockExclusive(&m_Lock);
    }

    ExclusiveLockGuard(const ExclusiveLockGuard&) = delete;
    ExclusiveLockGuard& operator=(const ExclusiveLockGuard&) = delete;

private:
    SRWLOCK& m_Lock;
};

} // namespace

Dx12Device::~Dx12Device() noexcept
{
    Reset();
}

void Dx12Device::Reset() noexcept
{
#if ACS_BUILD_DEBUG
    // 初期化途中で Factory または Adapter だけを得た経路も DXGI 診断対象にする。
    const bool report_d3d12_live_objects = m_Device != nullptr;
    const bool report_dxgi_live_objects = m_Device || m_Adapter || m_Factory;
    render_internal::D3D12DebugDeviceReportHandle debug_device = render_internal::CaptureD3D12DebugDeviceReportHandle(
        m_Device);
#endif
    // event まで揃っている場合だけ待機できる。Init 途中のロールバックでは
    // fence だけ存在する状態もあるため、NULL event を待ってはいけない。
    if (m_GfxQueue && m_IdleFence && m_IdleEvent) WaitIdle();
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
    m_SrvHighWater = 0;
    m_SrvFreeCount = 0;
    m_DsvHandleSize = 0;
    m_DsvHighWater = 0;
    m_DsvFreeCount = 0;
    m_RtvHandleSize = 0;
    m_RtvHighWater = 0;
    m_RtvFreeCount = 0;
}

HrResult Dx12Device::InitDescriptorHeaps() noexcept
{
    HrResult r{};
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
    m_SrvHighWater = 0;
    m_SrvFreeCount = 0;

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
    m_DsvHighWater = 0;
    m_DsvFreeCount = 0;

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
    m_RtvHighWater = 0;
    m_RtvFreeCount = 0;
    return r;
}

i32 Dx12Device::AllocateSrvSlot() noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (m_SrvFreeCount > 0) {
        return m_SrvFreeList[--m_SrvFreeCount];
    }
    if (m_SrvHighWater >= kSrvCapacity) return -1;
    return static_cast<i32>(m_SrvHighWater++);
}

void Dx12Device::FreeSrvSlot(i32 index) noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (index < 0 || static_cast<u32>(index) >= m_SrvHighWater) return;
    for (u32 i = 0; i < m_SrvFreeCount; ++i) {
        if (m_SrvFreeList[i] == index) return; // 二重返却で同一スロットを重複配布しない
    }
    if (m_SrvFreeCount < kSrvCapacity) {
        m_SrvFreeList[m_SrvFreeCount++] = index;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::SrvCpuHandle(i32 index) const noexcept
{
    if (!m_SrvHeap || index < 0 || static_cast<u32>(index) >= m_SrvHighWater) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_SrvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Device::SrvGpuHandle(i32 index) const noexcept
{
    if (!m_SrvHeap || index < 0 || static_cast<u32>(index) >= m_SrvHighWater) return D3D12_GPU_DESCRIPTOR_HANDLE{0};
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(m_SrvHandleSize) * static_cast<UINT64>(index);
    return h;
}

i32 Dx12Device::AllocateDsvSlot() noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (m_DsvFreeCount > 0) return m_DsvFreeList[--m_DsvFreeCount];
    if (m_DsvHighWater >= kDsvCapacity) return -1;
    return static_cast<i32>(m_DsvHighWater++);
}

void Dx12Device::FreeDsvSlot(i32 index) noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (index < 0 || static_cast<u32>(index) >= m_DsvHighWater) return;
    for (u32 i = 0; i < m_DsvFreeCount; ++i) {
        if (m_DsvFreeList[i] == index) return;
    }
    if (m_DsvFreeCount < kDsvCapacity) m_DsvFreeList[m_DsvFreeCount++] = index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::DsvCpuHandle(i32 index) const noexcept
{
    if (!m_DsvHeap || index < 0 || static_cast<u32>(index) >= m_DsvHighWater) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_DsvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

i32 Dx12Device::AllocateRtvSlot() noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (m_RtvFreeCount > 0) return m_RtvFreeList[--m_RtvFreeCount];
    if (m_RtvHighWater >= kRtvCapacity) return -1;
    return static_cast<i32>(m_RtvHighWater++);
}

void Dx12Device::FreeRtvSlot(i32 index) noexcept
{
    ExclusiveLockGuard guard(m_DescriptorLock);
    if (index < 0 || static_cast<u32>(index) >= m_RtvHighWater) return;
    for (u32 i = 0; i < m_RtvFreeCount; ++i) {
        if (m_RtvFreeList[i] == index) return;
    }
    if (m_RtvFreeCount < kRtvCapacity) m_RtvFreeList[m_RtvFreeCount++] = index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::RtvCpuHandle(i32 index) const noexcept
{
    if (!m_RtvHeap || index < 0 || static_cast<u32>(index) >= m_RtvHighWater) return D3D12_CPU_DESCRIPTOR_HANDLE{0};
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_RtvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

HrResult Dx12Device::Init(const DeviceConfig& configuration) noexcept
{
    HrResult r{};
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
void Dx12Device::WaitIdle() noexcept
{
    if (!m_GfxQueue || !m_IdleFence || !m_IdleEvent) return;
    const u64 value = SignalGraphicsQueue();
    if (value == 0) return;
    WaitForFenceValue(value);
}

// フレーム単位で利用する Signal/Wait（WaitIdle と同じ fence を共有）
u64 Dx12Device::SignalGraphicsQueue() noexcept
{
    if (!m_GfxQueue || !m_IdleFence) return 0;
    ExclusiveLockGuard guard(m_FenceSignalLock);
    const u64 next_value = m_IdleValue + 1;
    if (next_value == 0 || FAILED(m_GfxQueue->Signal(m_IdleFence, next_value))) return 0;
    m_IdleValue = next_value;
    return next_value;
}

void Dx12Device::WaitForFenceValue(u64 value) noexcept
{
    if (!m_IdleFence || !m_IdleEvent || value == 0) return;
    ExclusiveLockGuard guard(m_FenceWaitLock);
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

// RT テクスチャを CPU へ読み戻す (一度きりのサムネイル/スクショ用)。RGBA8/BGRA8 (4 B/px) 前提。
bool Dx12Device::ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept
{
    if (!m_Device || !m_GfxQueue || destination_pixels == nullptr) return false;
    auto* dtex = static_cast<Dx12Texture*>(&texture);
    ID3D12Resource* res = dtex->Resource();
    if (res == nullptr) return false;
    const u32 w = texture.Width(), h = texture.Height();
    const u32 bpp = 4; // RGBA8/BGRA8 前提
    if (texture.EPixelFormat() != EFormat::R8G8B8A8_UNorm && texture.EPixelFormat() != EFormat::R8G8B8A8_UNorm_sRGB &&
        texture.EPixelFormat() != EFormat::B8G8R8A8_UNorm)
        return false;
    const u64 required_bytes = static_cast<u64>(w) * static_cast<u64>(h) * bpp;
    if (w == 0 || h == 0 || required_bytes > destination_size) return false;

    // コピー可能フットプリント (行ピッチは 256B 整列されることがある)。
    D3D12_RESOURCE_DESC desc = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT num_rows = 0;
    UINT64 row_size = 0, total = 0;
    m_Device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &num_rows, &row_size, &total);
    if (total == 0 || num_rows < h || row_size < static_cast<u64>(w) * bpp) return false;

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
            ID3D12CommandList* lists[] = {cl};
            m_GfxQueue->ExecuteCommandLists(1, lists);
            const u64 fence_value = SignalGraphicsQueue();
            ok = fence_value != 0;
            if (ok) WaitForFenceValue(fence_value);
        }

        void* mapped = nullptr;
        D3D12_RANGE read_range{0, static_cast<SIZE_T>(total)};
        if (ok && SUCCEEDED(rb->Map(0, &read_range, &mapped)) && mapped != nullptr) {
            auto* dstp = static_cast<u8*>(destination_pixels);
            const auto* srcp = static_cast<const u8*>(mapped);
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
    rb->Release();
    return ok;
}

// ファクトリ関数: CreateRhiDevice の DX12 実装
// Diligent バックエンドが有効化されている場合は RhiBackend.cpp が実装を提供する。
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& configuration) noexcept
{
    auto d = MakeUnique<Dx12Device>();
    if (!d) return ACS_ERR(Memory, 200, "Dx12Device alloc failed");
    const HrResult r = d->Init(configuration);
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
