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

    TResult<void> Init(DiligentDevice& device, const FBufferDesc& desc) noexcept;

    // ---- IRhiBuffer ----
    usize       Size()  const noexcept override { return m_Size; }
    EBufferUsage Usage() const noexcept override { return m_Usage; }
    void        Update(const void* data, usize size, usize offset = 0) noexcept override;

    // 内部公開
    Diligent::IBuffer* Native() const noexcept { return m_Buffer; }

private:
    DiligentDevice*    m_Device = nullptr;
    Diligent::IBuffer* m_Buffer = nullptr;
    usize              m_Size   = 0;
    EBufferUsage        m_Usage  = EBufferUsage::Vertex;
};

} // namespace acs
