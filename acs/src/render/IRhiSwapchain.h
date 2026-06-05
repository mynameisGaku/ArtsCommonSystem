// SPDX-License-Identifier: Apache-2.0
// スワップチェイン抽象（バックバッファ管理 + 提示）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiDevice;
class FWindow;

/**
 * スワップチェイン生成パラメータ。
 *
 * @details
 * 提示先ウィンドウ、バックバッファのフォーマット・枚数、垂直同期の有無を指定する。
 * CreateRhiSwapchain に渡してスワップチェインを生成する。
 */
struct SwapchainConfig {
    /** 提示先のウィンドウ。 */
    FWindow* window      = nullptr;

    /** バックバッファのピクセルフォーマット。 */
    EFormat  format      = EFormat::B8G8R8A8_UNorm;

    /** バックバッファ枚数（2 = ダブルバッファ、3 = トリプルバッファ）。 */
    u32     buffer_count = 2;

    /** 垂直同期（VSync）の有無。 */
    bool    vsync       = true;
};

/**
 * バックバッファの管理と画面提示を担うスワップチェイン抽象。
 *
 * @details
 * 描画前に AcquireNextImage で書き込み先バックバッファを取得し、描画後に Present で
 * 画面へ反映する。ウィンドウサイズ変更時は Resize でバッファを作り直す。
 */
class IRhiSwapchain {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiSwapchain() noexcept = default;

    /**
     * 次に書き込めるバックバッファをロックして取得する（描画前に呼ぶ）。
     *
     * @return 取得したバックバッファのインデックス（CommandList で参照する）。
     */
    virtual u32 AcquireNextImage() noexcept = 0;

    /** 描画済みバックバッファを画面に反映する（描画後に呼ぶ）。 */
    virtual void Present() noexcept = 0;

    /**
     * ウィンドウサイズ変更時にバックバッファを作り直す。
     *
     * @details
     * false が返った場合はバッファ再取得に失敗してバックバッファ未取得状態のため、
     * 呼び出し側は本フレームの描画をスキップし次フレームで再試行すること
     * （null バックバッファ参照を避ける）。
     * @param width 新しい幅。
     * @param height 新しい高さ。
     * @return 成功（または無効サイズ・同一サイズで no-op）なら true、リサイズ失敗なら false。
     */
    virtual bool Resize(u32 width, u32 height) noexcept = 0;

    /**
     * バックバッファの枚数を返す。
     *
     * @return バックバッファ枚数。
     */
    virtual u32 BufferCount() const noexcept = 0;

    /**
     * バックバッファの幅を返す。
     *
     * @return バックバッファの幅（ピクセル）。
     */
    virtual u32 Width()       const noexcept = 0;

    /**
     * バックバッファの高さを返す。
     *
     * @return バックバッファの高さ（ピクセル）。
     */
    virtual u32 Height()      const noexcept = 0;
};

/**
 * スワップチェインを生成する。
 *
 * @param device スワップチェイン生成に使う RHI デバイス。
 * @param cfg スワップチェイン生成パラメータ。
 * @return 成功なら所有権付きの IRhiSwapchain、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiSwapchain>> CreateRhiSwapchain(IRhiDevice& device,
                                                          const SwapchainConfig& cfg) noexcept;

} // namespace acs
