// DiligentDevice 実装（D3D12 バックエンド経路）
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
}

Result<void> DiligentDevice::Init(const DeviceConfig& cfg) noexcept {
    // EngineFactoryD3D12 を取得
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

    // 作成情報
    Diligent::EngineD3D12CreateInfo eci{};
    eci.GraphicsAPIVersion = {12, 0};
    if (cfg.enable_debug_layer) {
        eci.EnableValidation = true;
    }
    eci.NumDeferredContexts = 0;

    // ACS の Memory モジュールを Diligent の内部アロケータとして使う
    // （MemorySystem::Get(Segment::Resource) → 描画リソース系の確保が ACS 経路を通る）
    if (auto* mem_seg = MemorySystem::Get(Segment::Resource)) {
        eci.pRawMemAllocator = static_cast<Diligent::IMemoryAllocator*>(
            DiligentMemoryAdapter::Create(mem_seg));
    }

    // アダプタ列挙して prefer_high_perf に従って選択
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

    // デバイス + 即時コンテキスト作成
    _factory->CreateDeviceAndContextsD3D12(eci, &_device, &_context);
    if (!_device || !_context) {
        ACS_LOG_ERROR("Diligent: CreateDeviceAndContextsD3D12 failed");
        return ACS_ERR(Render, 103, "CreateDeviceAndContextsD3D12 failed");
    }

    // 待機用 Fence を作る
    Diligent::FenceDesc fd;
    fd.Name = "ACS_DiligentDevice_IdleFence";
    fd.Type = Diligent::FENCE_TYPE_GENERAL;
    _device->CreateFence(fd, &_idle_fence);
    if (!_idle_fence) {
        ACS_LOG_WARN("Diligent: CreateFence for idle returned null (WaitIdle will fall back to Flush)");
    }

    ACS_LOG_INFO("Diligent device created: %s", _adapter_name);
    return Ok();
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
