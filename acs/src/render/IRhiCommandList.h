// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;
class IRhiSwapchain;

/** 記録期間ごとの描画・compute命令数を保持する診断値。 */
struct FRhiCommandStatistics {
    /** 発行した有効なdraw命令数。 */
    u64 draw_calls = 0;

    /** 発行した有効なcompute dispatch命令数。 */
    u64 dispatch_calls = 0;

    /** triangle listとして頂点・index数から推定した三角形数。 */
    u64 triangles = 0;
};

/** Named regions reported by optional backend GPU timestamp instrumentation. */
enum class ERhiGpuTimingPass : u32 {
    Opaque = 0,
    Atmosphere,
    Cloud,
    Fog,
    Post,
    Count,
};

/**
 * Latest completed GPU timing result.
 *
 * Timestamp readback is intentionally asynchronous, so frame_index identifies
 * the rendered frame that produced the values rather than the current CPU
 * frame. Unsupported backends leave this structure invalid.
 */
struct FRhiGpuTimingSnapshot {
    bool valid = false;
    u64 frame_index = 0;
    f32 frame_ms = -1.0f;
    f32 opaque_ms = -1.0f;
    f32 atmosphere_ms = -1.0f;
    f32 cloud_ms = -1.0f;
    f32 fog_ms = -1.0f;
    f32 post_ms = -1.0f;
};

/**
 * Validate the backend-independent contract for a live D32 depth snapshot.
 *
 * The destination is both a depth allocation and a shader resource so it can
 * be sampled while the original source is rebound for hardware depth
 * testing/writes. Copying aliases, MSAA resources, arrays, or mismatched
 * allocations is rejected before any backend command is recorded.
 */
inline bool IsDepthTextureCopyCompatible(
    const IRhiTexture& source,
    const IRhiTexture& destination) noexcept {
    return &source != &destination &&
           source.IsDepthTarget() &&
           destination.IsDepthTarget() &&
           destination.IsShaderVisibleDepth() &&
           source.PixelFormat() == EFormat::D32_Float &&
           destination.PixelFormat() == source.PixelFormat() &&
           source.Width() > 0u &&
           source.Height() > 0u &&
           destination.Width() == source.Width() &&
           destination.Height() == source.Height() &&
           source.MipLevels() == 1u &&
           destination.MipLevels() == 1u &&
           source.ArraySize() == 1u &&
           destination.ArraySize() == 1u &&
           !source.IsCubemap() &&
           !destination.IsCubemap() &&
           source.SampleCount() == 1u &&
           destination.SampleCount() == 1u;
}

/**
 * GPU に送る命令を記録するコマンドリストの抽象インターフェイス。
 *
 * @details
 * Begin で記録を開始し、各種パスの bind / Draw / state 設定を積んで End で確定、
 * Submit で GPU に投入する。バックエンド (DX12 / Diligent 等) が実装する。
 */
class IRhiCommandList {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiCommandList() noexcept = default;

    /** 呼び出し側が定めたフレーム境界で軽量な命令統計を初期化する。 */
    void ResetStatistics() noexcept { StatisticsStorage() = {}; }

    /** 直前のResetStatistics以降に蓄積した命令統計を返す。 */
    const FRhiCommandStatistics& Statistics() const noexcept {
        return StatisticsStorage();
    }

    /** 記録を開始する (毎フレーム最初に呼ぶ)。 */
    virtual void Begin() noexcept = 0;

    /** 記録を終了する (GPU 投入準備完了)。 */
    virtual void End() noexcept = 0;

    /**
     * GPU に投入して完了を待つ (簡易実装、本来は GPU フェンスで非同期化)。
     *
     * @return キュー投入と、その完了を証明する fence の発行に成功したとき true。
     *         false の場合は Present やフレーム統計の公開を行ってはならない。
     */
    virtual bool Submit() noexcept = 0;

    /**
     * Return whether the current frame slot can be reset without a CPU fence
     * wait. Backends without an explicit frame-slot fence keep the legacy
     * always-ready behavior.
     */
    virtual bool CanBeginWithoutGpuWait() const noexcept { return true; }

