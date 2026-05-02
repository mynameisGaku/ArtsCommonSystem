// DX12 シェーダ実装（D3DCompile による HLSL コンパイル）
#pragma once

#include "render/IRhiShader.h"
#include "render/Dx12/Dx12Common.h"
#include "container/Array.h"

#include <d3dcompiler.h>

namespace acs {

class Dx12Device;

class Dx12Shader final : public IRhiShader {
public:
    Dx12Shader() noexcept = default;
    ~Dx12Shader() noexcept override;

    HrResult Init(Dx12Device& device, const ShaderDesc& desc) noexcept;

    ShaderStage Stage() const noexcept override { return _stage; }
    const byte* Bytecode() const noexcept override {
        return _blob ? static_cast<const byte*>(_blob->GetBufferPointer()) : nullptr;
    }
    usize BytecodeSize() const noexcept override {
        return _blob ? _blob->GetBufferSize() : 0;
    }

private:
    ID3DBlob*    _blob  = nullptr;
    ShaderStage  _stage = ShaderStage::Vertex;
};

} // namespace acs
