// SPDX-License-Identifier: Apache-2.0
// コマンドリスト抽象（GPU に送る命令を記録するバッファ）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;
class IRhiSwapchain;

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

    /** 記録を開始する (毎フレーム最初に呼ぶ)。 */
    virtual void Begin() noexcept = 0;

    /** 記録を終了する (GPU 投入準備完了)。 */
    virtual void End() noexcept = 0;

    /** GPU に投入して完了を待つ (簡易実装、本来は Fence で非同期化)。 */
    virtual void Submit() noexcept = 0;

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
                                        const ClearColor& clear,
                                        class IRhiTexture* depth = nullptr,
                                        f32 depth_clear = 1.0f) noexcept = 0;

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
                                       const ClearColor& clear,
                                       class IRhiTexture* depth = nullptr,
                                       f32 depth_clear = 1.0f) noexcept = 0;

    /**
     * RT 描画を終了し、次パスで SRV としてサンプルできる状態に遷移する。
     *
     * @param rt 遷移させるレンダーターゲット。
     */
    virtual void EndRenderToTexture(class IRhiTexture& rt) noexcept = 0;

    /**
     * 「main pass への自動復帰先」(BeginRenderToSwapchain で記憶した swapchain RTV + depth DSV) を破棄する。
     *
     * @details これ以降の EndRenderToTexture は «復帰しない» (noop) になる。フレーム冒頭で
     * BeginRenderToSwapchain 済みでも、その後に off-screen サブパス (SSAO の motion/GTAO 等) を
     * «共有 depth を再 bind せずに» 連ねたい場合に呼ぶ。復帰先は次の BeginRenderToSwapchain で再設定される。
     * 既定は noop (復帰追従を持たない backend 用)。Diligent backend が実装する。
     */
    virtual void ClearMainPass() noexcept {}

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
                                            const ClearColor& clear) noexcept = 0;

    /**
     * MRT (複数レンダーターゲット) 描画を開始する。
     *
     * @details
     * 最大 8 個の color RT を同時 bind し、depth は optional。クリア色は単一値で全 RT に
     * 適用する (個別クリアが必要なら別 API か手動で SetTexture 前 clear)。終了は
     * EndRenderToTexture(rts[0]) で main pass に復帰できる。Diligent backend で実装、
     * Dx12 raw は stub (no-op)。
     * @param rts color レンダーターゲットの配列。
     * @param rt_count rts の要素数 (最大 8)。
     * @param clear 全 RT に適用するクリア色。
     * @param depth 深度バッファ (省略可、既定 nullptr)。
     * @param depth_clear depth を渡したときのクリア値 (既定 1.0f)。
     */
    virtual void BeginRenderToTextureMrt(class IRhiTexture* const* rts, u32 rt_count,
                                          const ClearColor& clear,
                                          class IRhiTexture* depth = nullptr,
                                          f32 depth_clear = 1.0f) noexcept = 0;

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
};

/**
 * コマンドリストを作成する。
 *
 * @param device コマンドリスト生成に使う RHI デバイス。
 * @return 成功なら所有権付きコマンドリスト、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
