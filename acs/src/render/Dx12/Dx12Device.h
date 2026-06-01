// SPDX-License-Identifier: Apache-2.0
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
    IDXGIFactory6*    DxgiFactory()     const noexcept { return m_Factory; }
    ID3D12Device*     D3DDevice()       const noexcept { return m_Device; }
    ID3D12CommandQueue* GraphicsQueue() const noexcept { return m_GfxQueue; }

    // SRV/CBV/UAV 用シェーダ可視ヒープ（テクスチャ等が永続スロットを取る）
    ID3D12DescriptorHeap* SrvHeap()      const noexcept { return m_SrvHeap; }
    u32                   SrvHandleSize() const noexcept { return m_SrvHandleSize; }

    // SRV ヒープから 1 スロット確保（テクスチャ作成時に呼ばれる）
    // 戻り値: スロットインデックス（< m_SrvCapacity）。-1 は失敗。
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
    const char* AdapterName() const noexcept override { return m_AdapterName; }
    void WaitIdle() noexcept override;

    // フレーム Fence サポート: ExecuteCommandLists 後にこの番号を Signal して返す
    // 戻り値: その投入が完了する完了値。後で WaitForFenceValue() に渡す。
    u64  SignalGraphicsQueue() noexcept;

    // 指定 fence value 以上に到達するまで CPU 側で待つ（既に到達済みなら即 return）
    void WaitForFenceValue(u64 value) noexcept;

    // フレームスロット（kFramesInFlight 個。Uniform バッファ等が使うリングインデックス）
    static constexpr u32 kFramesInFlight = 2;
    u32  CurrentFrameSlot() const noexcept { return m_FrameSlot; }
    void AdvanceFrameSlot() noexcept { m_FrameSlot = (m_FrameSlot + 1) % kFramesInFlight; }

    // 初期化（CreateRhiDevice から呼ばれる）
    HrResult Init(const DeviceConfig& cfg) noexcept;

private:
    HrResult InitDescriptorHeaps() noexcept;

    IDXGIFactory6*       m_Factory   = nullptr;
    IDXGIAdapter1*       m_Adapter   = nullptr;
    ID3D12Device*        m_Device    = nullptr;
    ID3D12CommandQueue*  m_GfxQueue = nullptr;
    ID3D12Fence*         m_IdleFence = nullptr;
    HANDLE               m_IdleEvent = nullptr;
    u64                  m_IdleValue = 0;
    char                 m_AdapterName[128]{};
    u32                  m_FrameSlot = 0;

    // シェーダ可視 SRV ヒープ（簡易フリーリスト式）
    ID3D12DescriptorHeap* m_SrvHeap        = nullptr;
    u32                   m_SrvHandleSize = 0;
    static constexpr u32  kSrvCapacity = 1024;
    u32                   m_SrvHighWater  = 0;
    i32                   m_SrvFreeList[kSrvCapacity]{};
    u32                   m_SrvFreeCount  = 0;

    // DSV ヒープ（CPU のみ、小容量）
    ID3D12DescriptorHeap* m_DsvHeap        = nullptr;
    u32                   m_DsvHandleSize = 0;
    static constexpr u32  kDsvCapacity = 16;
    u32                   m_DsvHighWater  = 0;
    i32                   m_DsvFreeList[kDsvCapacity]{};
    u32                   m_DsvFreeCount  = 0;

    // RTV ヒープ（CPU のみ、オフスクリーン RT 用。BeginRenderToTexture で使う）
    ID3D12DescriptorHeap* m_RtvHeap        = nullptr;
    u32                   m_RtvHandleSize = 0;
    // IBL の per-slice RTV (prefilter cube=6面×5mip=30 + irradiance 6 + env 6 = 42) に加え、
    // HDR/bloom/ポストプロセスのオフスクリーン RT も同ヒープを使うため広めに確保する。
    // RTV 記述子は CPU ヒープ上で 1 個あたり数十バイトと小さく、増やしてもコストは僅少。
    static constexpr u32  kRtvCapacity = 256;
    u32                   m_RtvHighWater  = 0;
    i32                   m_RtvFreeList[kRtvCapacity]{};
    u32                   m_RtvFreeCount  = 0;
};

} // namespace acs
