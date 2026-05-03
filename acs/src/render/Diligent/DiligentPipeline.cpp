// DiligentPipeline 実装
#include "render/Diligent/DiligentPipeline.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "render/Diligent/DiligentShader.h"
#include "foundation/Log.h"

namespace acs {

namespace {
// "cb0".."cb7" / "t0".."t7" の固定文字列（フォールバック名）。
// PipelineDesc.cbuffer_names / texture_names が省略されたとき使われる。
const char* const kCbFallback[8] = { "cb0","cb1","cb2","cb3","cb4","cb5","cb6","cb7" };
const char* const kTexFallback[8] = { "t0","t1","t2","t3","t4","t5","t6","t7" };
} // namespace

const char* DiligentPipeline::FallbackCbName(u32 slot) noexcept {
    return slot < 8 ? kCbFallback[slot] : "cb_invalid";
}
const char* DiligentPipeline::FallbackTexName(u32 slot) noexcept {
    return slot < 8 ? kTexFallback[slot] : "t_invalid";
}

const char* DiligentPipeline::CbufferName(u32 slot) const noexcept {
    if (slot >= kMaxResourceSlots) return FallbackCbName(slot);
    return _cb_names[slot] ? _cb_names[slot] : FallbackCbName(slot);
}
const char* DiligentPipeline::TextureName(u32 slot) const noexcept {
    if (slot >= kMaxResourceSlots) return FallbackTexName(slot);
    return _tex_names[slot] ? _tex_names[slot] : FallbackTexName(slot);
}

DiligentPipeline::~DiligentPipeline() noexcept {
    if (_srb) { _srb->Release(); _srb = nullptr; }
    if (_pso) { _pso->Release(); _pso = nullptr; }
}

Result<void> DiligentPipeline::Init(DiligentDevice& device, const PipelineDesc& desc) noexcept {
    _device = &device;
    _cb_slots  = desc.cbuffer_slots;
    _tex_slots = desc.texture_slots;

    // PipelineDesc の名前を取り込む（null は維持してフォールバックさせる）
    for (u32 i = 0; i < kMaxResourceSlots; ++i) {
        _cb_names[i]  = desc.cbuffer_names[i];
        _tex_names[i] = desc.texture_names[i];
    }

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 150, "DiligentPipeline: device not initialized");

    auto* vs = static_cast<DiligentShader*>(desc.vs);
    auto* ps = static_cast<DiligentShader*>(desc.ps);
    if (!vs || !vs->Native() || !ps || !ps->Native()) {
        return ACS_ERR(Render, 151, "DiligentPipeline: VS or PS missing");
    }

    Diligent::GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name            = "ACS_GraphicsPSO";
    psoCI.PSODesc.PipelineType    = Diligent::PIPELINE_TYPE_GRAPHICS;

    auto& gp = psoCI.GraphicsPipeline;
    gp.NumRenderTargets             = 1;
    gp.RTVFormats[0]                = diligent_detail::ToDiligent(desc.rt_format);
    gp.DSVFormat                    = (desc.depth_format == Format::Unknown)
                                       ? Diligent::TEX_FORMAT_UNKNOWN
                                       : diligent_detail::ToDiligent(desc.depth_format);
    gp.PrimitiveTopology            = diligent_detail::ToDiligent(desc.topology);
    gp.RasterizerDesc.CullMode      = diligent_detail::ToDiligent(desc.cull_mode);
    gp.RasterizerDesc.FillMode      = Diligent::FILL_MODE_SOLID;
    gp.RasterizerDesc.FrontCounterClockwise = false;
    gp.DepthStencilDesc.DepthEnable = desc.depth_test && desc.depth_format != Format::Unknown;
    gp.DepthStencilDesc.DepthWriteEnable = desc.depth_write;
    gp.DepthStencilDesc.DepthFunc   = Diligent::COMPARISON_FUNC_LESS_EQUAL;

    diligent_detail::ApplyBlend(desc.blend_mode, gp.BlendDesc.RenderTargets[0]);
    gp.BlendDesc.RenderTargets[0].RenderTargetWriteMask = Diligent::COLOR_MASK_ALL;

    psoCI.pVS = vs->Native();
    psoCI.pPS = ps->Native();

    // 入力レイアウト（最大 8 要素）
    Diligent::LayoutElement layout[8]{};
    for (u32 i = 0; i < desc.layout_count && i < 8; ++i) {
        const auto& e = desc.layout[i];
        layout[i].InputIndex      = i;
        layout[i].BufferSlot      = 0;
        layout[i].NumComponents   = 0;
        layout[i].ValueType       = Diligent::VT_FLOAT32;
        layout[i].IsNormalized    = false;
        layout[i].RelativeOffset  = e.offset;
        layout[i].HLSLSemantic    = e.semantic_name ? e.semantic_name : "POSITION";

        switch (e.format) {
            case Format::R32G32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 2; break;
            case Format::R32G32B32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 3; break;
            case Format::R32G32B32A32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 4; break;
            case Format::R8G8B8A8_UNorm:
                layout[i].ValueType = Diligent::VT_UINT8;   layout[i].NumComponents = 4;
                layout[i].IsNormalized = true; break;
            case Format::R8G8B8A8_UInt:
                layout[i].ValueType = Diligent::VT_UINT8;   layout[i].NumComponents = 4;
                layout[i].IsNormalized = false; break;
            default:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 4; break;
        }
    }
    gp.InputLayout.LayoutElements = desc.layout_count > 0 ? layout : nullptr;
    gp.InputLayout.NumElements    = desc.layout_count;

    // === Resource layout ===
    // 各 texture に対応する static sampler を `<TextureName>_sampler` の名前で
    // ImmutableSampler 登録する（Diligent の CombinedSamplerSuffix と一致）。
    psoCI.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;

    constexpr u32 kMaxStaticSamplers = 4;
    Diligent::ImmutableSamplerDesc samplers[kMaxStaticSamplers]{};
    u32 ns = desc.static_sampler_count > kMaxStaticSamplers
              ? kMaxStaticSamplers : desc.static_sampler_count;
    for (u32 i = 0; i < ns; ++i) {
        const auto& s = desc.static_samplers[i];
        // i 番目の static sampler は i 番目の texture に対応するという規約
        samplers[i].SamplerOrTextureName = TextureName(i);
        samplers[i].ShaderStages = Diligent::SHADER_TYPE_PIXEL;
        samplers[i].Desc.Name = "ACS_StaticSampler";
        samplers[i].Desc.MinFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.MagFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.MipFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.AddressU  = diligent_detail::ToDiligentAddress(s.address_u);
        samplers[i].Desc.AddressV  = diligent_detail::ToDiligentAddress(s.address_v);
        samplers[i].Desc.AddressW  = diligent_detail::ToDiligentAddress(s.address_w);
        samplers[i].Desc.MaxAnisotropy = s.max_anisotropy;
        samplers[i].Desc.MinLOD = s.min_lod;
        samplers[i].Desc.MaxLOD = s.max_lod;
    }
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = ns > 0 ? samplers : nullptr;
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = ns;

    dev->CreateGraphicsPipelineState(psoCI, &_pso);
    if (!_pso) {
        return ACS_ERR(Render, 152, "CreateGraphicsPipelineState failed");
    }

    _pso->CreateShaderResourceBinding(&_srb, true);
    if (!_srb) {
        return ACS_ERR(Render, 153, "CreateShaderResourceBinding failed");
    }

    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
