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
};

// コマンドリストを作成
Result<class UniquePtr<IRhiCommandList>> CreateRhiCommandList(IRhiDevice& device) noexcept;

} // namespace acs
