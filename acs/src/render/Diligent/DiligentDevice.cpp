// SPDX-License-Identifier: Apache-2.0
// CDiligentDevice 実装（D3D12 / Vulkan バックエンド両対応）
#include "render/Diligent/DiligentDevice.h"
#include "render/FormatTraits.h"

#if WITH_RENDER_DILIGENT

#    include "render/Diligent/DiligentCommon.h"
#    include "render/Diligent/DiligentTexture.h" // ReadTexture: Native()/cast
#    include "render/Diligent/DiligentMemoryAdapter.h"
#    include "foundation/Log.h"
#    include "foundation/Platform.h"
#    include "memory/Memory.h"
#    include "memory/MemorySystem.h"
#    include <d3d12.h>
#    include "RenderDeviceD3D12.h"

#    include <cstring>
#    include <thread>

#    if ACS_BUILD_DEBUG
#        include "render/Dx12/Dx12LiveObjectDiagnosticsInternal.h"
#    endif

namespace acs {

bool CDiligentDevice::SupportsAsyncShaderCompilation() const noexcept
{
    return m_Device != nullptr &&
           m_Device->GetDeviceInfo().Features.AsyncShaderCompilation ==
               Diligent::DEVICE_FEATURE_STATE_ENABLED;
}

bool CDiligentDevice::IsDeviceHealthy() const noexcept
{
    if (m_Device == nullptr || m_Context == nullptr ||
        m_IdleFence == nullptr) {
        return false;
    }

    if (m_ActualBackend == ERhiBackendKind::D3D12) {
        Diligent::IRenderDeviceD3D12* diligent_device = nullptr;
        m_Device->QueryInterface(
            Diligent::IID_RenderDeviceD3D12,
            reinterpret_cast<Diligent::IObject**>(&diligent_device));
        if (diligent_device == nullptr) return false;
        ID3D12Device* const native_device =
            diligent_device->GetD3D12Device();
        const bool healthy =
            native_device != nullptr &&
            SUCCEEDED(native_device->GetDeviceRemovedReason());
        diligent_device->Release();
        return healthy;
    }

    // Diligent's Vulkan timeline-fence implementation initializes the result
    // to UINT64_MAX and leaves it there when the native status query fails.
    // D3D12 fences use the same sentinel after device removal.
    return m_IdleFence->GetCompletedValue() !=
           static_cast<Diligent::Uint64>(~Diligent::Uint64{0});
}

namespace {

/**
 * ReadTexture が密な CPU 行へ変換できる非圧縮 format の bytes-per-pixel。
 *
 * 未対応 format を暗黙に 4 bytes と見なすと、RGB32F などで行を途中までしか
 * 読めないため 0 を返して fail closed にする。
 */
u32 ReadbackBytesPerPixel(EFormat format) noexcept
{
    return GetFormatTraits(format).bytes_per_block;
}

#    if ACS_BUILD_DEBUG
/** Diligent の D3D12 デバイスから終了後レポート用 Debug Device を取得する。 */
render_internal::FD3D12DebugDeviceReportHandle CaptureDiligentDebugDevice(Diligent::IRenderDevice* device) noexcept
{
    if (!device) return {};
    Diligent::IRenderDeviceD3D12* diligent_device = nullptr;
    device->QueryInterface(Diligent::IID_RenderDeviceD3D12, reinterpret_cast<Diligent::IObject**>(&diligent_device));
    if (!diligent_device) return {};

    render_internal::FD3D12DebugDeviceReportHandle debug_device{};
    ID3D12Device* const native_device = diligent_device->GetD3D12Device();
    debug_device = render_internal::CaptureD3D12DebugDeviceReportHandle(native_device);
    diligent_device->Release();
    return debug_device;
}

#    endif

} // namespace

CDiligentDevice::~CDiligentDevice() noexcept
{
    // 直接利用時も、所有 context が投入済みの GPU 処理を完了してから解放する。
    if (m_Context) WaitIdle();
    Reset();
}

void CDiligentDevice::Reset() noexcept
{
#    if ACS_BUILD_DEBUG
    // D3D12 Factory の取得後に失敗した場合も DXGI 診断を残す。
    const bool report_d3d12_live_objects = m_ActualBackend == ERhiBackendKind::D3D12 && (m_Device || m_Factory);
    render_internal::FD3D12DebugDeviceReportHandle debug_device = m_Device && report_d3d12_live_objects
                                                                     ? CaptureDiligentDebugDevice(m_Device)
                                                                     : render_internal::FD3D12DebugDeviceReportHandle{};
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
    const u64 binding_generation = CDiligentMemoryAdapter::BindingGeneration();
    const u64 backing_lifetime_generation = CDiligentMemoryAdapter::BackingLifetimeGeneration();
    const u64 outstanding_allocation_count = CDiligentMemoryAdapter::OutstandingAllocationCount();
    const u64 outstanding_requested_bytes = CDiligentMemoryAdapter::OutstandingRequestedBytes();
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
    m_FrameSubmissionPending = false;
    for (u32 slot = 0; slot < kFramesInFlight; ++slot)
        m_FrameFences[slot] = 0;
    m_ActualBackend = ERhiBackendKind::Auto;
    m_AdapterName[0] = '\0';
    m_BackendName = "Diligent";
}

TResult<void> CDiligentDevice::Init(const FDeviceConfig& configuration) noexcept
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

TResult<void> CDiligentDevice::InitD3D12(const FDeviceConfig& configuration) noexcept
{
    FAllocator* const memory_segment = FMemorySystem::Get(ESegment::Resource);
    if (!memory_segment) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=initialization "
                      "status=failed reason=memory_system_unavailable backend=Diligent-D3D12");
        return ACS_ERR(Render, 105, "Diligent requires an initialized MemorySystem Resource segment");
    }
    auto* const diligent_memory_allocator = static_cast<Diligent::IMemoryAllocator*>(
        CDiligentMemoryAdapter::Create(memory_segment));
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
    eci.Features.TimestampQueries =
        Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    eci.Features.AsyncShaderCompilation =
        Diligent::DEVICE_FEATURE_STATE_OPTIONAL;

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
    if (!m_IdleFence) {
        ACS_LOG_ERROR("Diligent: CreateFence failed");
        return ACS_ERR(Render, 107, "CreateFence failed");
    }

    ACS_LOG_INFO("Diligent D3D12 device created: %s", m_AdapterName);
    return Ok();
}

