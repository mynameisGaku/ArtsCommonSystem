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
        // structured buffer: compute / pixel shader で SRV としても UAV と
        // しても使えるように両 bind flag を立てる。Diligent の state tracker
        // が SRV ↔ UAV 遷移を自動で扱ってくれる。
        case BufferUsage::Storage:  return static_cast<Diligent::BIND_FLAGS>(
                                       Diligent::BIND_UNORDERED_ACCESS |
                                       Diligent::BIND_SHADER_RESOURCE);
        case BufferUsage::Staging:  return Diligent::BIND_NONE;
    }
    return Diligent::BIND_NONE;
}
} // namespace

Result<void> DiligentBuffer::Init(DiligentDevice& device, const BufferDesc& desc) noexcept {
    _device  = &device;
    _size    = desc.size;
    _usage   = desc.usage;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 120, "DiligentBuffer: device not initialized");

    Diligent::BufferDesc bd;
    bd.Name      = "ACS_Buffer";
    bd.Size      = static_cast<Diligent::Uint64>(desc.size);
    bd.BindFlags = BindFromUsage(desc.usage);

    if (desc.usage == BufferUsage::Staging) {
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

    dev->CreateBuffer(bd, p_init, &_buffer);
    if (!_buffer) {
        return ACS_ERR(Render, 121, "CreateBuffer failed");
    }

    // STAGING は別途 Update で書く (USAGE_DEFAULT は CreateBuffer 時に流し込み済)
    if (desc.initial_data && desc.size > 0 && bd.Usage == Diligent::USAGE_STAGING) {
        Update(desc.initial_data, desc.size, 0);
    }

    return Ok();
}

void DiligentBuffer::Update(const void* data, usize size, usize offset) noexcept {
    if (!_buffer || !_device || !data || size == 0) return;
    auto* ctx = _device->Context();
    if (!ctx) return;

    // USAGE_DEFAULT は UpdateBuffer、USAGE_STAGING は Map で書く
    if (_usage == BufferUsage::Staging) {
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
