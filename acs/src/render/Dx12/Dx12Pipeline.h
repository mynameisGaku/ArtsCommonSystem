// SPDX-License-Identifier: Apache-2.0
// DX12 グラフィックスパイプライン実装（PSO + RootSignature）
#pragma once

#include "render/IRhiPipeline.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class Dx12Device;

class Dx12Pipeline final : public IRhiPipeline {
public:
    Dx12Pipeline() noexcept = default;
    ~Dx12Pipeline() noexcept override;

    HrResult Init(Dx12Device& device, const FPipelineDesc& desc) noexcept;

    ID3D12PipelineState*   Pso()           const noexcept { return m_Pso; }
    ID3D12RootSignature*   RootSignature() const noexcept { return m_RootSig; }
    EPrimitiveTopology      Topology()      const noexcept { return m_Topology; }
    u32                    CBufferSlots()  const noexcept { return m_CbufferSlots; }
    u32                    TextureSlots()  const noexcept { return m_TextureSlots; }

private:
    ID3D12PipelineState* m_Pso      = nullptr;
    ID3D12RootSignature* m_RootSig = nullptr;
    EPrimitiveTopology    m_Topology = EPrimitiveTopology::TriangleList;
    u32                  m_CbufferSlots = 0;
    u32                  m_TextureSlots = 0;
};

} // namespace acs
