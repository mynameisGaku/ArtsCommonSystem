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

    HrResult Init(Dx12Device& device, const PipelineDesc& desc) noexcept;

    ID3D12PipelineState*   Pso()           const noexcept { return _pso; }
    ID3D12RootSignature*   RootSignature() const noexcept { return _root_sig; }
    PrimitiveTopology      Topology()      const noexcept { return _topology; }

private:
    ID3D12PipelineState* _pso      = nullptr;
    ID3D12RootSignature* _root_sig = nullptr;
    PrimitiveTopology    _topology = PrimitiveTopology::TriangleList;
};

} // namespace acs
