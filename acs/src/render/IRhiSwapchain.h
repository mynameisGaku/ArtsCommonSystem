// スワップチェイン抽象（バックバッファ管理 + 提示）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;
class Window;

// スワップチェイン作成オプション
struct SwapchainConfig {
    Window* window      = nullptr;
    Format  format      = Format::B8G8R8A8_UNorm;
    u32     buffer_count = 2;          // 2 = ダブルバッファ、3 = トリプルバッファ
    bool    vsync       = true;
};

class IRhiSwapchain {
public:
    virtual ~IRhiSwapchain() noexcept = default;

    // 描画前に呼ぶ — 次に書き込めるバックバッファをロックして取得
    // 戻り値はバックバッファインデックス（CommandList で参照する）
    virtual u32 AcquireNextImage() noexcept = 0;

    // 描画後に呼ぶ — 画面に反映
    virtual void Present() noexcept = 0;

    // ウィンドウサイズ変更時に呼ぶ
    virtual void Resize(u32 width, u32 height) noexcept = 0;

    // バックバッファ枚数 / 解像度
    virtual u32 BufferCount() const noexcept = 0;
    virtual u32 Width()       const noexcept = 0;
    virtual u32 Height()      const noexcept = 0;
};

// スワップチェインを作成
Result<UniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
                                                          const SwapchainConfig& cfg) noexcept;

} // namespace acs
