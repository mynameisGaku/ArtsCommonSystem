// SPDX-License-Identifier: Apache-2.0
// DiligentDevice 実装（D3D12 / Vulkan バックエンド両対応）
#include "render/Diligent/DiligentDevice.h"

#if WITH_RENDER_DILIGENT

#    include "render/Diligent/DiligentCommon.h"
#    include "render/Diligent/DiligentTexture.h" // ReadTexture: Native()/cast
#    include "render/Diligent/DiligentMemoryAdapter.h"
#    include "foundation/Log.h"
#    include "foundation/Platform.h"
#    include "memory/Memory.h"
#    include "memory/MemorySystem.h"

#    include <cstring>

#    if ACS_BUILD_DEBUG
#        include "render/Dx12/Dx12LiveObjectDiagnosticsInternal.h"
#        include "RenderDeviceD3D12.h"
#    endif

namespace acs {

namespace {

#    if ACS_BUILD_DEBUG
/** Diligent の D3D12 デバイスから終了後レポート用 Debug Device を取得する。 */
render_internal::D3D12DebugDeviceReportHandle CaptureDiligentDebugDevice(Diligent::IRenderDevice* device) noexcept
{
    if (!device) return {};
    Diligent::IRenderDeviceD3D12* diligent_device = nullptr;
    device->QueryInterface(Diligent::IID_RenderDeviceD3D12, reinterpret_cast<Diligent::IObject**>(&diligent_device));
    if (!diligent_device) return {};

    render_internal::D3D12DebugDeviceReportHandle debug_device{};
    ID3D12Device* const native_device = diligent_device->GetD3D12Device();
    debug_device = render_internal::CaptureD3D12DebugDeviceReportHandle(native_device);
    diligent_device->Release();
    return debug_device;
}

#    endif

} // namespace

DiligentDevice::~DiligentDevice() noexcept
{
    // 直接利用時も、所有 context が投入済みの GPU 処理を完了してから解放する。
    if (m_Context) WaitIdle();
    Reset();
}

void DiligentDevice::Reset() noexcept
{
#    if ACS_BUILD_DEBUG
    // D3D12 Factory の取得後に失敗した場合も DXGI 診断を残す。
    const bool report_d3d12_live_objects = m_ActualBackend == ERhiBackendKind::D3D12 && (m_Device || m_Factory);
    render_internal::D3D12DebugDeviceReportHandle debug_device = m_Device && report_d3d12_live_objects
                                                                     ? CaptureDiligentDebugDevice(m_Device)
                                                                     : render_internal::D3D12DebugDeviceReportHandle{};
#    endif
    if (m_IdleFence) {
        m_IdleFence->Release();
        m_IdleFence = nullptr;
    }
    if (m_Context) {
        m_Context->Flush();
        m_Context->Release();
        m_Context = nullptr;
    }
    if (m_Device) {
        m_Device->Release();
        m_Device = nullptr;
    }
    if (m_Factory) {
        m_Factory->Release();
        m_Factory = nullptr;
    }
#    if WITH_RENDER_DILIGENT_VULKAN
    if (m_FactoryVk) {
        m_FactoryVk->Release();
        m_FactoryVk = nullptr;
    }
#    endif
    const u64 binding_generation = DiligentMemoryAdapter::BindingGeneration();
    const u64 backing_lifetime_generation = DiligentMemoryAdapter::BackingLifetimeGeneration();
    const u64 outstanding_allocation_count = DiligentMemoryAdapter::OutstandingAllocationCount();
    const u64 outstanding_requested_bytes = DiligentMemoryAdapter::OutstandingRequestedBytes();
    if (outstanding_allocation_count != 0 || outstanding_requested_bytes != 0) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=shutdown device_released=true "
                      "leak_detected=true status=failed binding_generation=%llu backing_lifetime_generation=%llu "
                      "outstanding_allocations=%llu outstanding_requested_bytes=%llu",
                      static_cast<unsigned long long>(binding_generation),
                      static_cast<unsigned long long>(backing_lifetime_generation),
                      static_cast<unsigned long long>(outstanding_allocation_count),
                      static_cast<unsigned long long>(outstanding_requested_bytes));
    } else {
        ACS_LOG_INFO("[acs][memory] tracker=diligent_memory_adapter record=shutdown device_released=true "
                     "leak_detected=false status=ok binding_generation=%llu backing_lifetime_generation=%llu "
                     "outstanding_allocations=0 outstanding_requested_bytes=0",
                     static_cast<unsigned long long>(binding_generation),
                     static_cast<unsigned long long>(backing_lifetime_generation));
    }
#    if ACS_BUILD_DEBUG
    if (report_d3d12_live_objects) {
        render_internal::ReportD3D12LiveObjects(debug_device, "Diligent-D3D12");
        debug_device.Release();
        render_internal::ReportDxgiLiveObjects("Diligent-D3D12");
    }
