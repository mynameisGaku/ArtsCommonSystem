// SPDX-License-Identifier: Apache-2.0
// DX12 グラフィックスパイプライン実装（PSO + RootSignature）
#pragma once

#include "render/IRhiPipeline.h"
#include "render/Dx12/Dx12Common.h"

namespace acs {

class FDx12Device;

class FDx12Pipeline final : public IRhiPipeline {
public:
    FDx12Pipeline() noexcept = default;
    ~FDx12Pipeline() noexcept override;

    FHrResult Init(FDx12Device& device, const FPipelineDesc& desc) noexcept;

    ID3D12PipelineState*   Pso()           const noexcept { return _pso; }
    ID3D12RootSignature*   RootSignature() const noexcept { return _root_sig; }
    EPrimitiveTopology      Topology()      const noexcept { return _topology; }
    u32                    CBufferSlots()  const noexcept { return _cbuffer_slots; }
    u32                    TextureSlots()  const noexcept { return _texture_slots; }

private:
    ID3D12PipelineState* _pso      = nullptr;
    ID3D12RootSignature* _root_sig = nullptr;
    EPrimitiveTopology    _topology = EPrimitiveTopology::TriangleList;
    u32                  _cbuffer_slots = 0;
    u32                  _texture_slots = 0;
};

} // namespace acs
