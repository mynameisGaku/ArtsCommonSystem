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
    if (_idle_fence) { _idle_fence->Release(); _idle_fence = nullptr; }
    if (_context)    { _context->Flush(); _context->Release(); _context = nullptr; }
    if (_device)     { _device->Release();  _device  = nullptr; }
    if (_factory)    { _factory->Release(); _factory = nullptr; }
#if WITH_RENDER_DILIGENT_VULKAN
    if (_factory_vk) { _factory_vk->Release(); _factory_vk = nullptr; }
#endif
    _factory_generic = nullptr;
}

Result<void> DiligentDevice::Init(const DeviceConfig& cfg) noexcept {
    // バックエンドの選択
    RhiBackendKind kind = cfg.backend;
    if (kind == RhiBackendKind::Auto) kind = RhiBackendKind::D3D12;

#if WITH_RENDER_DILIGENT_VULKAN
    if (kind == RhiBackendKind::Vulkan) {
        return InitVulkan(cfg);
    }
#else
    if (kind == RhiBackendKind::Vulkan) {
        ACS_LOG_ERROR("Diligent: Vulkan backend requested but WITH_RENDER_DILIGENT_VULKAN=0");
        return ACS_ERR(Render, 99, "Vulkan backend not built");
    }
#endif
    return InitD3D12(cfg);
}

Result<void> DiligentDevice::InitD3D12(const DeviceConfig& cfg) noexcept {
    auto* GetFactory = Diligent::LoadGraphicsEngineD3D12();
    if (!GetFactory) {
        ACS_LOG_ERROR("Diligent: LoadGraphicsEngineD3D12 returned null");
        return ACS_ERR(Render, 100, "LoadGraphicsEngineD3D12 failed");
    }
    _factory = GetFactory();
    if (!_factory) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryD3D12 returned null");
        return ACS_ERR(Render, 101, "GetEngineFactoryD3D12 failed");
    }
    _factory->AddRef();
    _factory_generic = _factory;
    _actual_backend  = RhiBackendKind::D3D12;
    _backend_name    = "Diligent-D3D12";

    Diligent::EngineD3D12CreateInfo eci{};
    eci.GraphicsAPIVersion = {12, 0};
    if (cfg.enable_debug_layer) {
        eci.EnableValidation = true;
    }
    eci.NumDeferredContexts = 0;

    if (auto* mem_seg = MemorySystem::Get(Segment::Resource)) {
        eci.pRawMemAllocator = static_cast<Diligent::IMemoryAllocator*>(
            DiligentMemoryAdapter::Create(mem_seg));
    }

    Diligent::Uint32 num_adapters = 0;
    _factory->EnumerateAdapters(eci.GraphicsAPIVersion, num_adapters, nullptr);
    if (num_adapters == 0) {
        ACS_LOG_ERROR("Diligent: No D3D12 adapter found");
        return ACS_ERR(Render, 102, "No D3D12 adapter");
    }
    constexpr Diligent::Uint32 kMaxAdapters = 8;
    Diligent::GraphicsAdapterInfo adapters[kMaxAdapters]{};
    Diligent::Uint32 enumerate = num_adapters > kMaxAdapters ? kMaxAdapters : num_adapters;
    _factory->EnumerateAdapters(eci.GraphicsAPIVersion, enumerate, adapters);
    Diligent::Uint32 selected = 0;
    if (cfg.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) { selected = i; break; }
        }
    }
    eci.AdapterId = selected;
    std::strncpy(_adapter_name, adapters[selected].Description, sizeof(_adapter_name) - 1);
    _adapter_name[sizeof(_adapter_name) - 1] = 0;

    _factory->CreateDeviceAndContextsD3D12(eci, &_device, &_context);
    if (!_device || !_context) {
        ACS_LOG_ERROR("Diligent: CreateDeviceAndContextsD3D12 failed");
        return ACS_ERR(Render, 103, "CreateDeviceAndContextsD3D12 failed");
    }

    Diligent::FenceDesc fd;
    fd.Name = "ACS_DiligentDevice_IdleFence";
    fd.Type = Diligent::FENCE_TYPE_GENERAL;
    _device->CreateFence(fd, &_idle_fence);

    ACS_LOG_INFO("Diligent D3D12 device created: %s", _adapter_name);
    return Ok();
}

