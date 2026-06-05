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
};

/**
 * バックエンド選択。
 *
 * @details Diligent 経由のときのみ意味を持つ (Dx12 raw backend は無視する)。
 */
enum class ERhiBackendKind : u8 {
    /** 利用可能な中で最良を選ぶ (Diligent: D3D12 を優先)。 */
    Auto    = 0,

    /** 強制的に DX12 を使う。 */
    D3D12   = 1,

    /** 強制的に Vulkan を使う (要 ACS_DILIGENT_VULKAN=ON)。 */
    Vulkan  = 2,
};

/**
 * デバイス作成オプション。
 *
 * @details デバッグレイヤ有効化・GPU 選好・バックエンド選択を指定する。
 */
struct DeviceConfig {
    /** デバッグレイヤを有効化するか (Debug ビルドのみ ON 推奨)。 */
    bool           enable_debug_layer = false;

    /** 統合 GPU よりディスクリート GPU を優先するか。 */
    bool           prefer_high_perf   = true;

    /** 使用するバックエンドの種類。 */
    ERhiBackendKind backend            = ERhiBackendKind::Auto;
};

/**
 * デバイスを作成する。
 *
 * @details バックエンドはビルド設定と cfg.backend で決まる。
 * @param cfg デバイス作成オプション。
 * @return 成功なら所有権付きデバイス、生成失敗ならエラー。
 */
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept;

} // namespace acs
