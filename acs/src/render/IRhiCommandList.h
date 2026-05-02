// コマンドリスト抽象（GPU に送る命令を記録するバッファ）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
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
    virtual void BeginRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index,
                                        const ClearColor& clear) noexcept = 0;

    // バックバッファ描画を終了し、Present 可能状態にする
    virtual void EndRenderToSwapchain(IRhiSwapchain& sc, u32 buffer_index) noexcept = 0;

    // ビューポート / シザーを設定
    virtual void SetViewport(const Viewport& vp) noexcept = 0;
    virtual void SetScissor (const ScissorRect& sr) noexcept = 0;

    // パイプラインを設定（次の Draw 命令で使う VS+PS+入力レイアウト等）
    virtual void SetPipeline(class IRhiPipeline& pipeline) noexcept = 0;

    // 頂点バッファをスロット 0 にバインド（stride はパイプライン側で指定済み）
    virtual void SetVertexBuffer(class IRhiBuffer& vb, u32 stride) noexcept = 0;

    // インデックスバッファをバインド（type は Index16 / Index32）
    virtual void SetIndexBuffer(class IRhiBuffer& ib) noexcept = 0;

    // 非インデックス描画
    virtual void Draw(u32 vertex_count, u32 first_vertex = 0) noexcept = 0;

    // インデックス描画
    virtual void DrawIndexed(u32 index_count, u32 first_index = 0, i32 base_vertex = 0) noexcept = 0;

    // バックエンド固有のネイティブハンドル取得（ImGui 等の外部統合で使う）
    virtual void* NativeHandle() noexcept = 0;
};

// コマンドリストを作成
Result<class UniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