#    endif
    m_FactoryGeneric = nullptr;
    m_IdleValue = 0;
    m_FrameSlot = 0;
    m_ActualBackend = ERhiBackendKind::Auto;
    m_AdapterName[0] = '\0';
    m_BackendName = "Diligent";
}

TResult<void> DiligentDevice::Init(const DeviceConfig& configuration) noexcept
{
    // 二重 Init では前回の GPU 処理を完了させてから所有物を解放する。
    if (m_Context) WaitIdle();
    // 失敗後の再試行も同じ空状態から開始する。
    Reset();

    // バックエンドの選択
    ERhiBackendKind kind = configuration.backend;
    if (kind == ERhiBackendKind::Auto) kind = ERhiBackendKind::D3D12;

#    if WITH_RENDER_DILIGENT_VULKAN
    if (kind == ERhiBackendKind::Vulkan) {
        auto result = InitVulkan(configuration);
        if (result.IsErr()) Reset();
        return result;
    }
#    else
    if (kind == ERhiBackendKind::Vulkan) {
        ACS_LOG_ERROR("Diligent: Vulkan backend requested but WITH_RENDER_DILIGENT_VULKAN=0");
        return ACS_ERR(Render, 99, "Vulkan backend not built");
    }
#    endif
    auto result = InitD3D12(configuration);
    if (result.IsErr()) Reset();
    return result;
}

TResult<void> DiligentDevice::InitD3D12(const DeviceConfig& configuration) noexcept
{
    FAllocator* const memory_segment = FMemorySystem::Get(ESegment::Resource);
    if (!memory_segment) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=initialization "
                      "status=failed reason=memory_system_unavailable backend=Diligent-D3D12");
        return ACS_ERR(Render, 105, "Diligent requires an initialized MemorySystem Resource segment");
    }
    auto* const diligent_memory_allocator = static_cast<Diligent::IMemoryAllocator*>(
        DiligentMemoryAdapter::Create(memory_segment));
    if (!diligent_memory_allocator) {
        return ACS_ERR(Render, 106, "Diligent memory adapter rejected the allocator lifetime");
    }

    // 静的リンク (Diligent-GraphicsEngineD3D12-static) では LoadGraphicsEngineD3D12
    // (DLL ロード経由) は宣言されない。GetEngineFactoryD3D12 を直接呼ぶ。
    // GetEngineFactoryD3D12() の戻り値は借用参照。Load 成功までは所有メンバーへ入れない。
    Diligent::IEngineFactoryD3D12* const factory = Diligent::GetEngineFactoryD3D12();
    if (!factory) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryD3D12 returned null");
        return ACS_ERR(Render, 101, "GetEngineFactoryD3D12 failed");
    }
    // 新版 Diligent は d3d12.dll の明示ロードが必要。EnumerateAdapters の前に呼ぶ
    // (CreateDeviceAndContextsD3D12 は内部で auto-load するが、EnumerateAdapters は手動)
    if (!factory->LoadD3D12()) {
        ACS_LOG_ERROR("Diligent: LoadD3D12 failed (d3d12.dll not found?)");
        return ACS_ERR(Render, 104, "LoadD3D12 failed");
    }
    factory->AddRef();
    m_Factory = factory;
    m_FactoryGeneric = m_Factory;
    m_ActualBackend = ERhiBackendKind::D3D12;
    m_BackendName = "Diligent-D3D12";

    Diligent::EngineD3D12CreateInfo eci{};
    eci.GraphicsAPIVersion = {12, 0};
    eci.EnableValidation = configuration.enable_debug_layer;
    eci.NumDeferredContexts = 0;

    // Diligent の全内部確保を Resource セグメントの mimalloc heap へ集約する。
    eci.pRawMemAllocator = diligent_memory_allocator;

    Diligent::Uint32 num_adapters = 0;
    m_Factory->EnumerateAdapters(eci.GraphicsAPIVersion, num_adapters, nullptr);
    if (num_adapters == 0) {
        ACS_LOG_ERROR("Diligent: No D3D12 adapter found");
        return ACS_ERR(Render, 102, "No D3D12 adapter");
    }
    constexpr Diligent::Uint32 kMaxAdapters = 8;
    Diligent::GraphicsAdapterInfo adapters[kMaxAdapters]{};
    Diligent::Uint32 enumerate = num_adapters > kMaxAdapters
                                     ? kMaxAdapters
                                     : num_adapters; // 非 const: EnumerateAdapters が in/out 参照で個数を受ける
    m_Factory->EnumerateAdapters(eci.GraphicsAPIVersion, enumerate, adapters);
    Diligent::Uint32 selected = 0;
    if (configuration.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) {
                selected = i;
                break;
            }
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

