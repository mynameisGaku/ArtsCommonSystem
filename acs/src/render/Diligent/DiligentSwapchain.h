// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の Swapchain
#pragma once

#include "render/IRhiSwapchain.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct ISwapChain;
}

namespace acs {

class DiligentDevice;

/**
 * Diligent Engine 経由のスワップチェイン (IRhiSwapchain 実装)。
 *
 * @details
 * D3D12 / Vulkan バックエンドそれぞれの factory でネイティブの ISwapChain を生成し、
 * Present / Resize を委譲する。バックバッファインデックスは Diligent が内部管理するため
 * AcquireNextImage は常に 0 を返す。深度は ACS 側で別途管理し、本クラスはカラーのみ持つ。
 */
class DiligentSwapchain final : public IRhiSwapchain {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    DiligentSwapchain() noexcept = default;

    /** Diligent スワップチェインを解放する。 */
    ~DiligentSwapchain() noexcept override;

    /** コピー禁止 (GPU リソースを単独所有するため)。 */
    DiligentSwapchain(const DiligentSwapchain&) = delete;

    /** コピー代入も禁止。 */
    DiligentSwapchain& operator=(const DiligentSwapchain&) = delete;

    /**
     * ウィンドウに対してスワップチェインを生成する。
     *
     * @details
     * バックエンドに応じて CreateSwapChainVk / CreateSwapChainD3D12 を呼び分ける。
     * buffer_count は 2..3 に丸め、範囲外は 2 にする。cfg.window が無効なら失敗する。
     * @param device スワップチェイン生成に使う Diligent デバイス。
     * @param cfg ウィンドウ・フォーマット・バッファ数・vsync を含むスワップチェイン設定。
     * @return 成功なら空の TResult、ウィンドウ無効・device 未初期化・生成失敗ならエラー。
     */
    TResult<void> Init(DiligentDevice& device, const SwapchainConfig& cfg) noexcept;

    /**
     * 次に描画するバックバッファのインデックスを返す。
     *
     * @details Diligent がバックバッファを内部管理するため実質 no-op で常に 0 を返す。
     * @return 常に 0。
     */
    u32  AcquireNextImage() noexcept override;

    /** 現在のバックバッファを画面に提示する (vsync 設定に従う)。 */
    void Present()          noexcept override;

    /**
     * スワップチェインを新しいサイズに作り直す。
     *
     * @param width 新しい幅 (0 は no-op 扱い)。
     * @param height 新しい高さ (0 は no-op 扱い)。
     * @return スワップチェイン未初期化なら false、それ以外 (無効サイズの no-op 含む) は true。
     */
    bool Resize(u32 width, u32 height) noexcept override;

    /**
     * バックバッファ数を返す。
     *
     * @return バッファ数 (2 または 3)。
     */
    u32  BufferCount() const noexcept override { return m_BufferCount; }

    /**
     * スワップチェインの幅を返す。
     *
     * @return 現在の幅。
     */
    u32  Width()       const noexcept override { return m_Width; }

    /**
     * スワップチェインの高さを返す。
     *
     * @return 現在の高さ。
     */
    u32  Height()      const noexcept override { return m_Height; }

    /**
     * Diligent ネイティブのスワップチェインを返す (内部用)。
     *
     * @return Diligent::ISwapChain へのポインタ (未初期化なら nullptr)。
     */
    Diligent::ISwapChain* SwapChain() const noexcept { return m_Swap; }

    /**
     * カラーバッファのピクセルフォーマットを返す。
     *
     * @return カラーバッファの EFormat。
     */
    EFormat                ColorFormat() const noexcept { return m_Format; }

private:
    /** スワップチェインを解放して空状態へ戻す。 */
    void Reset() noexcept;

    /** Init で受け取った Diligent デバイス。 */
    DiligentDevice*       m_Device       = nullptr;

    /** Diligent ネイティブのスワップチェイン。 */
    Diligent::ISwapChain* m_Swap         = nullptr;

    /** カラーバッファのピクセルフォーマット。 */
    EFormat                m_Format       = EFormat::B8G8R8A8_UNorm;

    /** 現在の幅。 */
    u32                   m_Width        = 0;

    /** 現在の高さ。 */
    u32                   m_Height       = 0;

    /** バックバッファ数 (2..3)。 */
    u32                   m_BufferCount = 2;

    /** 垂直同期フラグ (Present に渡す)。 */
    bool                  m_bVsync        = true;
};

} // namespace acs
