// DiligentShader 実装
#include "render/Diligent/DiligentShader.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "foundation/Log.h"

namespace acs {

DiligentShader::~DiligentShader() noexcept {
    if (_shader) { _shader->Release(); _shader = nullptr; }
}

Result<void> DiligentShader::Init(DiligentDevice& device, const ShaderDesc& desc) noexcept {
    _device = &device;
    _stage  = desc.stage;

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 140, "DiligentShader: device not initialized");
    if (!desc.hlsl_source) return ACS_ERR(Render, 141, "DiligentShader: hlsl_source is null");

    Diligent::ShaderCreateInfo sci;
    sci.SourceLanguage = Diligent::SHADER_SOURCE_LANGUAGE_HLSL;
    // Diligent 内蔵の DXC コンパイラを使う（DX12/Vulkan/Metal すべての変換に対応）
    sci.ShaderCompiler = Diligent::SHADER_COMPILER_DEFAULT;
    sci.HLSLVersion    = {6, 0};
    sci.Source         = desc.hlsl_source;
    sci.EntryPoint     = desc.entry_point ? desc.entry_point : "main";

    // Diligent 新版で UseCombinedTextureSamplers / CombinedSamplerSuffix は
    // ShaderCreateInfo から ShaderDesc に移動した。
    // true にすると Diligent が <texture>_sampler 名で sampler を自動紐付け
    // するので、PSO 側の ImmutableSamplerDesc::SamplerOrTextureName に
    // テクスチャ名 ("albedo" 等) を渡すだけで sampler binding が成立する。
    // (D3D12 でも HLSL は分離宣言のまま、紐付けの abstraction)
    Diligent::ShaderDesc sd;
    sd.Name                       = desc.debug_name ? desc.debug_name : "ACS_Shader";
    sd.ShaderType                 = diligent_detail::ToDiligent(desc.stage);
    sd.UseCombinedTextureSamplers = true;
    sd.CombinedSamplerSuffix      = "_sampler";
    sci.Desc                      = sd;

    dev->CreateShader(sci, &_shader);
    if (!_shader) {
        ACS_LOG_ERROR("Diligent: CreateShader failed (entry=%s, name=%s)",
                      sci.EntryPoint, sd.Name);
        return ACS_ERR(Render, 142, "CreateShader failed");
    }

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
