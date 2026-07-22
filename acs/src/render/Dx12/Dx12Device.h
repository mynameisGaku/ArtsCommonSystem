// SPDX-License-Identifier: Apache-2.0
// DX12 デバイス実装
#pragma once

#include "render/IRhiDevice.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

/**
 * IRhiDevice の DX12 実装。
 *
 * @details
 * DXGI ファクトリ・D3D12 デバイス・グラフィックスキューを所有し、SRV/DSV/RTV の
 * 各デスクリプタヒープを簡易フリーリストで管理する。WaitIdle/フレーム fence による
 * GPU 同期と、kFramesInFlight スロットのリングインデックスも提供する。
 */
class FDx12Device final : public IRhiDevice {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FDx12Device() noexcept = default;

    /** GPU の完了を待ってから全 COM リソースを解放する。 */
    ~FDx12Device() noexcept override;

    /**
     * DXGI ファクトリを返す (内部使用)。
     *
     * @return 所有する IDXGIFactory6。
     */
    IDXGIFactory6* DxgiFactory() const noexcept
    {
        return m_Factory;
    }

    /**
     * D3D12 デバイスを返す (内部使用)。
     *
     * @return 所有する ID3D12Device。
     */
    ID3D12Device* D3DDevice() const noexcept
    {
        return m_Device;
    }

    /**
     * グラフィックスコマンドキューを返す (内部使用)。
     *
     * @return 所有する ID3D12CommandQueue (DIRECT)。
     */
    ID3D12CommandQueue* GraphicsQueue() const noexcept
    {
        return m_GfxQueue;
    }

    /**
     * SRV/CBV/UAV 用のシェーダ可視デスクリプタヒープを返す。
     *
     * @return テクスチャ等が永続スロットを取る共有 SRV ヒープ。
     */
    ID3D12DescriptorHeap* SrvHeap() const noexcept
    {
        return m_SrvHeap;
    }

    /**
     * SRV ヒープの 1 デスクリプタあたりのバイト数を返す。
     *
     * @return SRV デスクリプタのハンドル増分サイズ。
     */
    u32 SrvHandleSize() const noexcept
    {
        return m_SrvHandleSize;
    }

    /**
     * SRV ヒープから 1 スロットを確保する (テクスチャ作成時に呼ばれる)。
     *
     * @details フリーリストにあればそれを再利用し、無ければ high-water を進める。
     * @return スロットインデックス (< kSrvCapacity)。容量超過なら -1。
     */
    i32 AllocateSrvSlot() noexcept;

    /**
     * SRV ヒープスロットを返却する (テクスチャ破棄時)。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeSrvSlot(i32 index) noexcept;

    /**
     * 指定 SRV スロットの CPU デスクリプタハンドルを返す。
     *
     * @param index SRV スロットインデックス。
     * @return 対応する CPU ハンドル。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(i32 index) const noexcept;

    /**
     * 指定 SRV スロットの GPU デスクリプタハンドルを返す。
     *
     * @param index SRV スロットインデックス。
     * @return 対応する GPU ハンドル。
     */
    D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(i32 index) const noexcept;

    /**
     * DSV 専用ヒープから 1 スロットを確保する (深度バッファ用、シェーダ可視ではない)。
     *
     * @return スロットインデックス (< kDsvCapacity)。容量超過なら -1。
     */
    i32 AllocateDsvSlot() noexcept;

    /**
     * DSV ヒープスロットを返却する。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeDsvSlot(i32 index) noexcept;

    /**
     * 指定 DSV スロットの CPU デスクリプタハンドルを返す。
     *
     * @param index DSV スロットインデックス。
     * @return 対応する CPU ハンドル。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE DsvCpuHandle(i32 index) const noexcept;

    /**
     * RTV 専用ヒープから 1 スロットを確保する (オフスクリーン RT 用、シェーダ可視ではない)。
     *
     * @return スロットインデックス (< kRtvCapacity)。容量超過なら -1。
     */
    i32 AllocateRtvSlot() noexcept;

    /**
     * RTV ヒープスロットを返却する。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeRtvSlot(i32 index) noexcept;

    /**
     * 指定 RTV スロットの CPU デスクリプタハンドルを返す。
     *
     * @param index RTV スロットインデックス。
     * @return 対応する CPU ハンドル。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE RtvCpuHandle(i32 index) const noexcept;

    /**
     * バックエンド名を返す。
     *
     * @return 文字列 "DX12"。
     */
    const char* BackendName() const noexcept override
    {
        return "DX12";
    }

    /**
     * 選択したアダプタ (GPU) 名を返す。
     *
     * @return UTF-8 化したアダプタ名。
     */
    const char* AdapterName() const noexcept override
    {
        return m_AdapterName;
    }

    /** キューに積まれた全コマンドの GPU 完了を CPU 側で待つ。 */
    void WaitIdle() noexcept override;

    /** mip0/slice0 (3D は depth slice 0) を CPU へ読み戻す。 */
    bool ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept override;

    /**
     * 現フレームの投入完了を表す fence 値を Signal して返す。
     *
     * @details ExecuteCommandLists の後に呼び、戻り値を後で WaitForFenceValue に渡す。
     * @return その投入が完了する fence 値。
     */
    u64 SignalGraphicsQueue() noexcept;

    /**
     * 指定 fence 値以上に到達するまで CPU 側で待つ (既に到達済みなら即座に返る)。
     *
     * @param value 待機する fence 値 (0 は no-op)。
     */
    void WaitForFenceValue(u64 value) noexcept;

    /** フレームスロット数 (定数バッファ等のリングインデックスの周期)。 */
    static constexpr u32 kFramesInFlight = 2;

