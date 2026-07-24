// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由のコマンドリスト
// Diligent は IDeviceContext 自体がコマンドキューを持つ即時 API なので、
// ACS の CommandList 概念は IDeviceContext の薄いラッパとして実装する。
#pragma once

#include "render/IRhiCommandList.h"
#include "memory/UniquePtr.h"

namespace Diligent {
struct IQuery;
}

namespace acs {

class FDiligentDevice;
class FDiligentPipeline;
class FDiligentSwapchain;
class FDiligentTexture;

/**
 * Diligent の IDeviceContext を薄くラップした即時コマンドリスト実装。
 *
 * @details
 * Diligent は IDeviceContext 自体がコマンドキューを持つ即時 API なので、明示的な
 * Begin/End は no-op で、Submit でのみ Flush + フェンス Signal + フレームスロット前進を
 * 行う。RT 種別を跨ぐ pass (shadow / off-screen) の後に main pass へ復帰できるよう、
 * フレーム冒頭で bind した swap chain / depth を記憶する。
 */
class FDiligentCommandList final : public IRhiCommandList {
public:
    /** 空状態で構築する (実体は Init で device に結び付ける)。 */
    FDiligentCommandList() noexcept = default;

    /** 破棄する (保持するのは生ポインタのみで所有権は持たない)。 */
    ~FDiligentCommandList() noexcept override;

    /** コピー禁止 (device コンテキストへの参照を単独で持つため)。 */
    FDiligentCommandList(const FDiligentCommandList&) = delete;

    /** コピー代入も禁止。 */
    FDiligentCommandList& operator=(const FDiligentCommandList&) = delete;

    /**
     * device に結び付けてコマンドリストを初期化する。
     *
     * @param device コマンドを積む先の Diligent デバイス。
     * @return 常に成功 (空の TResult)。
     */
    TResult<void> Init(FDiligentDevice& device) noexcept;

    /** 記録開始フック (Diligent は即時 API のため no-op)。 */
    void Begin() noexcept override;

    /** 記録終了フック (Diligent は即時 API のため no-op)。 */
    void End() noexcept override;

    /** フレーム終了タイミングで Flush + フェンス Signal + フレームスロット前進を行う。 */
    void Submit() noexcept override;

    bool BeginGpuTimingFrame(u64 frame_index) noexcept override;
    bool BeginGpuTimingPass(ERhiGpuTimingPass pass) noexcept override;
    void EndGpuTimingPass() noexcept override;
    void EndGpuTimingFrame() noexcept override;
    bool TryGetGpuTiming(
        FRhiGpuTimingSnapshot& out_snapshot) const noexcept override;

    /**
     * バックバッファをレンダーターゲットとして bind しクリアする。
     *
     * @details depth を渡すと深度バッファも bind + clear する。main pass 復帰用に
     * swap chain と depth を記憶する。
     * @param sc 描画先スワップチェイン。
     * @param buffer_index バックバッファのインデックス (現状未使用)。
     * @param clear カラークリア値。
     * @param depth bind + clear する深度テクスチャ (省略時 nullptr)。
     * @param depth_clear 深度のクリア値 (既定 1.0)。
     */
    void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                const FClearColor& clear,
                                IRhiTexture* depth = nullptr,
                                f32 depth_clear = 1.0f) noexcept override;

    /**
     * バックバッファ描画を終了する (Present 状態への遷移は Diligent が自動で行うため no-op)。
     *
     * @param sc 描画先スワップチェイン。
     * @param buffer_index バックバッファのインデックス。
     */
    void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept override;

    bool CopyDepthTexture(
        IRhiTexture& source,
        IRhiTexture& destination) noexcept override;

