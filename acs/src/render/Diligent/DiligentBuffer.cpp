// DiligentBuffer 実装
#include "render/Diligent/DiligentBuffer.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

DiligentBuffer::~DiligentBuffer() noexcept {
    if (_buffer) { _buffer->Release(); _buffer = nullptr; }
}

namespace {
Diligent::BIND_FLAGS BindFromUsage(BufferUsage u) noexcept {
    switch (u) {
        case BufferUsage::Vertex:   return Diligent::BIND_VERTEX_BUFFER;
        case BufferUsage::Index16:  return Diligent::BIND_INDEX_BUFFER;
        case BufferUsage::Index32:  return Diligent::BIND_INDEX_BUFFER;
        case BufferUsage::Uniform:  return Diligent::BIND_UNIFORM_BUFFER;
        case BufferUsage::Storage:  return Diligent::BIND_UNORDERED_ACCESS;
        case BufferUsage::Staging:  return Diligent::BIND_NONE;
    }
    return Diligent::BIND_NONE;
}
} // namespace

Result<void> DiligentBuffer::Init(DiligentDevice& device, const BufferDesc& desc) noexcept {
    _device  = &device;
    _size    = desc.size;
    _usage   = desc.usage;
    _dynamic = desc.cpu_writable;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 120, "DiligentBuffer: device not initialized");

    Diligent::BufferDesc bd;
    bd.Name           = "ACS_Buffer";
    bd.Size           = static_cast<Diligent::Uint64>(desc.size);
    bd.BindFlags      = BindFromUsage(desc.usage);
    bd.Usage          = desc.cpu_writable ? Diligent::USAGE_DYNAMIC : Diligent::USAGE_DEFAULT;
    bd.CPUAccessFlags = desc.cpu_writable ? Diligent::CPU_ACCESS_WRITE : Diligent::CPU_ACCESS_NONE;
    if (desc.usage == BufferUsage::Staging) {
        bd.Usage          = Diligent::USAGE_STAGING;
        bd.CPUAccessFlags = Diligent::CPU_ACCESS_WRITE;
    }

    Diligent::BufferData  bdata;
    Diligent::BufferData* p_init = nullptr;
    if (desc.initial_data && desc.size > 0 &&
        bd.Usage != Diligent::USAGE_DYNAMIC &&
        bd.Usage != Diligent::USAGE_STAGING) {
        bdata.pData    = desc.initial_data;
        bdata.DataSize = static_cast<Diligent::Uint64>(desc.size);
        p_init = &bdata;
    }

    dev->CreateBuffer(bd, p_init, &_buffer);
    if (!_buffer) {
        return ACS_ERR(Render, 121, "CreateBuffer failed");
    }

    // dynamic + 初期データの場合は Update で書き込み
    if (desc.initial_data && desc.size > 0 &&
        (bd.Usage == Diligent::USAGE_DYNAMIC || bd.Usage == Diligent::USAGE_STAGING)) {
        Update(desc.initial_data, desc.size, 0);
    }

    return Ok();
}

void DiligentBuffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!_buffer || !_device || !data || size == 0) return;
    auto* ctx = _device->Context();
    if (!ctx) return;

    if (_dynamic) {
        void* p = nullptr;
        ctx->MapBuffer(_buffer, Diligent::MAP_WRITE, Diligent::MAP_FLAG_DISCARD, p);
        if (p) {
            std::memcpy(static_cast<u8*>(p) + offset, data, size);
            ctx->UnmapBuffer(_buffer, Diligent::MAP_WRITE);
        }
    } else {
        ctx->UpdateBuffer(_buffer,
                          static_cast<Diligent::Uint64>(offset),
                          static_cast<Diligent::Uint64>(size),
                          data,
                          Diligent::RESOURCE_STATE_TRANSITION_MODE_TRANSITION);
    }
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
