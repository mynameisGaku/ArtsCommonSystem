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
                                        const ClearColor& clear,
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
                                       const ClearColor& clear,
                                       class IRhiTexture* depth = nullptr,
                                       f32 depth_clear = 1.0f) noexcept = 0;

    // RT 描画を終了 → 次パスで SRV としてサンプルできる状態に遷移
    virtual void EndRenderToTexture(class IRhiTexture& rt) noexcept = 0;

    // cubemap 1 面 or 2D 配列 1 スライスに描画 (per_slice_rtv=true で作成済が前提)。
    // cubemap なら face は 0..5 (+X,-X,+Y,-Y,+Z,-Z の順)。mip は描画先 mip レベル。
    // 復帰は EndRenderToTexture と同じ挙動 (main pass RT を再 bind)。
    virtual void BeginRenderToTextureSlice(class IRhiTexture& rt,
                                            u32 slice, u32 mip,
                                            const ClearColor& clear) noexcept = 0;

    // ビューポート / シザーを設定
    virtual void SetViewport(const Viewport& vp) noexcept = 0;
    virtual void SetScissor (const ScissorRect& sr) noexcept = 0;

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
Result<UniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
