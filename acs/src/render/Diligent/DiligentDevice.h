// SPDX-License-Identifier: Apache-2.0
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
    struct IEngineFactory;
    struct IEngineFactoryD3D12;
    struct IEngineFactoryVk;
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
    TResult<void> Init(const DeviceConfig& cfg) noexcept;

    // ---- IRhiDevice ----
    const char* BackendName() const noexcept override { return m_BackendName; }
    const char* AdapterName() const noexcept override { return m_AdapterName; }
    void        WaitIdle()    noexcept override;

    // 実際に選ばれたバックエンド種別
    ERhiBackendKind ActualBackend() const noexcept { return m_ActualBackend; }

    // ---- 内部公開（他の Diligent* バックエンドが触れる）----
    // Factory は Vulkan / D3D12 で別型。汎用には IEngineFactory を返す。
    Diligent::IEngineFactory*      FactoryGeneric() const noexcept { return m_FactoryGeneric; }
    Diligent::IEngineFactoryD3D12* Factory()        const noexcept { return m_Factory; }
    Diligent::IEngineFactoryVk*    FactoryVk()      const noexcept { return m_FactoryVk; }
    Diligent::IRenderDevice*       RenderDev()      const noexcept { return m_Device; }
    Diligent::IDeviceContext*      Context()        const noexcept { return m_Context; }

    // フレーム同期: ExecuteCommandLists 後に Signal して値を返す。
    // CommandList::Submit がこれを呼ぶ想定。
    u64  SignalGraphicsQueue() noexcept;
    void WaitForFenceValue(u64 v) noexcept;

    // フレームスロット
    static constexpr u32 kFramesInFlight = 2;
    u32  CurrentFrameSlot() const noexcept { return m_FrameSlot; }
    void AdvanceFrameSlot() noexcept { m_FrameSlot = (m_FrameSlot + 1) % kFramesInFlight; }

private:
    TResult<void> InitD3D12(const DeviceConfig& cfg) noexcept;
    TResult<void> InitVulkan(const DeviceConfig& cfg) noexcept;

    // Diligent オブジェクトは生ポインタで保持し、Release() を明示的に呼んで破棄する。
    // RefCntAutoPtr を使わずに済ませることでヘッダから Diligent 依存を切り離す。
    Diligent::IEngineFactory*      m_FactoryGeneric = nullptr;  // m_Factory or m_FactoryVk と同じ実体
    Diligent::IEngineFactoryD3D12* m_Factory       = nullptr;     // D3D12 経路
    Diligent::IEngineFactoryVk*    m_FactoryVk    = nullptr;     // Vulkan 経路
    Diligent::IRenderDevice*       m_Device        = nullptr;
    Diligent::IDeviceContext*      m_Context       = nullptr;
    Diligent::IFence*              m_IdleFence    = nullptr;
    u64                            m_IdleValue    = 0;
    u32                            m_FrameSlot    = 0;
    ERhiBackendKind                 m_ActualBackend = ERhiBackendKind::Auto;
    char                           m_AdapterName[128]{};
    const char*                    m_BackendName  = "Diligent";
};

} // namespace acs
