// SPDX-License-Identifier: Apache-2.0
// FDiligentBuffer 実装
#include "render/Diligent/DiligentBuffer.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

/** 保持している Diligent バッファを解放する。 */
FDiligentBuffer::~FDiligentBuffer() noexcept {
    Reset();
}

void FDiligentBuffer::Reset() noexcept
{
    m_Srv = nullptr;
    m_Uav = nullptr;
    if (m_Buffer) { m_Buffer->Release(); m_Buffer = nullptr; }
    m_Device = nullptr;
    m_Size = 0;
    m_Usage = EBufferUsage::Vertex;
}

namespace {
/** フレーム分離した bind offset を指定できる用途かを返す。 */
bool SupportsFrameCycling_Internal(EBufferUsage usage) noexcept
{
    return usage == EBufferUsage::Vertex ||
           usage == EBufferUsage::Index16 ||
           usage == EBufferUsage::Index32 ||
           usage == EBufferUsage::Uniform;
}

/**
 * EBufferUsage を Diligent の BIND_FLAGS へ変換する。
 *
 * @details
 * Storage は SRV/UAV 両方で使えるよう BIND_UNORDERED_ACCESS と BIND_SHADER_RESOURCE を
 * 立てる。Staging には bind flag を付けない。
 * @param u 変換元のバッファ用途。
 * @return 対応する Diligent::BIND_FLAGS (未対応なら BIND_NONE)。
 */
Diligent::BIND_FLAGS BindFromUsage(EBufferUsage u) noexcept {
    switch (u) {
        case EBufferUsage::Vertex:   return Diligent::BIND_VERTEX_BUFFER;
        case EBufferUsage::Index16:  return Diligent::BIND_INDEX_BUFFER;
        case EBufferUsage::Index32:  return Diligent::BIND_INDEX_BUFFER;
        case EBufferUsage::Uniform:  return Diligent::BIND_UNIFORM_BUFFER;
        // structured buffer: compute / pixel shader で SRV としても UAV と
        // しても使えるように両 bind flag を立てる。Diligent の state tracker
        // が SRV ↔ UAV 遷移を自動で扱ってくれる。
        case EBufferUsage::Storage:  return static_cast<Diligent::BIND_FLAGS>(
                                       Diligent::BIND_UNORDERED_ACCESS |
                                       Diligent::BIND_SHADER_RESOURCE);
        case EBufferUsage::Staging:  return Diligent::BIND_NONE;
    }
    return Diligent::BIND_NONE;
}
} // namespace

