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

// グラフィックスデバイスの抽象インターフェイス
class IRhiDevice {
public:
    virtual ~IRhiDevice() noexcept = default;

    // バックエンド名（"DX12", "Vulkan" 等）
    virtual const char* BackendName() const noexcept = 0;

    // GPU 名（"NVIDIA RTX 4090" など、デバッグ表示用）
    virtual const char* AdapterName() const noexcept = 0;

    // GPU の処理が完了するまで待つ（Shutdown 前などに必要）
    virtual void WaitIdle() noexcept = 0;
};

// バックエンド選択 (Diligent 経由のときのみ意味を持つ。Dx12 raw backend は無視)
enum class ERhiBackendKind : u8 {
    Auto    = 0,   // 利用可能な中で最良 (Diligent: D3D12 を優先)
    D3D12   = 1,   // 強制的に DX12 を使う
    Vulkan  = 2,   // 強制的に Vulkan を使う (要 ACS_DILIGENT_VULKAN=ON)
};

// デバイス作成オプション
struct FDeviceConfig {
    bool           enable_debug_layer = false;            // Debug ビルドのみ ON 推奨
    bool           prefer_high_perf   = true;             // 統合 GPU よりディスクリート GPU を優先
    ERhiBackendKind backend            = ERhiBackendKind::Auto;
};

// デバイスを作成する（バックエンドはビルド設定で決まる）
TResult<TUniquePtr<IRhiDevice>> CreateRhiDevice(const FDeviceConfig& cfg) noexcept;

} // namespace acs