    /**
     * Begin recording only when the current frame slot is immediately
     * reusable. The default preserves the existing backend contract; explicit
     * fence-ring backends override this to fail closed before allocator reset.
     */
    virtual bool TryBeginWithoutGpuWait() noexcept {
        Begin();
        return true;
    }

    /**
     * Submit without waiting for the next frame slot on the calling thread.
     * The default remains the normal Submit path. Backends that expose an
     * asynchronous fence ring override it for latency-sensitive editor hosts.
     */
    virtual bool SubmitWithoutGpuWait() noexcept { return Submit(); }

    /**
     * Start asynchronous GPU timestamp collection for one rendered frame.
     *
     * The default implementation is unsupported. Implementations must never
     * block solely to make query data available; they may consume data from a
     * completed frame slot and report it through TryGetGpuTiming().
     */
    virtual bool BeginGpuTimingFrame(u64 /*frame_index*/) noexcept {
        return false;
    }

    /** Start one non-overlapping named GPU region. */
    virtual bool BeginGpuTimingPass(ERhiGpuTimingPass /*pass*/) noexcept {
        return false;
    }

    /** End the currently active named GPU region. */
    virtual void EndGpuTimingPass() noexcept {}

    /** Finish timestamp collection and enqueue its asynchronous readback. */
    virtual void EndGpuTimingFrame() noexcept {}

    /** Return the latest completed timestamp result without consuming it. */
    virtual bool TryGetGpuTiming(
        FRhiGpuTimingSnapshot& out_snapshot) const noexcept {
        out_snapshot = {};
        return false;
    }

    /**
     * バックバッファをレンダーターゲットとしてバインドしクリアする。
     *
     * @details depth を渡すと深度バッファも併せてバインド + クリアする。
     * @param sc 描画先スワップチェイン。
     * @param buffer_index 描画するバックバッファのインデックス。
     * @param clear カラーバッファのクリア色。
     * @param depth 深度バッファ (省略可、既定 nullptr)。
     * @param depth_clear depth を渡したときのクリア値 (既定 1.0f)。
     */
    virtual void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                        const FClearColor& clear,
                                        class IRhiTexture* depth = nullptr,
                                        f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * Rebind an already rendered swapchain backbuffer without clearing it.
     *
     * Intended for final display-space editor overlays after post processing.
     * No depth target is bound, so callers must use an overlay pipeline.
     */
    virtual void BeginRenderToSwapchainLoad(
        IRhiSwapchain& sc, u32 buffer_index) noexcept = 0;

    /**
     * バックバッファ描画を終了し Present 可能状態にする。
     *
     * @param sc 描画先スワップチェイン。
     * @param buffer_index 描画したバックバッファのインデックス。
     */
    virtual void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept = 0;

    /**
     * MSAA レンダーターゲットをスワップチェインのバックバッファへ解決 (resolve) する。
     *
     * @details
     * sample_count>1 で作成した RT をフレームの最後にバックバッファへ ResolveSubresource する。
     * 呼出し後バックバッファは RENDER_TARGET 状態に戻る (続けて描画 / EndRenderToSwapchain 可)。
     * 既定実装は no-op (現状 Dx12 backend のみ実装)。
     * @param src 解決元の MSAA レンダーターゲット (描画済み)。
     * @param sc 解決先のスワップチェイン。
     * @param buffer_index 解決先バックバッファのインデックス。
     */
    virtual void ResolveToSwapchain(class IRhiTexture& /*src*/, IRhiSwapchain& /*sc*/,
                                    u32 /*buffer_index*/) noexcept {}

    /**
     * Record a full-resolution D32 depth snapshot entirely on the GPU.
     *
     * Implementations must reject incompatible resources using
     * IsDepthTextureCopyCompatible(), transition the source/destination
     * safely, and leave the source ready to be rebound as a writable DSV and
     * the destination ready for pixel-shader sampling. The caller must begin
     * or rebind its render pass after this call.
     *
     * @return true only when a copy command was successfully recorded.
     */
    virtual bool CopyDepthTexture(
        class IRhiTexture& /*source*/,
        class IRhiTexture& /*destination*/) noexcept {
        return false;
    }

