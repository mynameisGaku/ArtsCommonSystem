// Diligent Engine 経由の RHI デバイス実装
// IRhiDevice を継承し、内部で IRenderDevice / IDeviceContext / EngineFactory を保持。
//
// 設計方針:
//   - Diligent の重い型 (RefCntAutoPtr) はヘッダに漏らさず、.cpp で扱う
//   - ヘッダはバックエンド非依存で完結。ユーザーコードはこのヘッダを include しなくて良い
//     （CreateRhiDevice() 経由で IRhiDevice ハンドルだけ受け取る）
#pragma once

#include "render/IRhiDevice.h"
#include "memory/UniquePtr.h"

// 前方宣言（DiligentCommon.h を漏らさない）
namespace Diligent {
    struct IEngineFactoryD3D12;
    struct IRenderDevice;
    struct IDeviceContext;
    struct IFence;
}

namespace acs {

class DiligentDevice final : public IRhiDevice {
public:
    DiligentDevice() noexcept = default;
    ~DiligentDevice() noexcept override;

    DiligentDevice(const DiligentDevice&) = delete;
    DiligentDevice& operator=(const DiligentDevice&) = delete;

    // 初期化（CreateRhiDevice から呼ばれる）
    Result<void> Init(const DeviceConfig& cfg) noexcept;

    // ---- IRhiDevice ----
    const char* BackendName() const noexcept override { return "Diligent-DX12"; }
    const char* AdapterName() const noexcept override { return _adapter_name; }
    void        WaitIdle()    noexcept override;

    // ---- 内部公開（他の Diligent* バックエンドが触れる）----
    Diligent::IEngineFactoryD3D12* Factory()   const noexcept { return _factory; }
    Diligent::IRenderDevice*       RenderDev() const noexcept { return _device; }
    Diligent::IDeviceContext*      Context()   const noexcept { return _context; }

    // フレーム同期: ExecuteCommandLists 後に Signal して値を返す。
    // CommandList::Submit がこれを呼ぶ想定。
    u64  SignalGraphicsQueue() noexcept;
    void WaitForFenceValue(u64 v) noexcept;

    // フレームスロット
    static constexpr u32 kFramesInFlight = 2;
    u32  CurrentFrameSlot() const noexcept { return _frame_slot; }
    void AdvanceFrameSlot() noexcept { _frame_slot = (_frame_slot + 1) % kFramesInFlight; }

private:
    // Diligent オブジェクトは生ポインタで保持し、Release() を明示的に呼んで破棄する。
    // RefCntAutoPtr を使わずに済ませることでヘッダから Diligent 依存を切り離す。
    Diligent::IEngineFactoryD3D12* _factory   = nullptr;
    Diligent::IRenderDevice*       _device    = nullptr;
    Diligent::IDeviceContext*      _context   = nullptr;
    Diligent::IFence*              _idle_fence = nullptr;
    u64                            _idle_value = 0;
    u32                            _frame_slot = 0;
    char                           _adapter_name[128]{};
};

} // namespace acs
