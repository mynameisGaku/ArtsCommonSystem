// Diligent Engine 経由のシェーダ
//
// 注: Diligent は内部で HLSL → SPIRV/MSL/GLSL に変換するため、
// Bytecode() は便宜的に nullptr を返す（Pipeline 作成時には IShader* を直接使う）。
#pragma once

#include "render/IRhiShader.h"
#include "memory/UniquePtr.h"

namespace Diligent {
    struct IShader;
}

namespace acs {

class DiligentDevice;

class DiligentShader final : public IRhiShader {
public:
    DiligentShader() noexcept = default;
    ~DiligentShader() noexcept override;

    DiligentShader(const DiligentShader&) = delete;
    DiligentShader& operator=(const DiligentShader&) = delete;

    Result<void> Init(DiligentDevice& device, const ShaderDesc& desc) noexcept;

    // ---- IRhiShader ----
    ShaderStage Stage()         const noexcept override { return _stage; }
    const byte* Bytecode()      const noexcept override { return nullptr; }
    usize       BytecodeSize()  const noexcept override { return 0; }

    // 内部公開
    Diligent::IShader* Native() const noexcept { return _shader; }

private:
    DiligentDevice*    _device = nullptr;
    Diligent::IShader* _shader = nullptr;
    ShaderStage        _stage  = ShaderStage::Vertex;
};

} // namespace acs
