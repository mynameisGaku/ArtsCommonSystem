// SPDX-License-Identifier: Apache-2.0
// DX12 デバイス実装
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"

namespace acs {

Dx12Device::~Dx12Device() noexcept {
    WaitIdle();  // Pending コマンドの完了を待ってから破棄
    if (_idle_event) ::CloseHandle(_idle_event);
    ACS_SAFE_RELEASE(_idle_fence);
    ACS_SAFE_RELEASE(_dsv_heap);
    ACS_SAFE_RELEASE(_srv_heap);
    ACS_SAFE_RELEASE(_gfx_queue);
    ACS_SAFE_RELEASE(_device);
    ACS_SAFE_RELEASE(_adapter);
    ACS_SAFE_RELEASE(_factory);
}

HrResult Dx12Device::InitDescriptorHeaps() noexcept {
    HrResult r{};
    // SRV/CBV/UAV 用シェーダ可視ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCapacity;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hd.NodeMask = 0;
    r.hr = _device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&_srv_heap));
    if (r.IsErr()) return r;
    _srv_handle_size =
        _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    _srv_high_water = 0;
    _srv_free_count = 0;

    // DSV 用 CPU 専用ヒープ
    D3D12_DESCRIPTOR_HEAP_DESC dsv_hd{};
    dsv_hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsv_hd.NumDescriptors = kDsvCapacity;
    dsv_hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dsv_hd.NodeMask = 0;
    r.hr = _device->CreateDescriptorHeap(&dsv_hd, IID_PPV_ARGS(&_dsv_heap));
    if (r.IsErr()) return r;
    _dsv_handle_size =
        _device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    _dsv_high_water = 0;
    _dsv_free_count = 0;
    return r;
}

i32 Dx12Device::AllocateSrvSlot() noexcept {
    if (_srv_free_count > 0) {
        return _srv_free_list[--_srv_free_count];
    }
    if (_srv_high_water >= kSrvCapacity) return -1;
    return static_cast<i32>(_srv_high_water++);
}

void Dx12Device::FreeSrvSlot(i32 index) noexcept {
    if (index < 0) return;
    if (_srv_free_count < kSrvCapacity) {
        _srv_free_list[_srv_free_count++] = index;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::SrvCpuHandle(i32 index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = _srv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(_srv_handle_size) * static_cast<SIZE_T>(index);
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE Dx12Device::SrvGpuHandle(i32 index) const noexcept {
    D3D12_GPU_DESCRIPTOR_HANDLE h = _srv_heap->GetGPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<UINT64>(_srv_handle_size) * static_cast<UINT64>(index);
    return h;
}

i32 Dx12Device::AllocateDsvSlot() noexcept {
    if (_dsv_free_count > 0) return _dsv_free_list[--_dsv_free_count];
    if (_dsv_high_water >= kDsvCapacity) return -1;
    return static_cast<i32>(_dsv_high_water++);
}

void Dx12Device::FreeDsvSlot(i32 index) noexcept {
    if (index < 0) return;
    if (_dsv_free_count < kDsvCapacity) _dsv_free_list[_dsv_free_count++] = index;
}

D3D12_CPU_DESCRIPTOR_HANDLE Dx12Device::DsvCpuHandle(i32 index) const noexcept {
    D3D12_CPU_DESCRIPTOR_HANDLE h = _dsv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(_dsv_handle_size) * static_cast<SIZE_T>(index);
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
    UINT factory_flags = cfg.enable_debug_layer ? DXGI_CREATE_FACTORY_DEBUG : 0;
    r.hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&_factory));
    if (r.IsErr()) return r;

    // 適切なアダプタ（GPU）を列挙して選ぶ
    DXGI_GPU_PREFERENCE pref = cfg.prefer_high_perf
        ? DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE
        : DXGI_GPU_PREFERENCE_UNSPECIFIED;
    for (UINT i = 0;; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT enum_hr = _factory->EnumAdapterByGpuPreference(i, pref, IID_PPV_ARGS(&adapter));
        if (enum_hr == DXGI_ERROR_NOT_FOUND) break;
        if (FAILED(enum_hr)) continue;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        // ソフトウェアアダプタ（WARP）はスキップ
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) { adapter->Release(); continue; }
        // この GPU で D3D12 デバイス作成を試みる
        if (SUCCEEDED(::D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0,
                                          IID_PPV_ARGS(&_device)))) {
            _adapter = adapter;
            // GPU 名（UTF-16 → UTF-8 簡易変換）
            for (int j = 0; j < 127 && desc.Description[j]; ++j) {
                wchar_t c = desc.Description[j];
                _adapter_name[j] = (c < 128) ? static_cast<char>(c) : '?';
                _adapter_name[j + 1] = 0;
            }
            break;
        }
        adapter->Release();
    }
    if (!_device) {
        r.hr = E_FAIL;
        return r;
    }

    // グラフィックスコマンドキュー作成
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    qd.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    qd.NodeMask = 0;
    r.hr = _device->CreateCommandQueue(&qd, IID_PPV_ARGS(&_gfx_queue));
    if (r.IsErr()) return r;

    // WaitIdle 用フェンス
    r.hr = _device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_idle_fence));
    if (r.IsErr()) return r;
    _idle_event = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);

    // 共有 SRV ヒープ
    r = InitDescriptorHeaps();
    return r;
}

// GPU が現在のキューに積まれた全コマンドを完了するまで待つ
void Dx12Device::WaitIdle() noexcept {
    if (!_gfx_queue || !_idle_fence) return;
    ++_idle_value;
    _gfx_queue->Signal(_idle_fence, _idle_value);
    if (_idle_fence->GetCompletedValue() < _idle_value) {
        _idle_fence->SetEventOnCompletion(_idle_value, _idle_event);
        ::WaitForSingleObject(_idle_event, INFINITE);
    }
}

// フレーム単位で利用する Signal/Wait（WaitIdle と同じ fence を共有）
u64 Dx12Device::SignalGraphicsQueue() noexcept {
    if (!_gfx_queue || !_idle_fence) return 0;
    ++_idle_value;
    _gfx_queue->Signal(_idle_fence, _idle_value);
    return _idle_value;
}

void Dx12Device::WaitForFenceValue(u64 value) noexcept {
    if (!_idle_fence || value == 0) return;
    if (_idle_fence->GetCompletedValue() >= value) return;
    _idle_fence->SetEventOnCompletion(value, _idle_event);
    ::WaitForSingleObject(_idle_event, INFINITE);
}

// ファクトリ関数: CreateRhiDevice の DX12 実装
// Diligent バックエンドが有効化されている場合は RhiBackend.cpp が実装を提供する。
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept {
    auto d = MakeUnique<Dx12Device>();
    if (!d) return ACS_ERR(Memory, 200, "Dx12Device alloc failed");
    HrResult r = d->Init(cfg);
    if (r.IsErr()) {
        return ACS_ERR_OS(Render, 1, "Dx12Device::Init failed", static_cast<u32>(r.hr));
    }
    TUniquePtr<IRhiDevice> base(d.Release(), d.GetAllocator());
    return TResult<TUniquePtr<IRhiDevice>>(OkInit, Move(base));
}
#endif

} // namespace acs