Result<void> DiligentDevice::InitVulkan(const DeviceConfig& cfg) noexcept {
#if WITH_RENDER_DILIGENT_VULKAN
    auto* GetFactory = Diligent::LoadGraphicsEngineVk();
    if (!GetFactory) {
        ACS_LOG_ERROR("Diligent: LoadGraphicsEngineVk returned null");
        return ACS_ERR(Render, 110, "LoadGraphicsEngineVk failed");
    }
    _factory_vk = GetFactory();
    if (!_factory_vk) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryVk returned null");
        return ACS_ERR(Render, 111, "GetEngineFactoryVk failed");
    }
    _factory_vk->AddRef();
    _factory_generic = _factory_vk;
    _actual_backend  = RhiBackendKind::Vulkan;
    _backend_name    = "Diligent-Vulkan";

    Diligent::EngineVkCreateInfo eci{};
    if (cfg.enable_debug_layer) {
        eci.EnableValidation = true;
    }
    eci.NumDeferredContexts = 0;

    if (auto* mem_seg = MemorySystem::Get(Segment::Resource)) {
        eci.pRawMemAllocator = static_cast<Diligent::IMemoryAllocator*>(
            DiligentMemoryAdapter::Create(mem_seg));
    }

    Diligent::Uint32 num_adapters = 0;
    _factory_vk->EnumerateAdapters({}, num_adapters, nullptr);
    if (num_adapters == 0) {
        ACS_LOG_ERROR("Diligent: No Vulkan adapter found");
        return ACS_ERR(Render, 112, "No Vulkan adapter");
    }
    constexpr Diligent::Uint32 kMaxAdapters = 8;
    Diligent::GraphicsAdapterInfo adapters[kMaxAdapters]{};
    Diligent::Uint32 enumerate = num_adapters > kMaxAdapters ? kMaxAdapters : num_adapters;
    _factory_vk->EnumerateAdapters({}, enumerate, adapters);
    Diligent::Uint32 selected = 0;
    if (cfg.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) { selected = i; break; }
        }
    }
    eci.AdapterId = selected;
    std::strncpy(_adapter_name, adapters[selected].Description, sizeof(_adapter_name) - 1);
    _adapter_name[sizeof(_adapter_name) - 1] = 0;

    _factory_vk->CreateDeviceAndContextsVk(eci, &_device, &_context);
    if (!_device || !_context) {
        ACS_LOG_ERROR("Diligent: CreateDeviceAndContextsVk failed");
        return ACS_ERR(Render, 113, "CreateDeviceAndContextsVk failed");
    }

    Diligent::FenceDesc fd;
    fd.Name = "ACS_DiligentDevice_IdleFence";
    fd.Type = Diligent::FENCE_TYPE_GENERAL;
    _device->CreateFence(fd, &_idle_fence);

    ACS_LOG_INFO("Diligent Vulkan device created: %s", _adapter_name);
    return Ok();
#else
    (void)cfg;
    return ACS_ERR(Render, 114, "Vulkan backend not built (WITH_RENDER_DILIGENT_VULKAN=0)");
#endif
}

void DiligentDevice::WaitIdle() noexcept {
    if (!_context) return;
    if (_idle_fence) {
        ++_idle_value;
        _context->EnqueueSignal(_idle_fence, _idle_value);
        _context->WaitForIdle();
        _idle_fence->Wait(_idle_value);
    } else {
        _context->Flush();
        _context->WaitForIdle();
    }
}

u64 DiligentDevice::SignalGraphicsQueue() noexcept {
    if (!_context || !_idle_fence) return 0;
    ++_idle_value;
    _context->EnqueueSignal(_idle_fence, _idle_value);
    return _idle_value;
}

void DiligentDevice::WaitForFenceValue(u64 v) noexcept {
    if (!_idle_fence) return;
    if (_idle_fence->GetCompletedValue() >= v) return;
    _idle_fence->Wait(v);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
