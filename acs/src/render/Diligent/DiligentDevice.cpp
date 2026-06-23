// SPDX-License-Identifier: Apache-2.0
// DiligentDevice 実装（D3D12 / Vulkan バックエンド両対応）
#include "render/Diligent/DiligentDevice.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentMemoryAdapter.h"
#include "foundation/Log.h"
#include "foundation/Platform.h"
#include "memory/Memory.h"
#include "memory/MemorySystem.h"

#include <cstring>

namespace acs {

DiligentDevice::~DiligentDevice() noexcept {
    if (m_IdleFence) { m_IdleFence->Release(); m_IdleFence = nullptr; }
    if (m_Context)    { m_Context->Flush(); m_Context->Release(); m_Context = nullptr; }
    if (m_Device)     { m_Device->Release();  m_Device  = nullptr; }
    if (m_Factory)    { m_Factory->Release(); m_Factory = nullptr; }
#if WITH_RENDER_DILIGENT_VULKAN
    if (m_FactoryVk) { m_FactoryVk->Release(); m_FactoryVk = nullptr; }
#endif
    m_FactoryGeneric = nullptr;
}

TResult<void> DiligentDevice::Init(const DeviceConfig& cfg) noexcept {
    // バックエンドの選択
    ERhiBackendKind kind = cfg.backend;
    if (kind == ERhiBackendKind::Auto) kind = ERhiBackendKind::D3D12;

#if WITH_RENDER_DILIGENT_VULKAN
    if (kind == ERhiBackendKind::Vulkan) {
        return InitVulkan(cfg);
    }
#else
    if (kind == ERhiBackendKind::Vulkan) {
        ACS_LOG_ERROR("Diligent: Vulkan backend requested but WITH_RENDER_DILIGENT_VULKAN=0");
        return ACS_ERR(Render, 99, "Vulkan backend not built");
    }
#endif
    return InitD3D12(cfg);
}

TResult<void> DiligentDevice::InitD3D12(const DeviceConfig& cfg) noexcept {
    // 静的リンク (Diligent-GraphicsEngineD3D12-static) では LoadGraphicsEngineD3D12
    // (DLL ロード経由) は宣言されない。GetEngineFactoryD3D12 を直接呼ぶ。
    m_Factory = Diligent::GetEngineFactoryD3D12();
    if (!m_Factory) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryD3D12 returned null");
        return ACS_ERR(Render, 101, "GetEngineFactoryD3D12 failed");
    }
    // 新版 Diligent は d3d12.dll の明示ロードが必要。EnumerateAdapters の前に呼ぶ
    // (CreateDeviceAndContextsD3D12 は内部で auto-load するが、EnumerateAdapters は手動)
    if (!m_Factory->LoadD3D12()) {
        ACS_LOG_ERROR("Diligent: LoadD3D12 failed (d3d12.dll not found?)");
        return ACS_ERR(Render, 104, "LoadD3D12 failed");
    }
    m_Factory->AddRef();
    m_FactoryGeneric = m_Factory;
    m_ActualBackend  = ERhiBackendKind::D3D12;
    m_BackendName    = "Diligent-D3D12";

    Diligent::EngineD3D12CreateInfo eci{};
    eci.GraphicsAPIVersion = {12, 0};
    eci.EnableValidation = cfg.enable_debug_layer;
    eci.NumDeferredContexts = 0;

    // TODO: カスタムアロケータ (DiligentMemoryAdapter→TLSF) を注入すると
    // 初期化中に TLSF 内部で access violation。Diligent の allocation pattern
    // (高頻度 alloc/free + 多サイズ) に TLSF 側の thread-safety か境界処理が
    // 追いついていない疑い。当面は Diligent の default allocator (malloc/free
    // ベース) に任せる。性能計測したくなったら FAllocator 側を直してから戻す。
    // 旧コード:
    // if (auto* mem_seg = FMemorySystem::Get(ESegment::Resource)) {
    //     eci.pRawMemAllocator = static_cast<Diligent::IMemoryAllocator*>(
    //         DiligentMemoryAdapter::Create(mem_seg));
    // }

