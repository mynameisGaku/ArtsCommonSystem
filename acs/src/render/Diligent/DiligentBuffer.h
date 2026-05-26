// SPDX-License-Identifier: Apache-2.0
// Diligent Engine 経由の GPU バッファ
#pragma once

#include "render/IRhiBuffer.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IBuffer;
}

namespace acs {

class FDiligentDevice;

class FDiligentBuffer final : public IRhiBuffer {
public:
    FDiligentBuffer() noexcept = default;
    ~FDiligentBuffer() noexcept override;

    FDiligentBuffer(const FDiligentBuffer&) = delete;
    FDiligentBuffer& operator=(const FDiligentBuffer&) = delete;

    TResult<void> Init(FDiligentDevice& device, const FBufferDesc& desc) noexcept;

    // ---- IRhiBuffer ----
    usize       Size()  const noexcept override { return _size; }
    EBufferUsage Usage() const noexcept override { return _usage; }
    void        Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部公開
    Diligent::IBuffer* Native() const noexcept { return _buffer; }

private:
    FDiligentDevice*    _device = nullptr;
    Diligent::IBuffer* _buffer = nullptr;
    usize              _size   = 0;
    EBufferUsage        _usage  = EBufferUsage::FVertex;
};

} // namespace acs
