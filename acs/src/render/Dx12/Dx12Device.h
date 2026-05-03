// DX12 デバイス実装
#pragma once

#include "render/IRhiDevice.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device final : public IRhiDevice {
public:
    Dx12Device() noexcept = default;
    ~Dx12Device() noexcept override;

    // 内部使用: DXGI ファクトリ・D3D12 デバイス・コマンドキューを取得
    IDXGIFactory6*    DxgiFactory()     const noexcept { return _factory; }
    ID3D12Device*     D3DDevice()       const noexcept { return _device; }
    ID3D12CommandQueue* GraphicsQueue() const noexcept { return _gfx_queue; }

    // SRV/CBV/UAV 用シェーダ可視ヒープ（テクスチャ等が永続スロットを取る）
    ID3D12DescriptorHeap* SrvHeap()      const noexcept { return _srv_heap; }
    u32                   SrvHandleSize() const noexcept { return _srv_handle_size; }

    // SRV ヒープから 1 スロット確保（テクスチャ作成時に呼ばれる）
    // 戻り値: スロットインデックス（< _srv_capacity）。-1 は失敗。
    i32  AllocateSrvSlot() noexcept;

    // SRV ヒープスロットを返却（テクスチャ破棄時）
    void FreeSrvSlot(i32 index) noexcept;

    // 指定スロットの CPU/GPU ハンドル
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(i32 index) const noexcept;
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(i32 index) const noexcept;

    // DSV 専用ヒープ（深度バッファ用、シェーダ可視ではない）
    i32  AllocateDsvSlot() noexcept;
    void FreeDsvSlot(i32 index) noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle(i32 index) const noexcept;

    // RTV 専用ヒープ（オフスクリーン RT 用、シェーダ可視ではない）
    i32  AllocateRtvSlot() noexcept;
    void FreeRtvSlot(i32 index) noexcept;
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(i32 index) const noexcept;

    const char* BackendName() const noexcept override { return "DX12"; }
    const char* AdapterName() const noexcept override { return _adapter_name; }
    void WaitIdle() noexcept override;

    // フレーム Fence サポート: ExecuteCommandLists 後にこの番号を Signal して返す
    // 戻り値: その投入が完了する完了値。後で WaitForFenceValue() に渡す。
    u64  SignalGraphicsQueue() noexcept;

    // 指定 fence value 以上に到達するまで CPU 側で待つ（既に到達済みなら即 return）
    void WaitForFenceValue(u64 value) noexcept;

    // フレームスロット（kFramesInFlight 個。Uniform バッファ等が使うリングインデックス）
    static constexpr u32 kFramesInFlight = 2;
    u32  CurrentFrameSlot() const noexcept { return _frame_slot; }
    void AdvanceFrameSlot() noexcept { _frame_slot = (_frame_slot + 1) % kFramesInFlight; }

    // 初期化（CreateRhiDevice から呼ばれる）
    HrResult Init(const DeviceConfig& cfg) noexcept;

private:
    HrResult InitDescriptorHeaps() noexcept;

    IDXGIFactory6*       _factory   = nullptr;
    IDXGIAdapter1*       _adapter   = nullptr;
    ID3D12Device*        _device    = nullptr;
    ID3D12CommandQueue*  _gfx_queue = nullptr;
    ID3D12Fence*         _idle_fence = nullptr;
    HANDLE               _idle_event = nullptr;
    u64                  _idle_value = 0;
    char                 _adapter_name[128]{};
    u32                  _frame_slot = 0;

    // シェーダ可視 SRV ヒープ（簡易フリーリスト式）
    ID3D12DescriptorHeap* _srv_heap        = nullptr;
    u32                   _srv_handle_size = 0;
    static constexpr u32  kSrvCapacity = 1024;
    u32                   _srv_high_water  = 0;
    i32                   _srv_free_list[kSrvCapacity]{};
    u32                   _srv_free_count  = 0;

    // DSV ヒープ（CPU のみ、小容量）
    ID3D12DescriptorHeap* _dsv_heap        = nullptr;
    u32                   _dsv_handle_size = 0;
    static constexpr u32  kDsvCapacity = 16;
    u32                   _dsv_high_water  = 0;
    i32                   _dsv_free_list[kDsvCapacity]{};
    u32                   _dsv_free_count  = 0;
};

} // namespace acs