    /**
     * シャドウパスを開始する (depth-only RT を bind + clear)。
     *
     * @details ビューポートも depth のサイズに合わせて自動設定する。
     * @param depth bind する depth-only レンダーターゲット。
     * @param depth_clear depth のクリア値 (既定 1.0f)。
     */
    virtual void BeginShadowPass(class IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * シャドウパスを終了する。
     *
     * @details depth を SHADER_RESOURCE 状態へ遷移し、主パスでサンプル可能にする。
     * @param depth 遷移させる depth テクスチャ。
     */
    virtual void EndShadowPass(class IRhiTexture& depth) noexcept = 0;

    /**
     * オフスクリーンレンダーターゲットへの描画を開始する (HDR RT / ポストプロセス用)。
     *
     * @details is_render_target=true で生成した IRhiTexture を渡すこと。
     * @param rt 描画先レンダーターゲット。
     * @param clear カラーバッファのクリア色。
     * @param depth 深度バッファ (省略可、既定 nullptr)。
     * @param depth_clear depth を渡したときのクリア値 (既定 1.0f)。
     */
    virtual void BeginRenderToTexture(class IRhiTexture& rt,
                                       const FClearColor& clear,
                                       class IRhiTexture* depth = nullptr,
                                       f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * RT 描画を終了し、次パスで SRV としてサンプルできる状態に遷移する。
     *
     * @param rt 遷移させるレンダーターゲット。
     */
    virtual void EndRenderToTexture(class IRhiTexture& rt) noexcept = 0;

    /**
     * RT を clear せずに再 bind する load 版の描画開始。
     *
     * @details
     * opaque pass 後に同じ HDR RT へさらに描画 (例: スクリーンスペース屈折オブジェクトを
     * 既存の opaque 上に追加描画) する用途で使う。BeginRenderToTexture と同じ resource state
     * 遷移と viewport / scissor 設定を行うが clear は行わない。終了は EndRenderToTexture(rt)
     * を呼ぶ (BeginRenderToTexture と共通)。depth を渡すとそれは DSV (DEPTH_WRITE) として
     * bind される。同じ depth を SRV として sample したい場合 (例: 深度を読む SS 屈折) は
     * depth=nullptr を渡し、depth を SetTexture で SRV 経由で別途バインドすること
     * (D3D12/Diligent はサブリソースの DEPTH_WRITE と SHADER_RESOURCE 同時保持を禁じる)。
     * @param rt 再 bind する (clear しない) レンダーターゲット。
     * @param depth DSV として bind する深度バッファ (省略可、既定 nullptr)。
     */
    virtual void BeginRenderToTextureLoad(class IRhiTexture& rt,
                                           class IRhiTexture* depth = nullptr) noexcept = 0;

    /**
     * cubemap の 1 面 or 2D 配列の 1 スライスへの描画を開始する。
     *
     * @details
     * per_slice_rtv=true で作成済みのテクスチャが前提。cubemap なら slice は 0..5
     * (+X,-X,+Y,-Y,+Z,-Z の順)。復帰は EndRenderToTexture と同じ挙動 (main pass RT を再 bind)。
     * @param rt 描画先テクスチャ (cubemap / 2D 配列)。
     * @param slice 描画する面 / スライスのインデックス。
     * @param mip 描画先の mip レベル。
     * @param clear カラーバッファのクリア色。
     */
    virtual void BeginRenderToTextureSlice(class IRhiTexture& rt,
                                            u32 slice, u32 mip,
                                            const FClearColor& clear) noexcept = 0;

    /**
     * MRT (複数レンダーターゲット) 描画を開始する。
     *
     * @details
     * 最大 8 個の color RT を同時 bind し、depth は optional。クリア色は単一値で全 RT に
     * 適用する (個別クリアが必要なら別 API か手動で SetTexture 前 clear)。
     * Diligent / raw DX12 の両 backend で実装する。後続 pass でサンプルする各 RT は、
     * 描画後に EndRenderToTextureMrt で一括して unbind / shader-resource state へ
     * 遷移させる。
     * @param rts color レンダーターゲットの配列。
     * @param rt_count rts の要素数 (最大 8)。
     * @param clear 全 RT に適用するクリア色。
     * @param depth 深度バッファ (省略可、既定 nullptr)。
     * @param depth_clear depth を渡したときのクリア値 (既定 1.0f)。
     * @return 全 attachment の検証と backend bind が完了したとき true。
     *         false の場合は draw を発行せず fallback path を使用すること。
     */
    virtual bool BeginRenderToTextureMrt(
        class IRhiTexture* const* rts,
        u32 rt_count,
        const FClearColor& clear,
        class IRhiTexture* depth = nullptr,
        f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * Bind MRTs while preserving selected existing attachments.
     *
     * Bit i in clear_mask clears color target i; unset bits use load
     * semantics. Depth is only cleared when clear_depth is true. This is used
     * when extending an already-rendered HDR scene with auxiliary G-buffer
     * targets without erasing sky color or opaque depth.
     *
     * @return 全 attachment の検証と backend bind が完了したとき true。
     *         false の場合、caller は MRT draw と対応する End を発行しないこと。
     */
    virtual bool BeginRenderToTextureMrtLoad(
        class IRhiTexture* const* rts,
        u32 rt_count,
        const FClearColor& clear,
        u32 clear_mask,
        class IRhiTexture* depth = nullptr,
        bool clear_depth = false,
        f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * End one MRT pass as a unit.
     *
     * The backend first unbinds the complete output set, then makes every
     * supplied color target sampleable. This avoids transitioning one target
     * while the same MRT binding still references it.
     */
    virtual void EndRenderToTextureMrt(
        class IRhiTexture* const* rts,
        u32 rt_count) noexcept = 0;

    /**
     * ビューポートを設定する。
     *
     * @param vp 設定するビューポート。
     */
    virtual void SetViewport(const FViewport& vp) noexcept = 0;

    /**
     * シザー矩形を設定する。
     *
     * @param sr 設定するシザー矩形。
     */
    virtual void SetScissor (const FScissorRect& sr) noexcept = 0;

    /**
     * ステンシル参照値を設定する。
     *
     * @details
     * stencil 有効パイプラインの Replace / 比較で使う。PSO ではなくコマンドリスト状態なので、
     * 同じ PSO で ref を切り替えられる。
     * @param ref 設定するステンシル参照値。
     */
    virtual void SetStencilRef(u32 ref) noexcept = 0;

    /**
     * パイプラインを設定する (次の Draw 命令で使う VS+PS+入力レイアウト等)。
     *
     * @param pipeline バインドするパイプライン。
     */
    virtual void SetPipeline(class IRhiPipeline& pipeline) noexcept = 0;

    /**
     * 頂点バッファをスロット 0 にバインドする。
     *
     * @param vb バインドする頂点バッファ。
     * @param stride 1 頂点のバイト数 (stride)。
     */
    virtual void SetVertexBuffer(class IRhiBuffer& vb, u32 stride) noexcept = 0;

    /**
     * インデックスバッファをバインドする。
     *
     * @details type は対象バッファの用途 (Index16 / Index32) で決まる。
     * @param ib バインドするインデックスバッファ。
     */
    virtual void SetIndexBuffer(class IRhiBuffer& ib) noexcept = 0;

    /**
     * 定数バッファを指定スロットにバインドする。
     *
     * @details パイプラインの cbuffer_slots > slot であることが必要。
     * @param slot バインド先の cbuffer スロット番号。
     * @param cb バインドする定数バッファ。
     */
    virtual void SetConstantBuffer(u32 slot, class IRhiBuffer& cb) noexcept = 0;

    /**
     * テクスチャを指定スロットにバインドする。
     *
     * @details パイプラインの texture_slots > slot であることが必要。
     * @param slot バインド先のテクスチャスロット番号。
     * @param tex バインドするテクスチャ。
     */
    virtual void SetTexture(u32 slot, class IRhiTexture& tex) noexcept = 0;

    /**
     * 非インデックス描画を行う。
     *
     * @param vertex_count 描画する頂点数。
     * @param first_vertex 開始頂点インデックス (既定 0)。
     */
    virtual void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept = 0;

    /**
     * インデックス描画を行う。
     *
     * @param index_count 描画するインデックス数。
     * @param first_index 開始インデックス (既定 0)。
     * @param base_vertex 各インデックスに加算する頂点オフセット (既定 0)。
     */
    virtual void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept = 0;

    /**
     * バックエンド固有のネイティブハンドルを返す。
     *
     * @details ImGui 等の外部統合で使う。
     * @return バックエンド固有のネイティブコマンドリストハンドル。
     */
    virtual void* NativeHandle() noexcept = 0;

    // ---- Compute (Phase 0: WickedEngine 流レンダラ移植の基盤) ----
    // 非 pure virtual + 既定空実装 → Dx12 raw backend は override せず no-op で済む
    // (Diligent のみ実装)。compute PSO は CreateRhiComputePipeline で生成。

    /** compute パイプラインを設定する (次の Dispatch で使う CS + リソース binding)。 */
    virtual void SetComputePipeline(class IRhiPipeline& /*pipeline*/) noexcept {}

    /** compute dispatch を発行する (スレッドグループ数 gx*gy*gz)。 */
    virtual void Dispatch(u32 /*gx*/, u32 /*gy*/, u32 /*gz*/) noexcept {}

    /** indirect compute dispatch。args バッファの byte_offset から u32x3
     *  (ThreadGroupCountX/Y/Z) を読んで dispatch する。args は indirect_args=true
     *  で作成したバッファ (compute が書いた ThreadGroupCount をそのまま使える)。 */
    virtual void DispatchIndirect(class IRhiBuffer& /*args*/, u32 /*byte_offset*/ = 0) noexcept {}

    /** UAV テクスチャ (RWTexture) を指定 slot にバインドする。is_uav=true で作成済みが前提。 */
    virtual void BindUav(u32 /*slot*/, class IRhiTexture& /*tex*/) noexcept {}

    /** UAV バッファ (RWStructuredBuffer 等) を指定 slot にバインドする。struct_stride>0 が前提。 */
    virtual void BindUav(u32 /*slot*/, class IRhiBuffer& /*buf*/) noexcept {}

    /** 構造化バッファ SRV (StructuredBuffer) を指定 slot にバインドする。struct_stride>0 が前提。 */
    virtual void BindStructuredSrv(u32 /*slot*/, class IRhiBuffer& /*buf*/) noexcept {}

protected:
    /**
     * 具象が所有する統計へ有効なdrawを記録する。
     *
     * @param statistics 更新する命令統計。
     * @param element_count drawへ渡す頂点またはindex数。
     */
    static void RecordDraw(FRhiCommandStatistics& statistics, u32 element_count) noexcept {
        if (element_count == 0u) return;
        ++statistics.draw_calls;
        statistics.triangles += static_cast<u64>(element_count / 3u);
    }

    /**
     * 具象が所有する統計へ有効なcompute dispatchを記録する。
     *
     * @param statistics 更新する命令統計。
     */
    static void RecordDispatch(FRhiCommandStatistics& statistics) noexcept {
        ++statistics.dispatch_calls;
    }

private:
    /**
     * 具象コマンドリストが所有する変更可能な命令統計を返す。
     *
     * @return このコマンドリストだけに属する統計領域。
     */
    virtual FRhiCommandStatistics& StatisticsStorage() noexcept = 0;

    /**
     * 具象コマンドリストが所有する読み取り専用の命令統計を返す。
     *
     * @return このコマンドリストだけに属する統計領域。
     */
    virtual const FRhiCommandStatistics& StatisticsStorage() const noexcept = 0;
};

/** RAII helper that keeps named GPU markers balanced on all early exits. */
class FScopedRhiGpuTiming final {
public:
    FScopedRhiGpuTiming(
        IRhiCommandList* command_list,
        ERhiGpuTimingPass pass) noexcept
        : m_CommandList(command_list),
          m_Active(command_list != nullptr &&
                   command_list->BeginGpuTimingPass(pass)) {}

    ~FScopedRhiGpuTiming() noexcept {
        if (m_Active) m_CommandList->EndGpuTimingPass();
    }

    FScopedRhiGpuTiming(const FScopedRhiGpuTiming&) = delete;
    FScopedRhiGpuTiming& operator=(const FScopedRhiGpuTiming&) = delete;

private:
    IRhiCommandList* m_CommandList = nullptr;
    bool m_Active = false;
};

/**
 * コマンドリストを作成する。
 *
 * @param device コマンドリスト生成に使う RHI デバイス。
 * @return 成功なら所有権付きコマンドリスト、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
