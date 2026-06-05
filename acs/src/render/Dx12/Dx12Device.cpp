// SPDX-License-Identifier: Apache-2.0
// DX12 デバイス実装
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

namespace acs {

Dx12Device::~Dx12Device() noexcept {
    WaitIdle();  // Pending コマンドの完了を待ってから破棄
    if (m_IdleEvent) ::CloseHandle(m_IdleEvent);
    ACS_SAFE_RELEASE(m_IdleFence);
    ACS_SAFE_RELEASE(m_RtvHeap);
    ACS_SAFE_RELEASE(m_DsvHeap);
    ACS_SAFE_RELEASE(m_SrvHeap);
    ACS_SAFE_RELEASE(m_GfxQueue);
    ACS_SAFE_RELEASE(m_Device);
    ACS_SAFE_RELEASE(m_Adapter);
    ACS_SAFE_RELEASE(m_Factory);
}

HrResult Dx12Device::InitDescriptorHeaps() noexcept {
    HrResult r{};
    // SRV/CBV/UAV 用シェーダ可視ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCapacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_SrvHeap));
    if (r.IsErr()) return r;
    m_SrvHandleSize =
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_SrvHighWater = 0;
    m_SrvFreeCount = 0;

    // DSV 用 CPU 専用ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{};
    dsv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_hd.NumDescriptors = kDsvCapacity;
    dsv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&m_DsvHeap));
    if (r.IsErr()) return r;
    m_DsvHandleSize =
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_DsvHighWater = 0;
    m_DsvFreeCount = 0;

    // RTV 用 CPU 専用ヒープ（オフスクリーン RT 用）
    D3D12_DESCRIPTOR_HEAP_DESC rtv_hd{};
    rtv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_hd.NumDescriptors = kRtvCapacity;
    rtv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    rtv_hd.NodeMask = 0;
    r.hr = m_Device->CreateDescriptorHeap(&rtv_hd, IID_PPV_ARGS(&m_RtvHeap));
    if (r.IsErr()) return r;
    m_RtvHandleSize =
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_RtvHighWater = 0;
    m_RtvFreeCount = 0;
    return r;
}

i32 Dx12Device::AllocateSrvSlot() noexcept {
    if (m_SrvFreeCount > 0) {
        return m_SrvFreeList[--m_SrvFreeCount];
    }
    if (m_SrvHighWater >= kSrvCapacity) return -1;
    return static_cast<i32>(m_SrvHighWater++);
}

