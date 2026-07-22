// SPDX-License-Identifier: Apache-2.0
// FDiligentPipeline 実装
#include "render/Diligent/DiligentPipeline.h"

#if WITH_RENDER_DILIGENT

#include "render/Diligent/DiligentCommon.h"
#include "render/Diligent/DiligentDevice.h"
#include "render/Diligent/DiligentShader.h"
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {
// "cb0".."cb7" / "t0".."t7" の固定文字列（フォールバック名）。
// FPipelineDesc.cbuffer_names / texture_names が省略されたとき使われる。
const char* const kCbFallback[16] = {
    "cb0","cb1","cb2","cb3","cb4","cb5","cb6","cb7",
    "cb8","cb9","cb10","cb11","cb12","cb13","cb14","cb15"
};
const char* const kTexFallback[16] = {
    "t0","t1","t2","t3","t4","t5","t6","t7",
    "t8","t9","t10","t11","t12","t13","t14","t15"
};
} // namespace

const char* FDiligentPipeline::FallbackCbName(u32 slot) noexcept {
    return slot < 16 ? kCbFallback[slot] : "cb_invalid";
}
const char* FDiligentPipeline::FallbackTexName(u32 slot) noexcept {
    return slot < 16 ? kTexFallback[slot] : "t_invalid";
}

const char* FDiligentPipeline::CbufferName(u32 slot) const noexcept {
    if (slot >= kMaxResourceSlots) return FallbackCbName(slot);
    return m_CbNames[slot] ? m_CbNames[slot] : FallbackCbName(slot);
}
const char* FDiligentPipeline::TextureName(u32 slot) const noexcept {
    if (slot >= kMaxResourceSlots) return FallbackTexName(slot);
    return m_TexNames[slot] ? m_TexNames[slot] : FallbackTexName(slot);
}

FDiligentPipeline::~FDiligentPipeline() noexcept {
    Reset();
}

void FDiligentPipeline::Reset() noexcept
{
    if (m_Srb) { m_Srb->Release(); m_Srb = nullptr; }
    if (m_Pso) { m_Pso->Release(); m_Pso = nullptr; }
    m_Device = nullptr;
    m_CbSlots = 0;
    m_TexSlots = 0;
    m_IsCompute = false;
    m_UavSlots = 0;
    for (u32 i = 0; i < kMaxResourceSlots; ++i) {
        m_CbNames[i] = nullptr;
        m_TexNames[i] = nullptr;
        m_UavNames[i] = nullptr;
    }
}

