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

class IRhiCommandList {
public:
    virtual ~IRhiCommandList() noexcept = default;

    // 記録開始（毎フレーム最初に呼ぶ）
    virtual void Begin() noexcept = 0;

    // 記録終了（GPU 投入準備完了）
    virtual void End() noexcept = 0;

    // GPU に投入して完了を待つ（簡易実装、本来は Fence で非同期化）
    virtual void Submit() noexcept = 0;

    // バックバッファをレンダーターゲットとしてバインドし、クリアする
    // depth は省略可能（指定すると深度バッファもバインド + クリア）
    virtual void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                        const FClearColor& clear,
                                        class IRhiTexture* depth = nullptr,
                                        f32 depth_clear = 1.0f) noexcept = 0;

    // バックバッファ描画を終了し、Present 可能状態にする
    virtual void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept = 0;

    // シャドウパス: depth-only RT として bind + clear、ビューポートも自動設定
    virtual void BeginShadowPass(class IRhiTexture& depth, f32 depth_clear = 1.0f) noexcept = 0;

    // シャドウパス終了: depth を SHADER_RESOURCE 状態へ遷移（主パスでサンプル可能に）
    virtual void EndShadowPass(class IRhiTexture& depth) noexcept = 0;

    // オフスクリーンレンダーターゲットへの描画開始（HDR RT / ポストプロセス用）
    // depth は省略可。is_render_target=true の IRhiTexture を渡すこと。
    virtual void BeginRenderToTexture(class IRhiTexture& rt,
                                       const FClearColor& clear,
                                       class IRhiTexture* depth = nullptr,
                                       f32 depth_clear = 1.0f) noexcept = 0;

    // RT 描画を終了 → 次パスで SRV としてサンプルできる状態に遷移
    virtual void EndRenderToTexture(class IRhiTexture& rt) noexcept = 0;

    // RT を clear せずに再 bind する load 版。opaque pass 後に同じ HDR RT へ
    // さらに描画 (例: スクリーンスペース屈折オブジェクトを既存の opaque 上に
    // 追加描画) する用途で使う。BeginRenderToTexture と同じ resource state
    // 遷移と viewport / scissor 設定を行うが、clear は行わない。
    // 終了は EndRenderToTexture(rt) を呼ぶ (BeginRenderToTexture と共通)。
    //
    // 制約: depth を渡すとそれは DSV (DEPTH_WRITE) として bind される。
    // 同じ depth を SRV として sample したい場合 (例: 深度を読む SS 屈折) は
    // depth=nullptr を渡し、depth を SetTexture で SRV 経由で別途バインドする
    // こと (D3D12/Diligent はサブリソースの DEPTH_WRITE と SHADER_RESOURCE
    // 同時保持を禁じる)。
    virtual void BeginRenderToTextureLoad(class IRhiTexture& rt,
                                           class IRhiTexture* depth = nullptr) noexcept = 0;

    // cubemap 1 面 or 2D 配列 1 スライスに描画 (per_slice_rtv=true で作成済が前提)。
    // cubemap なら face は 0..5 (+X,-X,+Y,-Y,+Z,-Z の順)。mip は描画先 mip レベル。
    // 復帰は EndRenderToTexture と同じ挙動 (main pass RT を再 bind)。
    virtual void BeginRenderToTextureSlice(class IRhiTexture& rt,
                                            u32 slice, u32 mip,
                                            const FClearColor& clear) noexcept = 0;

    // MRT 描画開始 (Phase 34d-2)。最大 8 個の color RT を同時 bind、depth は optional。
    // クリア色は単一値で全 RT に適用 (個別クリアが要れば別 API か手動で SetTexture 前 clear)。
    // 終了は EndRenderToTexture(rts[0]) で main pass に復帰可。
    // Diligent backend で実装、Dx12 raw は stub (no-op)。
    virtual void BeginRenderToTextureMrt(class IRhiTexture* const* rts, u32 rt_count,
                                          const FClearColor& clear,
                                          class IRhiTexture* depth = nullptr,
                                          f32 depth_clear = 1.0f) noexcept = 0;

    // ビューポート / シザーを設定
    virtual void SetViewport(const FViewport& vp) noexcept = 0;
    virtual void SetScissor (const FScissorRect& sr) noexcept = 0;

    // パイプラインを設定（次の Draw 命令で使う VS+PS+入力レイアウト等）
    virtual void SetPipeline(class IRhiPipeline& pipeline) noexcept = 0;

    // 頂点バッファをスロット 0 にバインド（stride はパイプライン側で指定済み）
    virtual void SetVertexBuffer(class IRhiBuffer& vb, u32 stride) noexcept = 0;

    // インデックスバッファをバインド（type は Index16 / Index32）
    virtual void SetIndexBuffer(class IRhiBuffer& ib) noexcept = 0;

    // 定数バッファをスロット slot にバインド（パイプラインの cbuffer_slots > slot が必要）
    virtual void SetConstantBuffer(u32 slot, class IRhiBuffer& cb) noexcept = 0;

    // テクスチャをスロット slot にバインド（パイプラインの texture_slots > slot が必要）
    virtual void SetTexture(u32 slot, class IRhiTexture& tex) noexcept = 0;

    // 非インデックス描画
    virtual void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept = 0;

    // インデックス描画
    virtual void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept = 0;

    // バックエンド固有のネイティブハンドル取得（ImGui 等の外部統合で使う）
    virtual void* NativeHandle() noexcept = 0;
};

// コマンドリストを作成
TResult<TUniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