TResult<void> CDiligentDevice::InitVulkan(const FDeviceConfig& configuration) noexcept
{
#    if WITH_RENDER_DILIGENT_VULKAN
    FAllocator* const memory_segment = FMemorySystem::Get(ESegment::Resource);
    if (!memory_segment) {
        ACS_LOG_ERROR("[acs][memory] tracker=diligent_memory_adapter record=initialization "
                      "status=failed reason=memory_system_unavailable backend=Diligent-Vulkan");
        return ACS_ERR(Render, 115, "Diligent requires an initialized MemorySystem Resource segment");
    }
    auto* const diligent_memory_allocator = static_cast<Diligent::IMemoryAllocator*>(
        CDiligentMemoryAdapter::Create(memory_segment));
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
    eci.Features.TimestampQueries =
        Diligent::DEVICE_FEATURE_STATE_OPTIONAL;
    eci.Features.AsyncShaderCompilation =
        Diligent::DEVICE_FEATURE_STATE_OPTIONAL;

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
    if (!m_IdleFence) {
        ACS_LOG_ERROR("Diligent Vulkan: CreateFence failed");
        return ACS_ERR(Render, 117, "CreateFence failed");
    }

    ACS_LOG_INFO("Diligent Vulkan device created: %s", m_AdapterName);
    return Ok();
#    else
    (void)configuration;
    return ACS_ERR(Render, 114, "Vulkan backend not built (WITH_RENDER_DILIGENT_VULKAN=0)");
#    endif
}

void CDiligentDevice::WaitIdle() noexcept
{
    if (!m_Context) return;

    // FinishFrame must precede WaitForIdle: FinishFrame moves dynamic descriptor
    // chunks to Diligent's stale list, and WaitForIdle's IdleCommandQueue(true)
    // then maps that list to the release queue and purges it after the GPU is idle.
    (void)FinishPendingSubmittedFrame();

    m_Context->WaitForIdle();
    if (m_Device) m_Device->ReleaseStaleResources(false);
}

bool CDiligentDevice::FinishPendingSubmittedFrame() noexcept
{
    if (!m_FrameSubmissionPending) return true;
    if (!m_Context) return false;

    // Submit() already flushed all commands. Close the Diligent frame now so its
    // dynamic allocations enter the stale list before the next real submission.
    m_Context->FinishFrame();
    if (m_Device) m_Device->ReleaseStaleResources(false);
    const bool fence_queued = QueueFinishedFrameFence();
    m_FrameSubmissionPending = false;
    return fence_queued;
}

void CDiligentDevice::PrepareCommandRecording() noexcept
{
    if (!m_Context) return;

    // A primary submission that never reached Present still needs a frame boundary.
    // FinishPendingSubmittedFrame closes it and submits a completion fence after
    // Diligent's frame-end work before this slot can be reused.
    (void)FinishPendingSubmittedFrame();

    // 今から書き込む frame slot は kFramesInFlight submissions 前に使ったもの。
    // 描画（dynamic descriptor allocation）を始める前に GPU 完了を待つ。
    const u64 reusable_slot_fence = m_FrameFences[m_FrameSlot];
    if (reusable_slot_fence != 0)
        WaitForFenceValue(reusable_slot_fence);

    // 完了 fence を観測した後に stale descriptor chunks を実際の free list へ戻す。
    if (m_Device) m_Device->ReleaseStaleResources(false);
}