TResult<void> FDiligentPipeline::Init(FDiligentDevice& device, const FPipelineDesc& desc) noexcept {
    Reset();
    m_Device = &device;
    m_CbSlots  = desc.cbuffer_slots;
    m_TexSlots = desc.texture_slots;

    // FPipelineDesc の名前を取り込む（null は維持してフォールバックさせる）
    for (u32 i = 0; i < kMaxResourceSlots; ++i) {
        m_CbNames[i]  = desc.cbuffer_names[i];
        m_TexNames[i] = desc.texture_names[i];
    }

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 150, "FDiligentPipeline: device not initialized");

    auto* vs = static_cast<FDiligentShader*>(desc.vs);
    auto* ps = static_cast<FDiligentShader*>(desc.ps);
    if (!vs || !vs->Native()) {
        return ACS_ERR(Render, 151, "FDiligentPipeline: VS missing");
    }

    // FPipelineDesc.cbuffer_names / texture_names が未指定の slot を、shader
    // が source parse して保持してる register(bN/tN) → 名前マッピングから
    // 補完する。VS と PS で同 slot に異なる名前がある場合は VS を優先 (ACS
    // 慣行: VS / PS で同じ cbuffer を共有)。Diligent::ShaderResourceDesc に
    // BindPoint が無い問題を回避し、HLSL declaration 順依存を消す。
    for (u32 i = 0; i < kMaxResourceSlots && i < FDiligentShader::kMaxSlots; ++i) {
        if (!m_CbNames[i]) {
            const char* n = vs->CbufferNameAt(i);
            if (!n && ps) n = ps->CbufferNameAt(i);
            if (n) m_CbNames[i] = n;
        }
        if (!m_TexNames[i]) {
            const char* n = ps ? ps->TextureNameAt(i) : nullptr;
            if (!n) n = vs->TextureNameAt(i);
            if (n) m_TexNames[i] = n;
        }
    }
    // PS は depth-only pass (FShadowMap 等) で null OK。NumRenderTargets=0、
    // RTVFormats[0]=UNKNOWN にして PSO 作成する。

    Diligent::GraphicsPipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name            = "ACS_GraphicsPSO";
    psoCI.PSODesc.PipelineType    = Diligent::PIPELINE_TYPE_GRAPHICS;

    const bool has_ps = ps && ps->Native();

    auto& gp = psoCI.GraphicsPipeline;
    // MRT 対応: desc.rt_count > 0 のとき rt_formats[] を使い、
    // それ以外は legacy 単一 RT (desc.rt_format)。
    if (has_ps) {
        if (desc.rt_count > 0) {
            // strict validation: 過大値を silent 切り詰めると caller の想定と乖離するので reject
            if (desc.rt_count > 8) {
                return ACS_ERR(Render, 154,
                    "FDiligentPipeline: rt_count must be <= 8 (got too large)");
            }
            // 各 slot が valid format か検証 (Unknown は MRT で意味なし)
            for (u32 i = 0; i < desc.rt_count; ++i) {
                if (desc.rt_formats[i] == EFormat::Unknown) {
                    return ACS_ERR(Render, 155,
                        "FDiligentPipeline: rt_formats[i] must be set for all i < rt_count");
                }
            }
            gp.NumRenderTargets = static_cast<u8>(desc.rt_count);
            for (u32 i = 0; i < desc.rt_count; ++i) {
                gp.RTVFormats[i] = diligent_detail::ToDiligent(desc.rt_formats[i]);
            }
        } else {
            gp.NumRenderTargets = 1u;
            gp.RTVFormats[0]    = diligent_detail::ToDiligent(desc.rt_format);
        }
    } else {
        gp.NumRenderTargets = 0u;
        gp.RTVFormats[0]    = Diligent::TEX_FORMAT_UNKNOWN;
    }
    gp.DSVFormat                    = (desc.depth_format == EFormat::Unknown)
                                       ? Diligent::TEX_FORMAT_UNKNOWN
                                       : diligent_detail::ToDiligent(desc.depth_format);
    gp.PrimitiveTopology            = diligent_detail::ToDiligent(desc.topology);
    gp.RasterizerDesc.CullMode      = diligent_detail::ToDiligent(desc.cull_mode);
    gp.RasterizerDesc.FillMode      = Diligent::FILL_MODE_SOLID;
    // 経験的に: 色 pass (PS あり) で `true`、depth-only (PS なし) で `false`
    // にすると ACS sample 群 (HelloLights / Bloom / Shadows / FSky / Mesh ...)
    // で正しい winding になる。Diligent の D3D12 backend が swap chain / RT
    // 種別で内部的に何か変換してると推測。
    // 注: HelloBloom の off-screen HDR RT (PS あり、has_ps=true) も `true`
    // で正常動作。off-screen color RT で誤動作する suspected ケースは現状
    // 未観測。
    gp.RasterizerDesc.FrontCounterClockwise = has_ps;
    gp.DepthStencilDesc.DepthEnable = desc.depth_test && desc.depth_format != EFormat::Unknown;
    gp.DepthStencilDesc.DepthWriteEnable = desc.depth_write;
    gp.DepthStencilDesc.DepthFunc   = Diligent::COMPARISON_FUNC_LESS_EQUAL;

    // ステンシル (マスク描画用)。FStencilDesc → Diligent DepthStencilStateDesc。
    {
        auto to_cmp = [](ECompareFunc f) -> Diligent::COMPARISON_FUNCTION {
            switch (f) {
                case ECompareFunc::Never:        return Diligent::COMPARISON_FUNC_NEVER;
                case ECompareFunc::Less:         return Diligent::COMPARISON_FUNC_LESS;
                case ECompareFunc::Equal:        return Diligent::COMPARISON_FUNC_EQUAL;
                case ECompareFunc::LessEqual:    return Diligent::COMPARISON_FUNC_LESS_EQUAL;
                case ECompareFunc::Greater:      return Diligent::COMPARISON_FUNC_GREATER;
                case ECompareFunc::NotEqual:     return Diligent::COMPARISON_FUNC_NOT_EQUAL;
                case ECompareFunc::GreaterEqual: return Diligent::COMPARISON_FUNC_GREATER_EQUAL;
                case ECompareFunc::Always:       return Diligent::COMPARISON_FUNC_ALWAYS;
            }
            return Diligent::COMPARISON_FUNC_ALWAYS;
        };
        auto to_op = [](EStencilOp o) -> Diligent::STENCIL_OP {
            switch (o) {
                case EStencilOp::Keep:     return Diligent::STENCIL_OP_KEEP;
                case EStencilOp::Zero:     return Diligent::STENCIL_OP_ZERO;
                case EStencilOp::Replace:  return Diligent::STENCIL_OP_REPLACE;
                case EStencilOp::IncrSat:  return Diligent::STENCIL_OP_INCR_SAT;
                case EStencilOp::DecrSat:  return Diligent::STENCIL_OP_DECR_SAT;
                case EStencilOp::Invert:   return Diligent::STENCIL_OP_INVERT;
                case EStencilOp::IncrWrap: return Diligent::STENCIL_OP_INCR_WRAP;
                case EStencilOp::DecrWrap: return Diligent::STENCIL_OP_DECR_WRAP;
            }
            return Diligent::STENCIL_OP_KEEP;
        };
        const FStencilDesc& st = desc.stencil;
        gp.DepthStencilDesc.StencilEnable    = st.enable;
        gp.DepthStencilDesc.StencilReadMask  = st.read_mask;
        gp.DepthStencilDesc.StencilWriteMask = st.write_mask;
        Diligent::StencilOpDesc face;
        face.StencilFunc        = to_cmp(st.func);
        face.StencilPassOp      = to_op(st.pass_op);
        face.StencilFailOp      = to_op(st.fail_op);
        face.StencilDepthFailOp = to_op(st.depth_fail_op);
        gp.DepthStencilDesc.FrontFace = face;
        gp.DepthStencilDesc.BackFace  = face;
    }

    diligent_detail::ApplyBlend(desc.blend_mode, gp.BlendDesc.RenderTargets[0]);
    gp.BlendDesc.RenderTargets[0].RenderTargetWriteMask = Diligent::COLOR_MASK_ALL;

    psoCI.pVS = vs->Native();
    psoCI.pPS = has_ps ? ps->Native() : nullptr;

    // 入力レイアウト（最大 8 要素）
    // Diligent の InputIndex は D3D12 では HLSL semantic index と一致しないと
    // PSO 作成が失敗する (例: POSITION0/NORMAL0/TEXCOORD0 → 全て 0)。
    // ACS は ACS::InputElement::semantic_index を持つのでそれを使う。
    Diligent::LayoutElement layout[8]{};
    for (u32 i = 0; i < desc.layout_count && i < 8; ++i) {
        const auto& e = desc.layout[i];
        layout[i].InputIndex      = e.semantic_index;
        layout[i].BufferSlot      = 0;
        layout[i].NumComponents   = 0;
        layout[i].ValueType       = Diligent::VT_FLOAT32;
        layout[i].IsNormalized    = false;
        layout[i].RelativeOffset  = e.offset;
        // Stride = 1 頂点全体のサイズ。各 LayoutElement で同じ buffer slot (=0)
        // を共有する場合、すべて同じ値を入れる。0 のままだと Diligent は要素
        // の sum で auto 計算するが、MeshVertex に tangent/color 等の余分が
        // ある場合に実 stride と食い違い、後続頂点が誤 offset で読まれて
        // ジオメトリが破壊される。
        layout[i].Stride          = desc.vertex_stride;
        layout[i].HLSLSemantic    = e.semantic_name ? e.semantic_name : "POSITION";

        switch (e.format) {
            case EFormat::R32G32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 2; break;
            case EFormat::R32G32B32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 3; break;
            case EFormat::R32G32B32A32_Float:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 4; break;
            case EFormat::R8G8B8A8_UNorm:
                layout[i].ValueType = Diligent::VT_UINT8;   layout[i].NumComponents = 4;
                layout[i].IsNormalized = true; break;
            case EFormat::R8G8B8A8_UInt:
                layout[i].ValueType = Diligent::VT_UINT8;   layout[i].NumComponents = 4;
                layout[i].IsNormalized = false; break;
            default:
                layout[i].ValueType = Diligent::VT_FLOAT32; layout[i].NumComponents = 4; break;
        }
    }
    gp.InputLayout.LayoutElements = desc.layout_count > 0 ? layout : nullptr;
    gp.InputLayout.NumElements    = desc.layout_count;

    // === Resource layout ===
    // 各 texture に対応する static sampler を `<TextureName>m_Sampler` の名前で
    // ImmutableSampler 登録する（Diligent の CombinedSamplerSuffix と一致）。
    psoCI.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;

    constexpr u32 kMaxStaticSamplers = 16;
    Diligent::ImmutableSamplerDesc samplers[kMaxStaticSamplers]{};
    const u32 ns = desc.static_sampler_count > kMaxStaticSamplers
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
        if (s.comparison) {   // HW 比較 PCF サンプラ (シャドウ SampleCmpLevelZero 用、lit ⇔ cmp ≤ stored)
            samplers[i].Desc.MinFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
            samplers[i].Desc.MagFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
            samplers[i].Desc.MipFilter = Diligent::FILTER_TYPE_COMPARISON_LINEAR;
            samplers[i].Desc.ComparisonFunc = Diligent::COMPARISON_FUNC_LESS_EQUAL;
        }
    }
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = ns > 0 ? samplers : nullptr;
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = ns;

    dev->CreateGraphicsPipelineState(psoCI, &m_Pso);
    if (!m_Pso) {
        return ACS_ERR(Render, 152, "CreateGraphicsPipelineState failed");
    }

    m_Pso->CreateShaderResourceBinding(&m_Srb, true);
    if (!m_Srb) {
        Reset();
        return ACS_ERR(Render, 153, "CreateShaderResourceBinding failed");
    }

    return Ok();
}

