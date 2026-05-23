// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の GPU バッファ
#pragma once

#include "render/IRhiBuffer.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IBuffer;
}

namespace acs {

class DiligentDevice;

class DiligentBuffer final : public IRhiBuffer {
public:
    DiligentBuffer() noexcept = default;
    ~DiligentBuffer() noexcept override;

    DiligentBuffer(const DiligentBuffer&) = delete;
    DiligentBuffer& operator=(const DiligentBuffer&) = delete;

    Result<void> Init(DiligentDevice& device, const BufferDesc& desc) noexcept;

    // ---- IRhiBuffer ----
    usize       Size()  const noexcept override { return _size; }
    EBufferUsage Usage() const noexcept override { return _usage; }
    void        Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部公開
    Diligent::IBuffer* Native() const noexcept { return _buffer; }

private:
    DiligentDevice*    _device = nullptr;
    Diligent::IBuffer* _buffer = nullptr;
    usize              _size   = 0;
    EBufferUsage        _usage  = EBufferUsage::Vertex;
};

} // namespace acs