TResult<void> DiligentDevice::InitVulkan(const DeviceConfig& configuration) noexcept
{
#    if WITH_RENDER_DILIGENT_VULKAN
    FAllocator* const memory_segment = FMemorySystem::Get(ESegment::Resource);
    if (!memory_segment) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=initialization "
                      "status=failed reason=memory_system_unavailable backend=Diligent-Vulkan");
        return ACS_ERR(Render, 115, "Diligent requires an initialized MemorySystem Resource segment");
    }
    auto* const diligent_memory_allocator = static_cast<Diligent::IMemoryAllocator*>(
        DiligentMemoryAdapter::Create(memory_segment));
    if (!diligent_memory_allocator) {
        return ACS_ERR(Render, 116, "Diligent memory adapter rejected the allocator lifetime");
    }

    m_FactoryVk = Diligent::GetEngineFactoryVk();
    if (!m_FactoryVk) {
        ACS_LOG_ERROR("Diligent: GetEngineFactoryVk returned null");
        return ACS_ERR(Render, 111, "GetEngineFactoryVk failed");
    }
    m_FactoryVk->AddRef();
    m_FactoryGeneric = m_FactoryVk;
    m_ActualBackend = ERhiBackendKind::Vulkan;
    m_BackendName = "Diligent-Vulkan";

    Diligent::EngineVkCreateInfo eci{};
    if (configuration.enable_debug_layer) {
        eci.EnableValidation = true;
    }
    eci.NumDeferredContexts = 0;

    eci.pRawMemAllocator = diligent_memory_allocator;

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
    if (configuration.prefer_high_perf) {
        for (Diligent::Uint32 i = 0; i < enumerate; ++i) {
            if (adapters[i].Type == Diligent::ADAPTER_TYPE_DISCRETE) {
                selected = i;
                break;
            }
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
#    else
    (void)configuration;
    return ACS_ERR(Render, 114, "Vulkan backend not built (WITH_RENDER_DILIGENT_VULKAN=0)");
#    endif
}

void DiligentDevice::WaitIdle() noexcept
{
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

bool DiligentDevice::ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept
{
    if (!m_Device || !m_Context || destination_pixels == nullptr) return false;
    auto* dtex = static_cast<DiligentTexture*>(&texture);
    Diligent::ITexture* src = dtex->Native();
    if (src == nullptr) return false;
    const Diligent::TextureDesc& sd = src->GetDesc();
    const u32 w = sd.Width, h = sd.Height;
    u32 bpp = 4;
    switch (dtex->EPixelFormat()) {
    case EFormat::R32G32B32A32_Float:
        bpp = 16;
        break;
    case EFormat::R16G16B16A16_Float:
        bpp = 8;
        break;
    case EFormat::R32G32_Float:
        bpp = 8;
        break;
    case EFormat::R11G11B10_Float:
        bpp = 4;
        break;
    case EFormat::R16G16_Float:
        bpp = 4;
        break;
    default:
        bpp = 4;
        break; // RGBA8/BGRA8 等
    }
    if (w == 0 || h == 0 || destination_size < w * h * bpp) return false;

    // staging texture (USAGE_STAGING + CPU_ACCESS_READ) を作って CopyTexture → Map で読む。
    Diligent::TextureDesc stg = sd;
    stg.Name = "ReadbackStaging";
    stg.Usage = Diligent::USAGE_STAGING;
    stg.CPUAccessFlags = Diligent::CPU_ACCESS_READ;
    stg.BindFlags = Diligent::BIND_NONE;
    stg.MiscFlags = Diligent::MISC_TEXTURE_FLAG_NONE;
    Diligent::RefCntAutoPtr<Diligent::ITexture> staging;
    m_Device->CreateTexture(stg, nullptr, &staging);
    if (!staging) return false;

    Diligent::CopyTextureAttribs cta;
    cta.pSrcTexture = src;
    cta.SrcTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    cta.pDstTexture = staging;
    cta.DstTextureTransitionMode = Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION;
    m_Context->CopyTexture(cta);
    m_Context->Flush();
    WaitIdle();

    Diligent::MappedTextureSubresource mapped;
    m_Context->MapTextureSubresource(staging, 0, 0, Diligent::MAP_READ, Diligent::MAP_FLAG_DO_NOT_WAIT, nullptr,
                                     mapped);
    if (mapped.pData == nullptr) return false;
    auto* dst = static_cast<u8*>(destination_pixels);
    const auto* srcp = static_cast<const u8*>(mapped.pData);
    for (u32 y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<usize>(y) * w * bpp, srcp + static_cast<usize>(y) * mapped.Stride, w * bpp);
    }
    m_Context->UnmapTextureSubresource(staging, 0, 0);
    return true;
}

u64 DiligentDevice::SignalGraphicsQueue() noexcept
{
    if (!m_Context || !m_IdleFence) return 0;
    ++m_IdleValue;
    m_Context->EnqueueSignal(m_IdleFence, m_IdleValue);
    return m_IdleValue;
}

void DiligentDevice::WaitForFenceValue(u64 fence_value) noexcept
{
    if (!m_IdleFence) return;
    if (m_IdleFence->GetCompletedValue() >= fence_value) return;
    m_IdleFence->Wait(fence_value);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