void Dx12Device::FreeSrvSlot(i32 index) noexcept {
    if (index < 0) return;
    if (m_SrvFreeCount < kSrvCapacity) {
        m_SrvFreeList[m_SrvFreeCount++] = index;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::SrvCpuHandle(i32 index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_SrvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Device::SrvGpuHandle(i32 index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(m_SrvHandleSize) * static_cast<UINT64>(index);
    return h;
}

i32 Dx12Device::AllocateDsvSlot() noexcept {
    if (m_DsvFreeCount > 0) return m_DsvFreeList[--m_DsvFreeCount];
    if (m_DsvHighWater >= kDsvCapacity) return -1;
    return static_cast<i32>(m_DsvHighWater++);
}

void Dx12Device::FreeDsvSlot(i32 index) noexcept {
    if (index < 0) return;
    if (m_DsvFreeCount < kDsvCapacity) m_DsvFreeList[m_DsvFreeCount++] = index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::DsvCpuHandle(i32 index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_DsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_DsvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

i32 Dx12Device::AllocateRtvSlot() noexcept {
    if (m_RtvFreeCount > 0) return m_RtvFreeList[--m_RtvFreeCount];
    if (m_RtvHighWater >= kRtvCapacity) return -1;
    return static_cast<i32>(m_RtvHighWater++);
}

void Dx12Device::FreeRtvSlot(i32 index) noexcept {
    if (index < 0) return;
    if (m_RtvFreeCount < kRtvCapacity) m_RtvFreeList[m_RtvFreeCount++] = index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::RtvCpuHandle(i32 index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(m_RtvHandleSize) * static_cast<SIZE_T>(index);
    return h;
}

HrResult Dx12Device::Init(const DeviceConfig& cfg) noexcept {
    HrResult r{};

    // デバッグレイヤー有効化（ID3D12Debug を取得して EnableDebugLayer）
    if (cfg.enable_debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(::D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }

    // DXGI ファクトリ作成
    const UINT factory_flags = cfg.enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    r.hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&m_Factory));
    if (r.IsErr()) return r;

    // 適切なアダプタ（GPU）を列挙して選ぶ
    const DXGI_GPU_PREFERENCE pref = cfg.prefer_high_perf
        ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
        : DXGI_GPU_PREFERENCE_UNSPECIFIED;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT enum_hr = m_Factory->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&adapter));
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        // ソフトウェアアダプタ（WARP）はスキップ
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); continue; }
        // この GPU で D3D12 デバイス作成を試みる
        if (SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                          IID_PPV_ARGS(&m_Device)))) {
            m_Adapter = adapter;
            // GPU 名（UTF-16 → UTF-8 簡易変換）
            for (int j = 0; j < 127 && desc.Description[j]; ++j) {
                const wchar_t c = desc.Description[j];
                m_AdapterName[j] = (c < 128) ? static_cast<char>(c) : '?';
                m_AdapterName[j + 1] = 0;
            }
            break;
        }
        adapter->Release();
    }
    if (!m_Device) {
        r.hr = E_FAIL;
        return r;
    }

    // グラフィックスコマンドキュー作成
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    r.hr = m_Device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_GfxQueue));
    if (r.IsErr()) return r;

    // WaitIdle 用フェンス
    r.hr = m_Device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_IdleFence));
    if (r.IsErr()) return r;
    // CreateEventW は失敗時 NULL を返す。未チェックだと m_IdleEvent==NULL のまま
    // WaitIdle()/WaitForFenceValue() の WaitForSingleObject(NULL,…) が即 WAIT_FAILED で
    // 返り、GPU 完了を待たずにリソースを破棄して GPU 側 use-after-free を招く。
    // ここで正直に失敗を伝播し、偽の成功を返さない。
    m_IdleEvent = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_IdleEvent) {
        r.hr = HRESULT_FROM_WIN32(::GetLastError());
        if (!r.IsErr()) r.hr = E_FAIL;  // GetLastError が 0 を返した場合の保険
        return r;
    }

    // 共有 SRV ヒープ
    r = InitDescriptorHeaps();
    return r;
}

// GPU が現在のキューに積まれた全コマンドを完了するまで待つ
void Dx12Device::WaitIdle() noexcept {
    if (!m_GfxQueue || !m_IdleFence) return;
    ++m_IdleValue;
    m_GfxQueue->Signal(m_IdleFence, m_IdleValue);
    if (m_IdleFence->GetCompletedValue() < m_IdleValue) {
        m_IdleFence->SetEventOnCompletion(m_IdleValue, m_IdleEvent);
        ::WaitForSingleObject(m_IdleEvent, INFINITE);
    }
}

// フレーム単位で利用する Signal/Wait（WaitIdle と同じ fence を共有）
u64 Dx12Device::SignalGraphicsQueue() noexcept {
    if (!m_GfxQueue || !m_IdleFence) return 0;
    ++m_IdleValue;
    m_GfxQueue->Signal(m_IdleFence, m_IdleValue);
    return m_IdleValue;
}

void Dx12Device::WaitForFenceValue(u64 value) noexcept {
    if (!m_IdleFence || value == 0) return;
    if (m_IdleFence->GetCompletedValue() >= value) return;
    m_IdleFence->SetEventOnCompletion(value, m_IdleEvent);
    ::WaitForSingleObject(m_IdleEvent, INFINITE);
}

// ファクトリ関数: CreateRhiDevice の DX12 実装
// Diligent バックエンドが有効化されている場合は RhiBackend.cpp が実装を提供する。
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept {
    auto d = MakeUnique<Dx12Device>();
    if (!d) return ACS_ERR(Memory, 200, "Dx12Device alloc failed");
    const HrResult r = d->Init(cfg);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 1, "Dx12Device::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiDevice> base(d.Release(), d.GetAllocator());
    return TResult<TUniquePtr<IRhiDevice>>(OkInit, Move(base));
}
#endif

} // namespace acs
