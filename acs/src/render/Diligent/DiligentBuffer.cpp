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
    m_Device  = &device;
    m_Size    = desc.size;
    m_Usage   = desc.usage;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 120, "FDiligentBuffer: device not initialized");

    Diligent::BufferDesc bd;
    bd.Name      = "ACS_Buffer";
    bd.Size      = static_cast<Diligent::Uint64>(desc.size);
    bd.BindFlags = BindFromUsage(desc.usage);
    if (desc.indirect_args) {   // DispatchIndirect の引数バッファにも使う
        bd.BindFlags = static_cast<Diligent::BIND_FLAGS>(
            bd.BindFlags | Diligent::BIND_INDIRECT_DRAW_ARGS);
    }
    if (desc.struct_stride > 0) {   // 構造化バッファ (Phase 0): SRV/UAV view が作られる
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
    if (desc.initial_data && desc.size > 0 &&
        (bd.Usage == Diligent::USAGE_DEFAULT || bd.Usage == Diligent::USAGE_IMMUTABLE)) {
        bdata.pData    = desc.initial_data;
        bdata.DataSize = static_cast<Diligent::Uint64>(desc.size);
        p_init = &bdata;
    }

    dev->CreateBuffer(bd, p_init, &m_Buffer);
    if (!m_Buffer) {
        return ACS_ERR(Render, 121, "CreateBuffer failed");
    }

    // 構造化バッファの default SRV/UAV view (compute の StructuredBuffer / RWStructuredBuffer)。
    if (desc.struct_stride > 0) {
        m_Srv = m_Buffer->GetDefaultView(Diligent::BUFFER_VIEW_SHADER_RESOURCE);
        if (bd.BindFlags & Diligent::BIND_UNORDERED_ACCESS)
            m_Uav = m_Buffer->GetDefaultView(Diligent::BUFFER_VIEW_UNORDERED_ACCESS);
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
        ctx->UpdateBuffer(m_Buffer,
                          static_cast<Diligent::Uint64>(offset),
                          static_cast<Diligent::Uint64>(size),
                          data,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