    /**
     * シャドウパスを開始する (depth-only RT として bind + clear、viewport/scissor も自動設定)。
     *
     * @param depth 描画先の深度テクスチャ。
     * @param depth_clear 深度のクリア値 (既定 1.0)。
     */
    void BeginShadowPass(IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept override;

    /**
     * シャドウパスを終了し main pass の RT/viewport へ復帰する。
     *
     * @details depth の DSV→SRV 遷移は次の SetTexture で Diligent が自動で行うため、
     * ここでは BeginRenderToSwapchain で記憶した swap chain RTV + main pass DSV を再 bind する。
     * @param depth シャドウパスで描画した深度テクスチャ。
     */
    void EndShadowPass(IRhiTexture& depth) noexcept override;

    /**
     * オフスクリーン RT への描画を開始する (HDR RT / ポストプロセス用)。
     *
     * @param rt 描画先 (is_render_target=true のテクスチャ)。
     * @param clear カラークリア値。
     * @param depth bind + clear する深度テクスチャ (省略時 nullptr)。
     * @param depth_clear 深度のクリア値 (既定 1.0)。
     */
    void BeginRenderToTexture(IRhiTexture& rt, const FClearColor& clear,
                              IRhiTexture* depth = nullptr,
                              f32 depth_clear = 1.0f) noexcept override;

    /**
     * オフスクリーン RT 描画を終了し main pass へ復帰する。
     *
     * @details RTV→SRV 遷移は次の SetTexture で Diligent が自動で行う。RT の復帰は
     * BeginRenderToSwapchain で記憶した内容を再 bind する (復帰先が無ければ no-op)。
     * @param rt 描画していた RT テクスチャ。
     */
    void EndRenderToTexture(IRhiTexture& rt) noexcept override;

    /**
     * クリアせずに RT を再 bind する load 版 (opaque pass 後に同じ HDR RT へ追加描画する用途)。
     *
     * @details viewport/scissor は dst RT サイズに揃える。clear は行わない。終了は
     * EndRenderToTexture を使う。depth を渡すとそれは DSV (DEPTH_WRITE) として bind される。
     * @param rt 追加描画先の RT テクスチャ。
     * @param depth bind する深度テクスチャ (省略時 nullptr)。
     */
    void BeginRenderToTextureLoad(IRhiTexture& rt,
                                  IRhiTexture* depth = nullptr) noexcept override;

    /**
     * cubemap 1 面または 2D 配列 1 スライスへの描画を開始する。
     *
     * @details per_slice_rtv=true で作成済が前提。mip に応じて viewport も縮小する。
     * 復帰は EndRenderToTexture と同じ挙動 (main pass RT を再 bind)。
     * @param rt 描画先 RT テクスチャ。
     * @param slice 描画先スライス (cubemap なら 0..5 = +X,-X,+Y,-Y,+Z,-Z)。
     * @param mip 描画先 mip レベル。
     * @param clear カラークリア値。
     */
    void BeginRenderToTextureSlice(IRhiTexture& rt, u32 slice, u32 mip,
                                   const FClearColor& clear) noexcept override;

    /**
     * 複数 RT を同時 bind する MRT 描画を開始する (最大 8 個、depth は optional)。
     *
     * @details null、RTV 無し、attachment 間の寸法・sample count 不一致は
     * false で拒否する。成功時の viewport は RT0 のサイズに合わせる。
     * クリア色は単一値で全 RT に適用する。
     * @param rts color RT のポインタ配列。
     * @param rt_count rts の要素数 (1..8)。
     * @param clear 全 RT に適用するカラークリア値。
     * @param depth bind + clear する深度テクスチャ (省略時 nullptr)。
     * @param depth_clear 深度のクリア値 (既定 1.0)。
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
    void SetScissor(const FScissorRect& sr) noexcept override;

    /**
     * ステンシル参照値を設定する (同じ PSO で ref を切り替えられる)。
     *
     * @param ref ステンシル参照値。
     */
    void SetStencilRef(u32 ref) noexcept override;

    /**
     * 次の Draw で使うパイプライン (VS+PS+入力レイアウト等) を設定する。
     *
     * @param pipeline 設定するパイプライン。
     */
    void SetPipeline(IRhiPipeline& pipeline) noexcept override;

    /**
     * 頂点バッファをスロット 0 に bind する。
     *
     * @param vb bind する頂点バッファ。
     * @param stride 頂点ストライド (パイプライン側で指定済みのため未使用)。
     */
    void SetVertexBuffer(IRhiBuffer& vb, u32 stride) noexcept override;

    /**
     * インデックスバッファを bind する (Index16/Index32 を usage から判定)。
     *
     * @param ib bind するインデックスバッファ。
     */
    void SetIndexBuffer(IRhiBuffer& ib) noexcept override;

    /**
     * 定数バッファをスロット slot に bind する。
     *
     * @details Pipeline が保持する名前で VS/PS の SRB 変数を lookup して Set する。
     * @param slot 定数バッファのスロット番号。
     * @param cb bind する定数バッファ。
     */
    void SetConstantBuffer(u32 slot, IRhiBuffer& cb) noexcept override;

    /**
     * テクスチャをスロット slot に bind する。
     *
     * @details Pipeline が保持する名前で PS の SRB 変数を lookup して SRV を Set する。
     * @param slot テクスチャのスロット番号。
     * @param tex bind するテクスチャ。
     */
    void SetTexture(u32 slot, IRhiTexture& tex) noexcept override;

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
     * @param base_vertex 各インデックスに加算するベース頂点 (既定 0)。
     */
    void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept override;

    /**
     * バックエンド固有のネイティブハンドルを返す (ImGui 等の外部統合で使う)。
     *
     * @return Diligent の IDeviceContext へのポインタ (未初期化なら nullptr)。
     */
    void* NativeHandle() noexcept override;

    // ---- Compute (Phase 0) ----
    /** compute パイプラインを設定する (次の Dispatch で使う CS + SRB)。 */
    void SetComputePipeline(IRhiPipeline& pipeline) noexcept override;
    /** compute dispatch (スレッドグループ数)。bind 済み UAV を UNORDERED_ACCESS へ遷移してから発行。 */
    void Dispatch(u32 gx, u32 gy, u32 gz) noexcept override;
    /** indirect compute dispatch。args バッファ(u32x3)の byte_offset から ThreadGroupCount を読む。 */
    void DispatchIndirect(IRhiBuffer& args, u32 byte_offset = 0) noexcept override;
    /** UAV テクスチャ (RWTexture) を slot に bind。 */
    void BindUav(u32 slot, IRhiTexture& tex) noexcept override;
    /** UAV バッファ (RWStructuredBuffer) を slot に bind。 */
    void BindUav(u32 slot, IRhiBuffer& buf) noexcept override;
    /** 構造化バッファ SRV (StructuredBuffer) を slot に bind。 */
    void BindStructuredSrv(u32 slot, IRhiBuffer& buf) noexcept override;

private:
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

    void ResetGpuTiming() noexcept;
    void CollectGpuTiming(u32 slot) noexcept;
    u32 EmitGpuTimestamp() noexcept;

    /** コマンドを積む先の Diligent デバイス。 */
    FDiligentDevice*    m_Device   = nullptr;

    Diligent::IQuery*
        m_GpuTimestampQueries[kGpuTimingFrameSlots]
                             [kGpuTimingQueriesPerSlot] = {};
    FGpuTimingSlot m_GpuTimingSlots[kGpuTimingFrameSlots]{};
    FRhiGpuTimingSnapshot m_LatestGpuTiming{};
    u32 m_GpuTimingRecordingSlot = 0;
    u32 m_GpuTimingActiveBegin = kInvalidGpuTimingQuery;
    ERhiGpuTimingPass m_GpuTimingActivePass =
        ERhiGpuTimingPass::Opaque;
    bool m_GpuTimingSupported = false;
    bool m_GpuTimingRecording = false;
    bool m_GpuTimingScopeActive = false;

    /** Dispatch 前に UNORDERED_ACCESS へ遷移する、現在 bind 中の UAV テクスチャ。 */
    FDiligentTexture*   m_BoundUavTex[16] = {};
    /** bind 中 UAV テクスチャ数。 */
    u32                m_BoundUavTexCount = 0;

    /** 現在 bind 中のパイプライン (SetConstantBuffer/SetTexture の名前 lookup に使う)。 */
    FDiligentPipeline*  m_Pipeline = nullptr;

    /** インデックスバッファが 32bit かどうか (DrawIndexed の IndexType に使う)。 */
    bool               m_bIsIndex32 = false;

    /** フレーム冒頭で bind した main pass の swap chain (shadow/off-screen pass 後の復帰先)。 */
    FDiligentSwapchain* m_MainSwapchain     = nullptr;

    /** フレーム冒頭で bind した main pass の depth (復帰時に再 bind する)。 */
    FDiligentTexture*   m_MainDepth         = nullptr;
};

} // namespace acs
