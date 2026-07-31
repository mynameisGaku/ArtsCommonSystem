// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "render/IRhiCommandList.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class FDx12Device;

/**
 * IRhiCommandList の DX12 実装。
 *
 * @details
 * kFramesInFlight 個のコマンドアロケータをリング状に持ち、各フレームで Begin→描画→
 * Submit する。Submit で GPU 完了 fence を Signal し、次に再利用するスロットの完了を
 * 待ってから返すため UPLOAD ヒープの書き込み競合を避ける。スワップチェイン/オフスクリーン
 * RT/シャドウ depth へのレンダーパス開始・終了でリソースバリアを発行する。
 */
class FDx12CommandList final : public IRhiCommandList {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    FDx12CommandList() noexcept = default;

    /** 全 fence の完了を待ってからコマンドリスト・アロケータを解放する。 */
    ~FDx12CommandList() noexcept override;

    /**
     * コマンドアロケータとコマンドリストを生成する。
     *
     * @details kFramesInFlight 個の DIRECT アロケータを作り、作成直後の Open 状態を Close する。
     * @param device 生成に使う DX12 デバイス。
     * @return 成功なら IsOk な FHrResult、生成失敗なら失敗 HRESULT。
     */
    FHrResult Init(FDx12Device& device) noexcept;

    /** 現フレームスロットのアロケータを Reset し、コマンド記録を開始する (共有 SRV ヒープを bind)。 */
    void Begin() noexcept override;

    /** コマンド記録を終了し、コマンドリストを Close する。 */
    void End()   noexcept override;

    /** コマンドリストをキューへ投入し、GPU 完了 fence を Signal して次スロットを待つ。 */
    bool Submit() noexcept override;

    bool CanBeginWithoutGpuWait() const noexcept override;
    bool TryBeginWithoutGpuWait() noexcept override;
    bool SubmitWithoutGpuWait() noexcept override;

    bool BeginGpuTimingFrame(u64 frame_index) noexcept override;
    bool BeginGpuTimingPass(ERhiGpuTimingPass pass) noexcept override;
    void EndGpuTimingPass() noexcept override;
    void EndGpuTimingFrame() noexcept override;
    bool TryGetGpuTiming(
        FRhiGpuTimingSnapshot& out_snapshot) const noexcept override;

