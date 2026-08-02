// SPDX-License-Identifier: Apache-2.0
// RHI デバイス抽象（GPU との対話を表す。DX12 / Vulkan で実装される）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiSwapchain;
class IRhiCommandList;
class IRhiTexture;
class FWindow;

/**
 * グラフィックスデバイスの抽象インターフェイス (GPU との対話を表す)。
 *
 * @details DX12 / Vulkan 等のバックエンドが実装する。生成は CreateRhiDevice で行う。
 */
class IRhiDevice {
public:
    /** 派生バックエンド実装を正しく破棄するための仮想デストラクタ。 */
    virtual ~IRhiDevice() noexcept = default;

    /**
     * バックエンド名を返す。
     *
     * @return バックエンド名 ("DX12"、"Vulkan" 等)。
     */
    virtual const char* BackendName() const noexcept = 0;

    /**
     * GPU 名を返す。
     *
     * @return GPU 名 ("NVIDIA RTX 4090" など、デバッグ表示用)。
     */
    virtual const char* AdapterName() const noexcept = 0;

    /** GPU の処理が完了するまで待つ (Shutdown 前などに必要)。 */
    virtual void WaitIdle() noexcept = 0;

    // 注: 以降に virtual を追加する場合は «末尾追加» すること (vtable スロット安定化)。

    /**
     * レンダーターゲットテクスチャの内容を CPU メモリへ読み戻す (同期、GPU→CPU)。
     *
     * @details
     * texture は描画済み (呼び出し側が render + WaitIdle 済み) であること。destination_pixels に
     * mip 0 / array slice 0 を行優先で詰める。3D texture は depth slice 0 の width*height texel
     * だけを返す。非圧縮 EFormat の bytes-per-pixel を使い、GPU row pitch は除去する。
     * サムネイル/スクリーンショット/検証用の同期操作で、内部で readback resource + copy +
     * fence wait を行う (遅い)。
     * 既定は未対応 (false)。DX12 バックエンドが実装する。
     * @param texture 読み戻し元のテクスチャ (render target)。
     * @param destination_pixels 書き込み先 (>= width*height*format_bytes バイト)。
     * @param destination_size destination_pixels のバイト数。
     * @return 成功なら true、未対応/失敗なら false。
     */
    virtual bool ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept
    {
        (void)texture;
        (void)destination_pixels;
        (void)destination_size;
        return false;
    }

    /**
     * Return whether the backend can compile shaders asynchronously while the
     * render-owner thread polls their status.
     */
    virtual bool SupportsAsyncShaderCompilation() const noexcept
    {
        return false;
    }

    /**
     * Return whether the backend can accept more rendering work.
     *
     * Backends override this for device-removal/loss detection. The default
     * preserves compatibility for implementations without an explicit health
     * query.
     */
    virtual bool IsOperational() const noexcept
    {
        return true;
    }
};

/**
 * バックエンド選択。
 *
 * @details Diligent 経由のときのみ意味を持つ (Dx12 raw backend は無視する)。
 */
enum class ERhiBackendKind : u8 {
    /** 利用可能な中で最良を選ぶ (Diligent: D3D12 を優先)。 */
    Auto = 0,

    /** 強制的に DX12 を使う。 */
    D3D12 = 1,

    /** 強制的に Vulkan を使う (要 ACS_DILIGENT_VULKAN=ON)。 */
    Vulkan = 2,
};

/**
 * デバイス作成オプション。
 *
 * @details デバッグレイヤ有効化・GPU 選好・バックエンド選択を指定する。
 */
struct FDeviceConfig {
    /** デバッグレイヤを有効化するか (Debug ビルドのみ ON 推奨)。 */
    bool enable_debug_layer = false;

    /** 統合 GPU よりディスクリート GPU を優先するか。 */
    bool prefer_high_perf = true;

    /** 使用するバックエンドの種類。 */
    ERhiBackendKind backend = ERhiBackendKind::Auto;
};

/**
 * デバイスを作成する。
 *
     * @details バックエンドはビルド設定と configuration.backend で決まる。
     * @param configuration デバイス作成オプション。
 * @return 成功なら所有権付きデバイス、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const FDeviceConfig& configuration) noexcept;

/**
 * バックエンドのプロセス寿命シングルトンを先に生成しておく。
 *
 * @details
     * Diligent の IEngineFactoryD3D12 のような「初回使用時に CRT ヒープへ遅延構築され、
 * プロセス終了の static デストラクタまで生きる」シングルトンを、CRT デバッグヒープの
 * リーク計測スコープ (CApplication スコープ) を開く前に確定させるためのフック。
 * これを呼ばないと、計測スコープ内で構築されたシングルトンがスコープ終了時の
 * ダンプに残留ブロックとして現れ、実リークが無いのに leak_detected=true になる。
 * デバイス生成は行わない。何度呼んでも安全 (2 回目以降は no-op)。
 */
void PrewarmRhiProcessSingletons() noexcept;

} // namespace acs
