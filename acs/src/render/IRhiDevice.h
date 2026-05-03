// RHI デバイス抽象（GPU との対話を表す。DX12 / Vulkan で実装される）
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "memory/UniquePtr.h"
#include "render/RhiTypes.h"

namespace acs {

class IRhiSwapchain;
class IRhiCommandList;
class Window;

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

// デバイス作成オプション
struct DeviceConfig {
    bool enable_debug_layer  = false;     // Debug ビルドのみ ON 推奨
    bool prefer_high_perf    = true;       // 統合 GPU よりディスクリート GPU を優先
};

// デバイスを作成する（バックエンドはビルド設定で決まる）
Result<UniquePtr<IRhiDevice>> CreateRhiDevice(const DeviceConfig& cfg) noexcept;

} // namespace acs