    Diligent::Uint32 num_adapters = 0;
    m_Factory->EnumerateAdapters(eci.GraphicsAPIVersion, num_adapters, nullptr);
    if (num_adapters == 0) {
        ACS_LOG_ERROR("Diligent: No D3D12 adapter found");
        return ACS_ERR(Render, 102, "No D3D12 adapter");
    }
    constexpr Diligent::Uint32 kMaxAdapters = 8;
    Diligent::GraphicsAdapterInfo adapters[kMaxAdapters]{};
    Diligent::Uint32 enumerate = num_adapters > kMaxAdapters ? kMaxAdapters : num_adapters;  // 非 const: EnumerateAdapters が in/out 参照で個数を受ける
    m_Factory->EnumerateAdapters(eci.GraphicsAPIVersion, enumerate, adapters);
    Diligent::Uint32 selected = 0;
    if (cfg.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) { selected = i; break; }
        }
    }
    eci.AdapterId = selected;
    std::strncpy(m_AdapterName, adapters[selected].Description, sizeof(m_AdapterName) - 1);
    m_AdapterName[sizeof(m_AdapterName) - 1] = 0;

    m_Factory->CreateDeviceAndContextsD3D12(eci, &m_Device, &m_Context);
    if (!m_Device || !m_Context) {
        ACS_LOG_ERROR("Diligent: CreateDeviceAndContextsD3D12 failed");
        return ACS_ERR(Render, 103, "CreateDeviceAndContextsD3D12 failed");
    }

    Diligent::FenceDesc fd;
    fd.Name = "ACS_DiligentDevice_IdleFence";
    fd.Type = Diligent::FENCE_TYPE_GENERAL;
    m_Device->CreateFence(fd, &m_IdleFence);

    ACS_LOG_INFO("Diligent D3D12 device created: %s", m_AdapterName);
    return Ok();
}

TResult<void> DiligentDevice::InitVulkan(const DeviceConfig& cfg) noexcept {
#if WITH_RENDER_DILIGENT_VULKAN
    m_FactoryVk = Diligent::GetEngineFactoryVk();
    if (!m_FactoryVk) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryVk returned null");
        return ACS_ERR(Render, 111, "GetEngineFactoryVk failed");
    }
    m_FactoryVk->AddRef();
    m_FactoryGeneric = m_FactoryVk;
    m_ActualBackend  = ERhiBackendKind::Vulkan;
    m_BackendName    = "Diligent-Vulkan";

    Diligent::EngineVkCreateInfo eci{};
    if (cfg.enable_debug_layer) {
        eci.EnableValidation = true;
    }
    eci.NumDeferredContexts = 0;

    if (auto* mem_seg = FMemorySystem::Get(ESegment::Resource)) {
        eci.pRawMemAllocator = static_cast<Diligent::IMemoryAllocator*>(
            DiligentMemoryAdapter::Create(mem_seg));
    }

    Diligent::Uint32 num_adapters = 0;
    m_FactoryVk->EnumerateAdapters({}, num_adapters, nullptr);
    if (num_adapters == 0) {
        ACS_LOG_ERROR("Diligent: No Vulkan adapter found");
        return ACS_ERR(Render, 112, "No Vulkan adapter");
    }
    constexpr Diligent::Uint32 kMaxAdapters = 8;
    Diligent::GraphicsAdapterInfo adapters[kMaxAdapters]{};
    const Diligent::Uint32 enumerate = num_adapters > kMaxAdapters ? kMaxAdapters : num_adapters;
    m_FactoryVk->EnumerateAdapters({}, enumerate, adapters);
    Diligent::Uint32 selected = 0;
    if (cfg.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) { selected = i; break; }
        }
    }
    eci.AdapterId = selected;
    std::strncpy(m_AdapterName, adapters[selected].Description, sizeof(m_AdapterName) - 1);
    m_AdapterName[sizeof(m_AdapterName) - 1] = 0;

    m_FactoryVk->CreateDeviceAndContextsVk(eci, &m_Device, &m_Context);
    if (!m_Device || !m_Context) {
        ACS_LOG_ERROR("Diligent: CreateDeviceAndContextsVk failed");
        return ACS_ERR(Render, 113, "CreateDeviceAndContextsVk failed");
    }

    Diligent::FenceDesc fd;
    fd.Name = "ACS_DiligentDevice_IdleFence";
    fd.Type = Diligent::FENCE_TYPE_GENERAL;
    m_Device->CreateFence(fd, &m_IdleFence);

    ACS_LOG_INFO("Diligent Vulkan device created: %s", m_AdapterName);
    return Ok();
#else
    (void)cfg;
    return ACS_ERR(Render, 114, "Vulkan backend not built (WITH_RENDER_DILIGENT_VULKAN=0)");
#endif
}

void DiligentDevice::WaitIdle() noexcept {
    if (!m_Context) return;
    if (m_IdleFence) {
        ++m_IdleValue;
        m_Context->EnqueueSignal(m_IdleFence, m_IdleValue);
        m_Context->WaitForIdle();
        m_IdleFence->Wait(m_IdleValue);
    } else {
        m_Context->Flush();
        m_Context->WaitForIdle();
    }
}

u64 DiligentDevice::SignalGraphicsQueue() noexcept {
    if (!m_Context || !m_IdleFence) return 0;
    ++m_IdleValue;
    m_Context->EnqueueSignal(m_IdleFence, m_IdleValue);
    return m_IdleValue;
}

void DiligentDevice::WaitForFenceValue(u64 v) noexcept {
    if (!m_IdleFence) return;
    if (m_IdleFence->GetCompletedValue() >= v) return;
    m_IdleFence->Wait(v);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