bool CDiligentDevice::CanPrepareCommandRecordingWithoutWait() const noexcept
{
    if (!IsDeviceHealthy() || m_FrameSubmissionPending)
        return false;
    const u64 fence_value = m_FrameFences[m_FrameSlot];
    return fence_value == 0u ||
           (m_IdleFence != nullptr &&
            m_IdleFence->GetCompletedValue() >= fence_value);
}

void CDiligentDevice::MarkFrameSubmitted() noexcept
{
    if (!m_Context) return;

    // The commands are flushed, but their Diligent frame still needs either an
    // off-screen FinishFrame or the primary swapchain's Present.
    m_FrameSubmissionPending = true;
}

bool CDiligentDevice::NotifyPrimaryPresentFinished() noexcept
{
    if (!m_FrameSubmissionPending) return false;

    // IsPrimary Present has already called FinishFrame and ReleaseStaleResources.
    const bool fence_queued = QueueFinishedFrameFence();
    m_FrameSubmissionPending = false;
    return fence_queued;
}

bool CDiligentDevice::QueueFinishedFrameFence() noexcept
{
    if (!m_Context || !IsDeviceHealthy()) return false;
    const u32 submitted_slot = m_FrameSlot;
    const u64 fence_value = SignalGraphicsQueue();
    if (fence_value == 0) return false;
    // FinishFrame/primary Present may submit Diligent's trailing stale-resource
    // work internally. Flush the queued signal now so it orders after that work.
    m_Context->Flush();
    if (!IsDeviceHealthy()) return false;
    m_FrameFences[submitted_slot] = fence_value;
    AdvanceFrameSlot();
    return true;
}

bool CDiligentDevice::ReadTexture(IRhiTexture& texture, void* destination_pixels, u32 destination_size) noexcept
{
    if (!m_Device || !m_Context || destination_pixels == nullptr) return false;
    auto* dtex = static_cast<FDiligentTexture*>(&texture);
    Diligent::ITexture* src = dtex->Native();
    if (src == nullptr) return false;
    const Diligent::TextureDesc& sd = src->GetDesc();
    const u32 w = sd.Width, h = sd.Height;
    const u32 bpp = ReadbackBytesPerPixel(dtex->PixelFormat());
    if (bpp == 0u) return false;
    if (w == 0u || h == 0u) return false;
    const u64 dense_row_bytes = static_cast<u64>(w) * bpp;
    if (dense_row_bytes > destination_size ||
        static_cast<u64>(h) >
            static_cast<u64>(destination_size) / dense_row_bytes)
        return false;

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
    if (mapped.Stride < dense_row_bytes ||
        static_cast<u64>(h - 1u) >
            (static_cast<u64>(-1) - dense_row_bytes) / mapped.Stride) {
        m_Context->UnmapTextureSubresource(staging, 0, 0);
        return false;
    }
    const u64 row_offset =
        static_cast<u64>(h - 1u) * mapped.Stride;
    const u64 dense_slice_bytes = row_offset + dense_row_bytes;
    if (mapped.DepthStride != 0u &&
        mapped.DepthStride < dense_slice_bytes) {
        m_Context->UnmapTextureSubresource(staging, 0, 0);
        return false;
    }
    auto* dst = static_cast<u8*>(destination_pixels);
    const auto* srcp = static_cast<const u8*>(mapped.pData);
    for (u32 y = 0; y < h; ++y) {
        std::memcpy(dst + static_cast<usize>(y) * w * bpp, srcp + static_cast<usize>(y) * mapped.Stride, w * bpp);
    }
    m_Context->UnmapTextureSubresource(staging, 0, 0);
    return true;
}

u64 CDiligentDevice::SignalGraphicsQueue() noexcept
{
    if (!m_Context || !m_IdleFence) return 0;
    ++m_IdleValue;
    m_Context->EnqueueSignal(m_IdleFence, m_IdleValue);
    return m_IdleValue;
}

void CDiligentDevice::WaitForFenceValue(u64 fence_value) noexcept
{
    if (!m_IdleFence) return;
    // Diligent 2.5.6's D3D12 fence uses a manual-reset event without resetting it,
    // so IFence::Wait may return immediately after the first successful wait.
    while (m_IdleFence->GetCompletedValue() < fence_value)
        std::this_thread::yield();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
