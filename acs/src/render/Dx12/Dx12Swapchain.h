// SPDX-License-Identifier: Apache-2.0
// DX12 スワップチェイン実装
#pragma once

#include "render/IRhiSwapchain.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

/**
 * IRhiSwapchain の DX12 実装。
 *
 * @details
 * IDXGISwapChain3 と RTV ヒープを所有し、最大 kMaxBuffers 個のバックバッファと
 * その RTV を管理する。FLIP_DISCARD + ALLOW_TEARING で生成し、vsync 設定に応じて
 * Present の同期間隔とフラグを切り替える。Resize は GPU の idle を待ってから
 * バッファを作り直す。
 */
class Dx12Swapchain final : public IRhiSwapchain {
public:
    /** 空状態で構築する (GPU リソースは Init で確保)。 */
    Dx12Swapchain() noexcept = default;

    /** バックバッファ・RTV ヒープ・スワップチェインを解放する。 */
    ~Dx12Swapchain() noexcept override;

    /**
     * 指定インデックスのバックバッファリソースを返す (内部使用)。
     *
     * @details 範囲外 index は nullptr を返す (Resize 失敗直後の未取得状態でも安全に判定できる)。
     * @param i バックバッファのインデックス。
     * @return バックバッファの ID3D12Resource (範囲外なら nullptr)。
     */
    ID3D12Resource*             BackBuffer(u32 i)        const noexcept {
        return (i < m_BufferCount) ? m_BackBuffers[i] : nullptr;
    }

    /**
     * 指定インデックスのバックバッファ RTV の CPU ハンドルを返す (内部使用)。
     *
     * @param i バックバッファのインデックス。
     * @return 対応する RTV の CPU デスクリプタハンドル。
     */
    D3D12_CPU_DESCRIPTOR_HANDLE BackBufferRTV(u32 i)     const noexcept;

    /**
     * バックバッファ RTV を保持するデスクリプタヒープを返す (内部使用)。
     *
     * @return RTV デスクリプタヒープ。
     */
    ID3D12DescriptorHeap*       RtvHeap()                const noexcept { return m_RtvHeap; }

    /**
     * スワップチェインと RTV ヒープを生成し、バックバッファを取得する。
     *
     * @param device 生成に使う DX12 デバイス。
     * @param cfg バッファ数・vsync・フォーマット・対象ウィンドウなどの構成。
     * @return 成功なら IsOk な HrResult、いずれかの段階で失敗ならその HRESULT。
     */
    HrResult Init(Dx12Device& device, const SwapchainConfig& cfg) noexcept;

    /**
     * 次に描画すべきバックバッファのインデックスを返す。
     *
     * @return 現在のバックバッファインデックス。
     */
    u32  AcquireNextImage() noexcept override;

    /** バックバッファを表示する (vsync 設定で同期間隔と tearing フラグを切替)。 */
    void Present() noexcept override;

    /**
     * スワップチェインを指定サイズに作り直す。
     *
     * @details
     * サイズ 0 や変化なしは no-op で成功扱い。GPU の idle を待ってから ResizeBuffers し、
     * バッファ再取得が成功した場合のみ寸法を確定する (失敗時は旧寸法を保つ)。
     * @param width 新しい幅。
     * @param height 新しい高さ。
     * @return 成功・no-op なら true、ResizeBuffers/再取得失敗なら false。
     */
    bool Resize(u32 width, u32 height) noexcept override;

    /**
     * バックバッファ数を返す。
     *
     * @return 確保したバックバッファ数。
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

private:
    /** バックバッファを含む全所有物を解放し、空状態へ戻す。 */
    void Reset() noexcept;

    /** 全バックバッファを取得済みかを返す。 */
    bool HasAllBuffers() const noexcept;

    /** 取得済みの全バックバッファを解放する。 */
    void ReleaseBuffers() noexcept;

    /**
     * 各バックバッファの ID3D12Resource を取得し RTV を作成する。
     *
     * @details 途中で失敗した場合は取得済みバッファを全解放して全 null に戻す。
     * @param device RTV 作成に使う DX12 デバイス。
     * @return 成功なら IsOk な HrResult、取得失敗ならその HRESULT。
     */
    HrResult AcquireBuffers(Dx12Device& device) noexcept;

    /** Init で受け取った DX12 デバイス (Resize での WaitIdle/再取得に使う)。 */
    Dx12Device*          m_Device       = nullptr;

    /** 所有する DXGI スワップチェイン (SwapChain3)。 */
    IDXGISwapChain3*     m_Swapchain    = nullptr;

    /** バックバッファ RTV を保持するデスクリプタヒープ。 */
    ID3D12DescriptorHeap* m_RtvHeap    = nullptr;

    /** RTV デスクリプタ 1 個あたりのバイト数。 */
    u32                  m_RtvSize     = 0;

    /** バックバッファ配列の最大数。 */
    static constexpr u32 kMaxBuffers = 3;

    /** バックバッファリソースの配列。 */
    ID3D12Resource*      m_BackBuffers[kMaxBuffers] {};

    /** 実際に確保したバックバッファ数 (2..kMaxBuffers)。 */
    u32                  m_BufferCount = 0;

    /** スワップチェインの幅。 */
    u32                  m_Width        = 0;

    /** スワップチェインの高さ。 */
    u32                  m_Height       = 0;

    /** vsync 有効フラグ (Present の同期間隔と tearing を決める)。 */
    bool                 m_bVsync        = true;

    /** 実行環境が可変リフレッシュの tearing 表示を許可しているか。 */
    bool m_bAllowTearing = false;
};

} // namespace acs