/** desc に従って GPU バッファを生成する。 */
TResult<void> FDiligentBuffer::Init(CDiligentDevice& device, const FBufferDesc& desc) noexcept {
    Reset();

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 120, "FDiligentBuffer: device not initialized");

    // 描画中の GPU が前フレーム領域を読む間、CPU 更新は現在スロットだけへ記録する。
    const bool frame_cycled = desc.cpu_writable && SupportsFrameCycling_Internal(desc.usage);
    usize slot_stride = desc.size;
    usize physical_size = desc.size;
    if (frame_cycled) {
        const usize max_size = static_cast<usize>(-1);
        if (desc.size == 0u || desc.size > max_size - 255u) {
            return ACS_ERR(Render, 122, "FDiligentBuffer: invalid frame-cycled size");
        }
        slot_stride = (desc.size + 255u) & ~static_cast<usize>(255u);
        if (slot_stride > max_size / CDiligentDevice::kFramesInFlight) {
            return ACS_ERR(Render, 122, "FDiligentBuffer: frame-cycled size overflow");
        }
        physical_size = slot_stride * CDiligentDevice::kFramesInFlight;
    }

    m_Device  = &device;
    m_Size    = desc.size;
    m_Usage   = desc.usage;

    Diligent::BufferDesc bd;
    bd.Name      = "ACS_Buffer";
    bd.Size      = static_cast<Diligent::Uint64>(physical_size);
    bd.BindFlags = BindFromUsage(desc.usage);
    if (desc.indirect_args) {   // DispatchIndirect の引数バッファにも使う
        bd.BindFlags = static_cast<Diligent::BIND_FLAGS>(
            bd.BindFlags | Diligent::BIND_INDIRECT_DRAW_ARGS);
    }
    // 要素幅が指定されたバッファには計算処理用の SRV/UAV view を作る。
    if (desc.struct_stride > 0) {
        bd.Mode             = Diligent::BUFFER_MODE_STRUCTURED;
        bd.ElementByteStride = desc.struct_stride;
    }

    if (desc.usage == EBufferUsage::Staging) {
        // CPU 読み戻し用
        bd.Usage          = Diligent::USAGE_STAGING;
        bd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    } else if (!desc.cpu_writable && desc.initial_data) {
        // 静的データ (vertex/index for static mesh 等): IMMUTABLE で作って
        // 以後 read-only。CreateBuffer 時に初期データ流し込み、re-update 不可。
        // Diligent の resource state tracker が正しく VERTEX_BUFFER/INDEX_BUFFER
        // 状態に置いてくれるので、後の Draw で COPY_DEST 状態残り問題が起きない。
        bd.Usage          = Diligent::USAGE_IMMUTABLE;
        bd.CPUAccessFlags = Diligent::CPU_ACCESS_NONE;
    } else {
        // 動的更新する constant buffer 等は USAGE_DEFAULT + UpdateBuffer 経路。
        // (USAGE_DYNAMIC はフレーム末で破棄されるので vertex/index には使わない)
        bd.Usage          = Diligent::USAGE_DEFAULT;
        bd.CPUAccessFlags = Diligent::CPU_ACCESS_NONE;
    }

    Diligent::BufferData  bdata;
    Diligent::BufferData* p_init = nullptr;
    if (!frame_cycled && desc.initial_data && desc.size > 0 && (bd.Usage == Diligent::USAGE_DEFAULT || bd.Usage == Diligent::USAGE_IMMUTABLE)) {
        bdata.pData    = desc.initial_data;
        bdata.DataSize = static_cast<Diligent::Uint64>(desc.size);
        p_init = &bdata;
    }

    dev->CreateBuffer(bd, p_init, &m_Buffer);
    if (!m_Buffer) {
        Reset();
        return ACS_ERR(Render, 121, "CreateBuffer failed");
    }

    // 構造化バッファの default SRV/UAV view (compute の StructuredBuffer / RWStructuredBuffer)。
    if (desc.struct_stride > 0) {
        m_Srv = m_Buffer->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE);
        if (bd.BindFlags & Diligent::BIND_UNORDERED_ACCESS)
            m_Uav = m_Buffer->GetDefaultView(Diligent::BUFFER_VIEW_UNORDERED_ACCESS);
    }

    // フレーム分離する初期データは、どのスロットから開始しても同じ内容になるよう複製する。
    if (frame_cycled && desc.initial_data && desc.size > 0u) {
        auto* context = device.Context();
        if (!context) {
            Reset();
            return ACS_ERR(Render, 123, "FDiligentBuffer: context not initialized");
        }
        for (u32 slot = 0u; slot < CDiligentDevice::kFramesInFlight; ++slot) {
            context->UpdateBuffer(m_Buffer, static_cast<Diligent::Uint64>(slot * slot_stride), static_cast<Diligent::Uint64>(desc.size), desc.initial_data, Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
        }
    }

    // STAGING は別途 Update で書く (USAGE_DEFAULT は CreateBuffer 時に流し込み済)
    if (desc.initial_data && desc.size > 0 && bd.Usage == Diligent::USAGE_STAGING) {
        Update(desc.initial_data, desc.size, 0);
    }

    return Ok();
}

/** バッファの内容を更新する (Staging は Map、それ以外は UpdateBuffer)。 */
void FDiligentBuffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!m_Buffer || !m_Device || !data || size == 0) return;
    if (offset > m_Size || size > m_Size - offset) return;
    auto* ctx = m_Device->Context();
    if (!ctx) return;

    // USAGE_DEFAULT は UpdateBuffer、USAGE_STAGING は Map で書く
    if (m_Usage == EBufferUsage::Staging) {
        void* p = nullptr;
        ctx->MapBuffer(m_Buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, p);
        if (p) {
            std::memcpy(static_cast<u8*>(p) + offset, data, size);
            ctx->UnmapBuffer(m_Buffer, Diligent::MAP_WRITE);
        }
    } else {
        const usize physical_offset = BindingOffset() + offset;
        ctx->UpdateBuffer(m_Buffer,
                          static_cast<Diligent::Uint64>(physical_offset),
                          static_cast<Diligent::Uint64>(size),
                          data,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

/** 現在フレームが使う実バッファ内の先頭位置を返す。 */
usize FDiligentBuffer::BindingOffset() const noexcept
{
    if (!IsFrameCycled_Internal() || !m_Device) return 0u;
    return FrameSlotStride_Internal() * m_Device->CurrentFrameSlot();
}

/** 実バッファがフレームスロットごとの領域を持つかを返す。 */
bool FDiligentBuffer::IsFrameCycled_Internal() const noexcept
{
    if (!m_Buffer || !SupportsFrameCycling_Internal(m_Usage) || m_Size == 0u) return false;
    const usize slot_stride = FrameSlotStride_Internal();
    const usize physical_size = slot_stride * CDiligentDevice::kFramesInFlight;
    return m_Buffer->GetDesc().Size == static_cast<Diligent::Uint64>(physical_size);
}

/** 1 フレームスロットの 256 バイト境界へ切り上げた幅を返す。 */
usize FDiligentBuffer::FrameSlotStride_Internal() const noexcept
{
    return (m_Size + 255u) & ~static_cast<usize>(255u);
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
