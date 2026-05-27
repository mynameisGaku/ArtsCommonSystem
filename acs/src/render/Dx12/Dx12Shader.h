// SPDX-License-Identifier: Apache-2.0
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

    HrResult Init(Dx12Device& device, const FShaderDesc& desc) noexcept;

    EShaderStage Stage() const noexcept override { return m_Stage; }
    const byte* Bytecode() const noexcept override {
        return m_Blob ? static_cast<const byte*>(m_Blob->GetBufferPointer()) : nullptr;
    }
    usize BytecodeSize() const noexcept override {
        return m_Blob ? m_Blob->GetBufferSize() : 0;
    }

private:
    ID3DBlob*    m_Blob  = nullptr;
    EShaderStage  m_Stage = EShaderStage::Vertex;
};

} // namespace acs
