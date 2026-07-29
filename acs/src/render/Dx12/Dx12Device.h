// SPDX-License-Identifier: Apache-2.0
#pragma once
// DX12 デバイス実装

#include "render/IRhiDevice.h"
#include "render/DescriptorSlotPool.h"
#include "render/PipelineStateKeyCache.h"
#include "render/Dx12/Dx12Common.h"
#include "container/Array.h"

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

    /** 複数の SRV/UAV スロットを一度の同期区間で確保する。 */
    bool AllocateSrvSlots(i32* output, u32 count) noexcept;

    /**
     * SRV ヒープスロットを返却する (テクスチャ破棄時)。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeSrvSlot(i32 index) noexcept;

    /** 複数の SRV/UAV スロットを一度の同期区間で返却する。 */
    void FreeSrvSlots(const i32* slots, u32 count) noexcept;

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

    /** 複数の DSV スロットを一度の同期区間で確保する。 */
    bool AllocateDsvSlots(i32* output, u32 count) noexcept;

    /**
     * DSV ヒープスロットを返却する。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeDsvSlot(i32 index) noexcept;

    /** 複数の DSV スロットを一度の同期区間で返却する。 */
    void FreeDsvSlots(const i32* slots, u32 count) noexcept;

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

    /** 複数の RTV スロットを一度の同期区間で確保する。 */
    bool AllocateRtvSlots(i32* output, u32 count) noexcept;

    /**
     * RTV ヒープスロットを返却する。
     *
     * @param index 返却するスロットインデックス (負値は無視)。
     */
    void FreeRtvSlot(i32 index) noexcept;

    /** 複数の RTV スロットを一度の同期区間で返却する。 */
    void FreeRtvSlots(const i32* slots, u32 count) noexcept;

    /**
     * Transfer an unreferenced buffer resource to the device retirement queue.
     *
     * The resource remains alive through the next graphics-queue submission
     * that can contain already-recorded references to it. The normal path
     * never waits for the GPU.
     */
    void RetireResource(ID3D12Resource* resource) noexcept;

    /**
     * Transfer a texture resource and all descriptor slots to retirement.
     *
     * Descriptor indices remain unavailable until the owning fence completes,
     * so an in-flight descriptor table cannot observe a replacement view.
     */
    void RetireTextureResource(
        ID3D12Resource* resource,
        i32 srv_slot, i32 uav_slot, i32 dsv_slot,
        TArray<i32>&& rtv_slots) noexcept;

    /** Release every retirement whose sealed queue fence has completed. */
    void CollectRetiredResources() noexcept;

    /** Number of transferred resources still pending or in flight. */
    usize RetiredResourceCount() noexcept;

    /**
     * Execute the main renderer command lists and signal their completion.
     *
     * Retirement sealing is part of the same queue-order transaction as
     * ExecuteCommandLists. Only this path may attach pending retirements to a
     * fence, because it is the only submission known to contain references
     * recorded by the main renderer command list.
     *
     * @return The signalled fence value, or 0 when execution could not be
     *         followed by a successful Signal.
     */
    u64 SubmitGraphicsCommandLists(
        ID3D12CommandList* const* command_lists,
        u32 command_list_count) noexcept;

    /**
     * Execute one transient upload/readback list and signal its completion.
     *
     * This deliberately does not seal pending retirements: an editor frame
     * may still be open and unsubmitted while the transient list is queued.
     * Sealing against this earlier fence could release a resource before the
     * main list which references it is ever executed.
     *
     * @return The signalled fence value, or 0 on failure.
     */
    u64 ExecuteOneOffGraphicsCommandList(
        ID3D12CommandList* command_list) noexcept;

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

    bool IsOperational() const noexcept override;

    /** mip0/slice0 (3D は depth slice 0) を CPU へ読み戻す。 */
    bool ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept override;

    /**
     * 現フレームの投入完了を表す fence 値を Signal して返す。
     *
     * @details ExecuteCommandLists の後に呼び、戻り値を後で WaitForFenceValue に渡す。
     * @return その投入が完了する fence 値。
     */
    /**
     * Signal already-submitted queue work without sealing retirements.
     *
     * A generic wait cannot prove that a currently open main command list has
     * been submitted, so sealing is restricted to SubmitGraphicsCommandLists.
     */
    u64 SignalGraphicsQueue() noexcept;

    /**
     * 指定 fence 値以上に到達するまで CPU 側で待つ (既に到達済みなら即座に返る)。
     *
     * @param value 待機する fence 値 (0 は no-op)。
     */
    void WaitForFenceValue(u64 value) noexcept;

    /**
     * Test a graphics fence without waiting. A zero fence represents an
     * unused frame slot and is therefore immediately reusable.
     */
    bool IsFenceComplete(u64 value) const noexcept
    {
        return value == 0u ||
               (m_IdleFence != nullptr &&
                m_IdleFence->GetCompletedValue() >= value);
    }

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
    friend class FDx12Pipeline;

    /** 同一 PSO key の native object を取得する。 */
    bool FindCachedPipeline(const FPipelineStateKey& key, ID3D12PipelineState*& pipeline, ID3D12RootSignature*& root_signature) noexcept;

    /** 作成済み native object を PSO key へ登録する。 */
    void StoreCachedPipeline(const FPipelineStateKey& key, ID3D12PipelineState* pipeline, ID3D12RootSignature* root_signature) noexcept;

    /** device が所有する PSO cache を解放または安全側で放棄する。 */
    void ResetPipelineCache(bool release_objects) noexcept;

    struct FRetiredResource {
        ID3D12Resource* resource = nullptr;
        i32 srv_slot = -1;
        i32 uav_slot = -1;
        i32 dsv_slot = -1;
        TArray<i32> rtv_slots;
        u64 fence_value = 0u;

        FRetiredResource() noexcept = default;
        FRetiredResource(const FRetiredResource&) = delete;
        FRetiredResource& operator=(const FRetiredResource&) = delete;

        FRetiredResource(FRetiredResource&& other) noexcept
            : resource(other.resource),
              srv_slot(other.srv_slot),
              uav_slot(other.uav_slot),
              dsv_slot(other.dsv_slot),
              rtv_slots(Move(other.rtv_slots)),
              fence_value(other.fence_value)
        {
            other.resource = nullptr;
            other.srv_slot = -1;
            other.uav_slot = -1;
            other.dsv_slot = -1;
            other.fence_value = 0u;
        }

        FRetiredResource& operator=(FRetiredResource&& other) noexcept
        {
            if (this == &other) return *this;
            resource = other.resource;
            srv_slot = other.srv_slot;
            uav_slot = other.uav_slot;
            dsv_slot = other.dsv_slot;
            rtv_slots = Move(other.rtv_slots);
            fence_value = other.fence_value;
            other.resource = nullptr;
            other.srv_slot = -1;
            other.uav_slot = -1;
            other.dsv_slot = -1;
            other.fence_value = 0u;
            return *this;
        }
    };

    /** 所有する Win32/COM リソースを解放し、再初期化可能な空状態へ戻す。 */
    void Reset() noexcept;

    /** Release one entry after its fence, or during final device teardown. */
    void ReleaseRetiredResource(FRetiredResource& retired) noexcept;

    /** Final teardown fallback after WaitIdle: release pending and sealed work. */
    void ReleaseAllRetiredResources() noexcept;

    /**
     * Drop retirement metadata without releasing GPU-visible objects.
     *
     * Used only when the final queue signal fails and completion is unknown.
     * Leaking is safer than releasing an object still referenced by the GPU.
     */
    void AbandonAllRetiredResources() noexcept;

    /** Queue a fully populated retirement record, with a fixed emergency path. */
    void QueueRetiredResource(FRetiredResource&& retired) noexcept;

    /**
     * Execute command lists and signal while holding the queue-order lock.
     *
     * seal_retirements is true only for the main renderer Submit path.
     */
    u64 ExecuteGraphicsCommandListsAndSignal(
        ID3D12CommandList* const* command_lists,
        u32 command_list_count,
        bool seal_retirements) noexcept;

    /** Signal helper; caller must hold m_QueueSubmissionLock. */
    u64 SignalGraphicsQueueLocked() noexcept;

    /** Attach all pending retirements to a successful main-submit fence. */
    void SealPendingRetirements(u64 fence_value) noexcept;

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
    /**
     * Serializes ExecuteCommandLists + Signal queue-order transactions.
     *
     * Main submission holds Retirement -> QueueSubmission so a retirement
     * cannot be inserted between Execute and sealing. One-off submissions
     * hold only QueueSubmission and never seal.
     */
    SRWLOCK m_QueueSubmissionLock = SRWLOCK_INIT;

    /** 単一の完了イベントを使う待機処理を直列化する SRW ロック。 */
    SRWLOCK m_FenceWaitLock = SRWLOCK_INIT;

    /**
     * Protects retirement insertion, signal sealing, and collection.
     *
     * Lock order is Retirement -> QueueSubmission and Retirement -> Descriptor.
     * No descriptor or fence path acquires this lock in the reverse order.
     */
    SRWLOCK m_RetirementLock = SRWLOCK_INIT;

    /**
     * fence_value==0 entries await the next main renderer Submit. One-off
     * upload/readback signals never seal these entries.
     */
    TArray<FRetiredResource> m_RetiredResources;

    /**
     * Allocation-free fallback when growing m_RetiredResources fails.
     *
     * The record already owns the texture's RTV-array storage, so moving it
     * into this fixed array does not allocate. Exhausting both stores retains
     * (leaks) the final record rather than risking an in-flight release.
     */
    static constexpr u32 kEmergencyRetirementCapacity = 256u;
    FRetiredResource
        m_EmergencyRetiredResources[kEmergencyRetirementCapacity]{};
    u32 m_EmergencyRetiredResourceCount = 0u;

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

    /** SRV/UAV スロットの再利用状態。 */
    TDescriptorSlotPool<kSrvCapacity> m_SrvSlots;

    /** DSV ヒープ (CPU のみ、小容量)。 */
    ID3D12DescriptorHeap* m_DsvHeap = nullptr;

    /** DSV デスクリプタ 1 個あたりのバイト数。 */
    u32 m_DsvHandleSize = 0;

    /** DSV ヒープの容量。 */
    static constexpr u32 kDsvCapacity = 16;

    /** DSV スロットの再利用状態。 */
    TDescriptorSlotPool<kDsvCapacity> m_DsvSlots;

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

    /** RTV スロットの再利用状態。 */
    TDescriptorSlotPool<kRtvCapacity> m_RtvSlots;

    /** PSO cache の固定容量。 */
    static constexpr u32 kPipelineCacheCapacity = 512u;

    /** PSO key を native 配列 index へ intern する表。 */
    TPipelineStateKeyCache<kPipelineCacheCapacity> m_PipelineKeyCache;

    /** cache が所有する PSO。 */
    ID3D12PipelineState* m_CachedPipelineStates[kPipelineCacheCapacity]{};

    /** cache が所有する root signature。 */
    ID3D12RootSignature* m_CachedRootSignatures[kPipelineCacheCapacity]{};

    /** PSO cache の検索と登録を直列化する lock。 */
    SRWLOCK m_PipelineCacheLock = SRWLOCK_INIT;
};

} // namespace acs