namespace {
const char* const kUavFallback[16] = {
    "u0","u1","u2","u3","u4","u5","u6","u7",
    "u8","u9","u10","u11","u12","u13","u14","u15"
};
} // namespace

const char* FDiligentPipeline::UavName(u32 slot) const noexcept {
    if (slot >= kMaxResourceSlots) return "u_invalid";
    return m_UavNames[slot] ? m_UavNames[slot] : kUavFallback[slot];
}

TResult<void> FDiligentPipeline::InitCompute(FDiligentDevice& device,
                                            const FComputePipelineDesc& desc) noexcept {
    Reset();
    m_Device    = &device;
    m_IsCompute = true;
    m_CbSlots   = desc.cbuffer_slots;
    m_TexSlots  = desc.srv_slots;
    m_UavSlots  = desc.uav_slots;
    for (u32 i = 0; i < kMaxResourceSlots; ++i) {
        m_CbNames[i]  = desc.cbuffer_names[i];
        m_TexNames[i] = desc.srv_names[i];
        m_UavNames[i] = desc.uav_names[i];
    }

    auto* dev = device.RenderDev();
    if (!dev) return ACS_ERR(Render, 156, "FDiligentPipeline(compute): device not initialized");
    auto* cs = static_cast<FDiligentShader*>(desc.cs);
    if (!cs || !cs->Native()) return ACS_ERR(Render, 157, "FDiligentPipeline(compute): CS missing");

    Diligent::ComputePipelineStateCreateInfo psoCI;
    psoCI.PSODesc.Name         = "ACS_ComputePSO";
    psoCI.PSODesc.PipelineType = Diligent::PIPELINE_TYPE_COMPUTE;
    psoCI.pCS                  = cs->Native();
    psoCI.PSODesc.ResourceLayout.DefaultVariableType =
        Diligent::SHADER_RESOURCE_VARIABLE_TYPE_DYNAMIC;

    // Compute shaders deliberately disable Diligent's combined-sampler mode so
    // UAV names are left untouched.  In that mode ImmutableSamplerDesc must
    // name the actual HLSL SamplerState, not its texture.  ACS shaders pair
    // `Texture* foo` with `SamplerState foo_sampler`.
    constexpr u32 kMaxStaticSamplers = 16;
    Diligent::ImmutableSamplerDesc samplers[kMaxStaticSamplers]{};
    char samplerNames[kMaxStaticSamplers][96]{};
    const u32 ns = desc.static_sampler_count > kMaxStaticSamplers
                 ? kMaxStaticSamplers : desc.static_sampler_count;
    for (u32 i = 0; i < ns; ++i) {
        const auto& s = desc.static_samplers[i];
        const char* textureName =
            desc.srv_names[i] ? desc.srv_names[i] : kTexFallback[i];
        constexpr char suffix[] = "_sampler";
        const size_t nameLength = std::strlen(textureName);
        constexpr size_t suffixLength = sizeof(suffix) - 1u;
        if (nameLength + suffixLength >= sizeof(samplerNames[i])) {
            return ACS_ERR(Render, 160,
                           "FDiligentPipeline(compute): static sampler name too long");
        }
        std::memcpy(samplerNames[i], textureName, nameLength);
        std::memcpy(samplerNames[i] + nameLength, suffix, sizeof(suffix));
        samplers[i].SamplerOrTextureName = samplerNames[i];
        samplers[i].ShaderStages   = Diligent::SHADER_TYPE_COMPUTE;
        samplers[i].Desc.Name      = "ACS_CsStaticSampler";
        samplers[i].Desc.MinFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.MagFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.MipFilter = diligent_detail::ToDiligentFilter(s.filter);
        samplers[i].Desc.AddressU  = diligent_detail::ToDiligentAddress(s.address_u);
        samplers[i].Desc.AddressV  = diligent_detail::ToDiligentAddress(s.address_v);
        samplers[i].Desc.AddressW  = diligent_detail::ToDiligentAddress(s.address_w);
    }
    psoCI.PSODesc.ResourceLayout.ImmutableSamplers    = ns > 0 ? samplers : nullptr;
    psoCI.PSODesc.ResourceLayout.NumImmutableSamplers = ns;

    dev->CreateComputePipelineState(psoCI, &m_Pso);
    if (!m_Pso) return ACS_ERR(Render, 158, "CreateComputePipelineState failed");
    m_Pso->CreateShaderResourceBinding(&m_Srb, true);
    if (!m_Srb) {
        Reset();
        return ACS_ERR(Render, 159, "CreateShaderResourceBinding(compute) failed");
    }
    return Ok();
}

} // namespace acs

#endif // WITH_RENDER_DILIGENT