    /**
     * 現在のフレームスロットを返す。
     *
     * @return 0..kFramesInFlight-1 のリングインデックス。
     */
    u32 CurrentFrameSlot() const noexcept
    {
        return m_FrameSlot;
    }

    /** フレームスロットを次へ進める (kFramesInFlight で周回)。 */
    void AdvanceFrameSlot() noexcept
    {
        m_FrameSlot = (m_FrameSlot + 1) % kFramesInFlight;
    }

    /**
     * デバイス・キュー・ヒープを初期化する (CreateRhiDevice から呼ばれる)。
     *
     * @details
     * デバッグレイヤ→DXGI ファクトリ→アダプタ列挙 (WARP 除外)→D3D12 デバイス→
     * グラフィックスキュー→idle fence/event→デスクリプタヒープの順に構築する。
     * @param configuration デバッグレイヤ有効化・高性能 GPU 優先などの構成。
     * @return 成功なら IsOk な FHrResult、いずれかの段階で失敗ならその HRESULT。
     */
    FHrResult Init(const FDeviceConfig& configuration) noexcept;

private:
    /** 所有する Win32/COM リソースを解放し、再初期化可能な空状態へ戻す。 */
    void Reset() noexcept;

    /**
     * SRV/DSV/RTV のデスクリプタヒープを生成する。
     *
     * @return 成功なら IsOk な FHrResult、生成失敗ならその HRESULT。
     */
    FHrResult InitDescriptorHeaps() noexcept;

    /** 所有する DXGI ファクトリ。 */
    IDXGIFactory6* m_Factory = nullptr;

    /** 選択したアダプタ (GPU)。 */
    IDXGIAdapter1* m_Adapter = nullptr;

    /** 所有する D3D12 デバイス。 */
    ID3D12Device* m_Device = nullptr;

    /** グラフィックスコマンドキュー (DIRECT)。 */
    ID3D12CommandQueue* m_GfxQueue = nullptr;

    /** WaitIdle/フレーム同期で共有する fence。 */
    ID3D12Fence* m_IdleFence = nullptr;

    /** fence 完了通知を受けるイベントハンドル。 */
    HANDLE m_IdleEvent = nullptr;

    /** 単調増加する fence の現在値。 */
    u64 m_IdleValue = 0;

    /** fence 値の採番と Signal を直列化する SRW ロック。 */
    SRWLOCK m_FenceSignalLock = SRWLOCK_INIT;

    /** 単一の完了イベントを使う待機処理を直列化する SRW ロック。 */
    SRWLOCK m_FenceWaitLock = SRWLOCK_INIT;

    /** UTF-8 化したアダプタ名バッファ。 */
    char m_AdapterName[128]{};

    /** 現在のフレームスロット (リングインデックス)。 */
    u32 m_FrameSlot = 0;

    /** シェーダ可視 SRV ヒープ (簡易フリーリスト式)。 */
    ID3D12DescriptorHeap* m_SrvHeap = nullptr;

    /** 3 種のデスクリプタ確保・返却を直列化する SRW ロック。 */
    SRWLOCK m_DescriptorLock = SRWLOCK_INIT;

    /** SRV デスクリプタ 1 個あたりのバイト数。 */
    u32 m_SrvHandleSize = 0;

    /** SRV ヒープの容量。 */
    static constexpr u32 kSrvCapacity = 1024;

    /** SRV ヒープの未割り当て先頭位置 (high-water)。 */
    u32 m_SrvHighWater = 0;

    /** 返却された SRV スロットのフリーリスト。 */
    i32 m_SrvFreeList[kSrvCapacity]{};

    /** フリーリストに積まれた SRV スロット数。 */
    u32 m_SrvFreeCount = 0;

    /** DSV ヒープ (CPU のみ、小容量)。 */
    ID3D12DescriptorHeap* m_DsvHeap = nullptr;

    /** DSV デスクリプタ 1 個あたりのバイト数。 */
    u32 m_DsvHandleSize = 0;

    /** DSV ヒープの容量。 */
    static constexpr u32 kDsvCapacity = 16;

    /** DSV ヒープの未割り当て先頭位置 (high-water)。 */
    u32 m_DsvHighWater = 0;

    /** 返却された DSV スロットのフリーリスト。 */
    i32 m_DsvFreeList[kDsvCapacity]{};

    /** フリーリストに積まれた DSV スロット数。 */
    u32 m_DsvFreeCount = 0;

    /** RTV ヒープ (CPU のみ、オフスクリーン RT 用。BeginRenderToTexture で使う)。 */
    ID3D12DescriptorHeap* m_RtvHeap = nullptr;

    /** RTV デスクリプタ 1 個あたりのバイト数。 */
    u32 m_RtvHandleSize = 0;

    /**
     * RTV ヒープの容量。
     *
     * @details
     * IBL の per-slice RTV (prefilter cube=6面×5mip=30 + irradiance 6 + env 6 = 42) に加え、
     * HDR/bloom/ポストプロセスのオフスクリーン RT も同ヒープを使うため広めに確保する。
     * RTV 記述子は CPU ヒープ上で 1 個あたり数十バイトと小さく、増やしてもコストは僅少。
     */
    static constexpr u32 kRtvCapacity = 256;

    /** RTV ヒープの未割り当て先頭位置 (high-water)。 */
    u32 m_RtvHighWater = 0;

    /** 返却された RTV スロットのフリーリスト。 */
    i32 m_RtvFreeList[kRtvCapacity]{};

    /** フリーリストに積まれた RTV スロット数。 */
    u32 m_RtvFreeCount = 0;
};

} // namespace acs