    /**
     * スワップチェインのバックバッファを RT として bind し、クリアしてパスを開始する。
     *
     * @details
     * バックバッファ未取得なら何もしない。Present→RenderTarget バリアは冪等で、
     * 同一フレーム内にオフスクリーン RT を挟んで再 bind しても安全。
     * @param sc 対象スワップチェイン。
     * @param buffer_index 描画先バックバッファのインデックス。
     * @param clear カラーバッファのクリア色。
     * @param depth 併用する深度バッファ (不要なら nullptr)。
     * @param depth_clear 深度クリア値 (既定 1.0)。
     */
    void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                const FClearColor& clear,
                                IRhiTexture* depth = nullptr,
                                f32 depth_clear = 1.0f) noexcept override;
    void BeginRenderToSwapchainLoad(
        IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    /**
     * バックバッファを RenderTarget→Present へ戻してパスを終了する。
     *
     * @param sc 対象スワップチェイン。
     * @param buffer_index 描画していたバックバッファのインデックス。
     */
    void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    /**
     * シャドウ用 depth-only パスを開始する (depth を DEPTH_WRITE に遷移し bind・クリア)。
     *
     * @param depth 描画先の深度テクスチャ。
     * @param depth_clear 深度クリア値 (既定 1.0)。
     */
    void BeginShadowPass(IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept override;

    /**
     * シャドウパスを終了し、depth を PIXEL_SHADER_RESOURCE へ遷移する。
     *
     * @param depth シャドウパスで使った深度テクスチャ。
     */
    void EndShadowPass(IRhiTexture& depth) noexcept override;

    /**
     * オフスクリーン RT を bind し、クリアしてパスを開始する。
     *
     * @details RT を RENDER_TARGET へ遷移し、viewport/scissor を RT サイズに合わせる。
     * @param rt 描画先のレンダーターゲットテクスチャ。
     * @param clear カラーバッファのクリア色。
     * @param depth 併用する深度バッファ (不要なら nullptr)。
     * @param depth_clear 深度クリア値 (既定 1.0)。
     */
    void BeginRenderToTexture(IRhiTexture& rt, const FClearColor& clear,
                              IRhiTexture* depth = nullptr,
                              f32 depth_clear = 1.0f) noexcept override;

    /**
     * オフスクリーン RT のパスを終了し、PIXEL_SHADER_RESOURCE へ遷移する。
     *
     * @param rt パスで使ったレンダーターゲットテクスチャ。
     */
    void EndRenderToTexture(IRhiTexture& rt) noexcept override;

    /**
     * MSAA RT をバックバッファへ ResolveSubresource で解決する。
     *
     * @details 解決後バックバッファは RENDER_TARGET 状態に戻る (EndRenderToSwapchain 可)。
     * @param src 解決元の MSAA レンダーターゲット。
     * @param sc 解決先のスワップチェイン。
     * @param buffer_index 解決先バックバッファのインデックス。
     */
    void ResolveToSwapchain(IRhiTexture& src, IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    bool CopyDepthTexture(
        IRhiTexture& source,
        IRhiTexture& destination) noexcept override;

    /**
     * クリアせずにオフスクリーン RT を再 bind してパスを開始する (load 版)。
     *
     * @details SS 屈折など既存内容を保持したまま追記描画する用途で使う。
     * @param rt 描画先のレンダーターゲットテクスチャ。
     * @param depth 併用する深度バッファ (不要なら nullptr)。
     */
    void BeginRenderToTextureLoad(IRhiTexture& rt,
                                   IRhiTexture* depth = nullptr) noexcept override;

    /**
     * cubemap の 1 面 / 配列の 1 スライス / 1 mip を bind しクリアしてパスを開始する。
     *
     * @details
     * per_slice_rtv=true で作成された RT 用。IBL の env/irradiance/prefilter 各面・各 mip を
     * 焼くのに使う。viewport/scissor は描画先 mip の寸法に合わせる。
     * @param rt 描画先のレンダーターゲットテクスチャ。
     * @param slice 描画先のスライス (cube 面など)。
     * @param mip 描画先の mip レベル。
     * @param clear カラーバッファのクリア色。
     */
    void BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                    const FClearColor& clear) noexcept override;

    /**
     * 複数レンダーターゲット (MRT) を bind してパスを開始する。
     *
     * @details 全 attachment を先に検証し、bind を完了した場合だけ true を返す。
     * @param rts レンダーターゲットの配列。
     * @param rt_count レンダーターゲットの数。
     * @param clear カラーバッファのクリア色。
     * @param depth 併用する深度バッファ (不要なら nullptr)。
     * @param depth_clear 深度クリア値 (既定 1.0)。
     */
    bool BeginRenderToTextureMrt(
        IRhiTexture* const* rts, u32 rt_count,
        const FClearColor& clear,
        IRhiTexture* depth = nullptr,
        f32 depth_clear = 1.0f) noexcept override;
    bool BeginRenderToTextureMrtLoad(
        IRhiTexture* const* rts, u32 rt_count,
        const FClearColor& clear, u32 clear_mask,
        IRhiTexture* depth = nullptr, bool clear_depth = false,
        f32 depth_clear = 1.0f) noexcept override;
    void EndRenderToTextureMrt(
        IRhiTexture* const* rts,
        u32 rt_count) noexcept override;

    /**
     * ビューポートを設定する。
     *
     * @param vp 適用するビューポート。
     */
    void SetViewport(const FViewport& vp) noexcept override;

    /**
     * シザー矩形を設定する。
     *
     * @param sr 適用するシザー矩形。
     */
    void SetScissor (const FScissorRect& sr) noexcept override;

    /**
     * ステンシル参照値を設定する。
     *
     * @param ref ステンシルテストの参照値。
     */
    void SetStencilRef(u32 ref) noexcept override;

    /**
     * パイプライン (PSO + RootSignature + トポロジ) を bind する。
     *
     * @param pipeline bind するパイプライン。
     */
    void SetPipeline(IRhiPipeline& pipeline) noexcept override;

    /**
     * 頂点バッファをスロット 0 に bind する。
     *
     * @param vb bind する頂点バッファ。
     * @param stride 1 頂点あたりのバイト数。
     */
    void SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept override;

    /**
     * インデックスバッファを bind する (16/32bit は Usage から判定)。
     *
     * @param ib bind するインデックスバッファ。
     */
    void SetIndexBuffer(IRhiBuffer& ib) noexcept override;

    /**
     * 定数バッファを指定スロットの root CBV に bind する。
     *
     * @details bind 済みパイプラインの cbuffer_slots 外の slot は無視される。
     * @param slot 定数バッファのスロット (b0..)。
     * @param cb bind する定数バッファ。
     */
    void SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept override;

    /**
     * テクスチャを指定スロットの descriptor table に bind する。
     *
     * @details root index は cbuffer_slots + slot。texture_slots 外の slot は無視される。
     * @param slot テクスチャのスロット (t0..)。
     * @param tex bind するテクスチャ。
     */
    void SetTexture(u32 slot, IRhiTexture& tex) noexcept override;

    /** Raw DX12 compute PSO/root signature を bind する。 */
    void SetComputePipeline(IRhiPipeline& pipeline) noexcept override;

    /** IRhiTexture の UAV を u-slot へ bind する。 */
    void BindUav(u32 slot, IRhiTexture& tex) noexcept override;

    /** Compute thread groups を dispatch する。 */
    void Dispatch(u32 gx, u32 gy, u32 gz) noexcept override;

    /**
     * 非インデックス描画を発行する。
     *
     * @param vertex_count 描画する頂点数。
     * @param first_vertex 開始頂点インデックス (既定 0)。
     */
    void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept override;

    /**
     * インデックス描画を発行する。
     *
     * @param index_count 描画するインデックス数。
     * @param first_index 開始インデックス位置 (既定 0)。
     * @param base_vertex インデックスに加算する頂点オフセット (既定 0)。
     */
    void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept override;

    /**
     * ネイティブな ID3D12GraphicsCommandList を返す。
     *
     * @return DX12 コマンドリストへの不透明ポインタ。
     */
    void* NativeHandle() noexcept override { return m_CmdList; }

private:
    /** Raw DX12が所有する変更可能な命令統計を返す。 */
    FRhiCommandStatistics& StatisticsStorage() noexcept override { return m_CommandStatistics; }

    /** Raw DX12が所有する読み取り専用の命令統計を返す。 */
    const FRhiCommandStatistics& StatisticsStorage() const noexcept override { return m_CommandStatistics; }

    bool BeginCurrentSlot() noexcept;
    bool SubmitInternal(bool wait_for_next_slot) noexcept;
    static constexpr u32 kGpuTimingFrameSlots = 2;
    static constexpr u32 kGpuTimingQueriesPerSlot = 32;
    static constexpr u32 kGpuTimingSegmentsPerSlot = 15;
    static constexpr u32 kInvalidGpuTimingQuery = ~0u;

    struct FGpuTimingSegment {
        ERhiGpuTimingPass pass = ERhiGpuTimingPass::Opaque;
        u32 begin_query = 0;
        u32 end_query = 0;
    };

    struct FGpuTimingSlot {
        u64 frame_index = 0;
        u32 query_count = 0;
        u32 segment_count = 0;
        bool pending = false;
        FGpuTimingSegment segments[kGpuTimingSegmentsPerSlot]{};
    };

    /** 必要なら投入済み fence を待ち、COM 所有物と記録状態を空に戻す。 */
    void Reset(bool wait_for_gpu) noexcept;
    void ResetGpuTiming() noexcept;
    void CollectGpuTiming(u32 slot) noexcept;
    u32 EmitGpuTimestamp() noexcept;

    /** このRaw DX12コマンドリストだけに属する命令統計。 */
    FRhiCommandStatistics m_CommandStatistics{};

    /** Init で受け取った DX12 デバイス (fence 待ち・キュー投入に使う)。 */
    FDx12Device*                     m_Device      = nullptr;

    /** フレームスロットごとのコマンドアロケータ (kFramesInFlight=2)。 */
    ID3D12CommandAllocator*         _allocators[2] {};

    /** 各アロケータの最後の投入に対する GPU 完了 fence 値。 */
    u64                             m_FrameFences[2] {};

    /** 記録対象のグラフィックスコマンドリスト。 */
    ID3D12GraphicsCommandList*      m_CmdList    = nullptr;

    ID3D12QueryHeap* m_GpuTimestampHeap = nullptr;
    ID3D12Resource* m_GpuTimestampReadback = nullptr;
    u64* m_GpuTimestampReadbackData = nullptr;
    u64 m_GpuTimestampFrequency = 0;
    FGpuTimingSlot m_GpuTimingSlots[kGpuTimingFrameSlots]{};
    FRhiGpuTimingSnapshot m_LatestGpuTiming{};
    u32 m_GpuTimingRecordingSlot = 0;
    u32 m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
    ERhiGpuTimingPass m_GpuTimingActivePass =
        ERhiGpuTimingPass::Opaque;
    bool m_GpuTimingSupported = false;
    bool m_GpuTimingRecording = false;
    bool m_GpuTimingScopeActive = false;

    /** 現在 bind 中のパイプライン (CBuffer/Texture の root index 解決に使う)。 */
    class FDx12Pipeline*             m_BoundPipe  = nullptr;

    /** コマンド記録中なら true (Begin..End の間)。 */
    bool                            _open        = false;

    /**
     * バックバッファが RENDER_TARGET 状態かを示すフラグ。
     *
     * @details
     * Begin/EndRenderToSwapchain のバリアを冪等化し、同一フレームでオフスクリーン RT を
     * 挟んで再バインドしても安全にする。
     */
    bool                            m_BackbufferIsRt = false;
};

} // namespace acs
