// SPDX-License-Identifier: Apache-2.0
// DX12 グラフィックスパイプライン実装
#include "render/Dx12/Dx12Pipeline.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

namespace acs {

namespace {

// EPrimitiveTopology → DX12 トポロジ種別
D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3DTopologyType(EPrimitiveTopology t) noexcept {
    switch (t) {
        case EPrimitiveTopology::PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case EPrimitiveTopology::LineList:
        case EPrimitiveTopology::LineStrip:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case EPrimitiveTopology::TriangleList:
        case EPrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

// ECullMode → DX12
D3D12_CULL_MODE ToD3DCullMode(ECullMode m) noexcept {
    switch (m) {
        case ECullMode::None:  return D3D12_CULL_MODE_NONE;
        case ECullMode::Front: return D3D12_CULL_MODE_FRONT;
        case ECullMode::Back:  return D3D12_CULL_MODE_BACK;
    }
    return D3D12_CULL_MODE_NONE;
}

// ラスタライザのデフォルト設定
D3D12_RASTERIZER_DESC MakeRasterizer(ECullMode cull) noexcept {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = ToD3DCullMode(cull);
    r.FrontCounterClockwise = FALSE;
    r.DepthClipEnable = TRUE;
    r.MultisampleEnable = FALSE;
    r.AntialiasedLineEnable = FALSE;
    return r;
}

// ブレンド設定（不透明・α・加算）
D3D12_BLEND_DESC MakeBlend(EBlendMode mode) noexcept {
    D3D12_BLEND_DESC b{};
    b.AlphaToCoverageEnable = FALSE;
    b.IndependentBlendEnable = FALSE;
    auto& rt = b.RenderTarget[0];
    rt.LogicOpEnable = FALSE;
    rt.LogicOp = D3D12_LOGIC_OP_NOOP;
    rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    switch (mode) {
        case EBlendMode::Opaque:
            rt.BlendEnable = FALSE;
            rt.SrcBlend = D3D12_BLEND_ONE; rt.DestBlend = D3D12_BLEND_ZERO; rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ZERO; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        case EBlendMode::AlphaBlend:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA; rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
        case EBlendMode::Additive:
            rt.BlendEnable = TRUE;
            rt.SrcBlend = D3D12_BLEND_SRC_ALPHA; rt.DestBlend = D3D12_BLEND_ONE; rt.BlendOp = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha = D3D12_BLEND_ONE; rt.DestBlendAlpha = D3D12_BLEND_ONE; rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
            break;
    }
    // 残りのレンダーターゲットは独立ブレンドではないので 0 番をコピー
    for (int i = 1; i < 8; ++i) b.RenderTarget[i] = rt;
    return b;
}

D3D12_DEPTH_STENCIL_DESC MakeDepthStencil(bool enabled, bool write) noexcept {
    D3D12_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = enabled ? TRUE : FALSE;
    d.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    d.StencilEnable = FALSE;
    return d;
}

// ESamplerFilter → DX12 フィルタ
D3D12_FILTER ToD3DFilter(ESamplerFilter f) noexcept {
    switch (f) {
        case ESamplerFilter::Point:        return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case ESamplerFilter::Linear:       return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        case ESamplerFilter::Anisotropic:  return D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

// ESamplerAddress → DX12
D3D12_TEXTURE_ADDRESS_MODE ToD3DAddress(ESamplerAddress a) noexcept {
    switch (a) {
        case ESamplerAddress::Wrap:    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case ESamplerAddress::Mirror:  return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case ESamplerAddress::Clamp:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case ESamplerAddress::Border:  return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(const SamplerDesc& s, u32 reg) noexcept {
    D3D12_STATIC_SAMPLER_DESC d{};
    d.Filter = ToD3DFilter(s.filter);
    d.AddressU = ToD3DAddress(s.address_u);
    d.AddressV = ToD3DAddress(s.address_v);
    d.AddressW = ToD3DAddress(s.address_w);
    d.MipLODBias = 0.0f;
    d.MaxAnisotropy = (s.filter == ESamplerFilter::Anisotropic) ? s.max_anisotropy : 0;
    d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    d.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    d.MinLOD = s.min_lod;
    d.MaxLOD = s.max_lod;
    d.ShaderRegister = reg;
    d.RegisterSpace = 0;
    d.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return d;
}

} // namespace

Dx12Pipeline::~Dx12Pipeline() noexcept {
    ACS_SAFE_RELEASE(_pso);
    ACS_SAFE_RELEASE(_root_sig);
}

HrResult Dx12Pipeline::Init(Dx12Device& device, const FPipelineDesc& desc) noexcept {
    HrResult r{};
    _topology = desc.topology;
    _cbuffer_slots = desc.cbuffer_slots;
    _texture_slots = desc.texture_slots;

    if (!desc.vs) { r.hr = E_INVALIDARG; return r; }
    // ps は省略可（depth-only パイプラインの場合）

    // ===== ルートシグネチャを構築 =====
    // パラメータ: [N x root CBV (b0..)] + [M x descriptor table (1 SRV @ tN..)]
    constexpr u32 kMaxParams = 16;
    if (desc.cbuffer_slots + desc.texture_slots > kMaxParams) {
        r.hr = E_INVALIDARG; return r;
    }

    D3D12_ROOT_PARAMETER params[kMaxParams]{};
    D3D12_DESCRIPTOR_RANGE ranges[kMaxParams]{};   // テクスチャ用、各 1 entry
    u32 param_count = 0;

    for (u32 i = 0; i < desc.cbuffer_slots; ++i) {
        auto& p = params[param_count++];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    for (u32 i = 0; i < desc.texture_slots; ++i) {
        auto& rng = ranges[param_count];
        rng.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        rng.NumDescriptors = 1;
        rng.BaseShaderRegister = i;
        rng.RegisterSpace = 0;
        rng.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
        auto& p = params[param_count++];
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p.DescriptorTable.NumDescriptorRanges = 1;
        p.DescriptorTable.pDescriptorRanges = &rng;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }

    // 静的サンプラ
    D3D12_STATIC_SAMPLER_DESC samplers[4]{};
    u32 sampler_count = desc.static_sampler_count > 4 ? 4 : desc.static_sampler_count;
    for (u32 i = 0; i < sampler_count; ++i) {
        samplers[i] = MakeStaticSampler(desc.static_samplers[i], i);
    }

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = param_count;
    rsd.pParameters = param_count > 0 ? params : nullptr;
    rsd.NumStaticSamplers = sampler_count;
    rsd.pStaticSamplers = sampler_count > 0 ? samplers : nullptr;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    r.hr = ::D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1,
                                         &sig_blob, &err_blob);
    if (r.IsErr()) {
        if (err_blob) {
            ACS_LOG_ERROR("RootSignature serialize: %s",
                          static_cast<const char*>(err_blob->GetBufferPointer()));
            err_blob->Release();
        }
        return r;
    }
    r.hr = device.D3DDevice()->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        IID_PPV_ARGS(&_root_sig));
    sig_blob->Release();
    if (err_blob) err_blob->Release();
    if (r.IsErr()) return r;

    // 入力レイアウトを DX12 形式に変換
    D3D12_INPUT_ELEMENT_DESC ie[8]{};
    for (u32 i = 0; i < desc.layout_count && i < 8; ++i) {
        ie[i].SemanticName = desc.layout[i].semantic_name;
        ie[i].SemanticIndex = desc.layout[i].semantic_index;
        ie[i].Format = ToDxgiFormat(desc.layout[i].format);
        ie[i].InputSlot = 0;
        ie[i].AlignedByteOffset = desc.layout[i].offset;
        ie[i].InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        ie[i].InstanceDataStepRate = 0;
    }

    // PSO 記述
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = _root_sig;
    pd.VS.pShaderBytecode = desc.vs->Bytecode();
    pd.VS.BytecodeLength  = desc.vs->BytecodeSize();
    if (desc.ps) {
        pd.PS.pShaderBytecode = desc.ps->Bytecode();
        pd.PS.BytecodeLength  = desc.ps->BytecodeSize();
    }
    // depth-only: PS なし
    pd.RasterizerState   = MakeRasterizer(desc.cull_mode);
    pd.BlendState        = MakeBlend(desc.blend_mode);
    pd.DepthStencilState = MakeDepthStencil(desc.depth_test && desc.depth_format != EFormat::Unknown,
                                            desc.depth_write);
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = ToD3DTopologyType(desc.topology);
    if (desc.ps) {
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0]    = ToDxgiFormat(desc.rt_format);
    } else {
        pd.NumRenderTargets = 0;          // depth-only
    }
    pd.DSVFormat = ToDxgiFormat(desc.depth_format);
    pd.SampleDesc.Count = 1;
    pd.InputLayout.pInputElementDescs = desc.layout_count > 0 ? ie : nullptr;
    pd.InputLayout.NumElements = desc.layout_count;

    r.hr = device.D3DDevice()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&_pso));
    return r;
}

// ファクトリ
#if !WITH_RENDER_DILIGENT
TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                  const FPipelineDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 50, "CreateRhiPipeline: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto p = MakeUnique<Dx12Pipeline>();
    HrResult r = p->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 51, "Dx12Pipeline::Init failed", static_cast<u32>(r.hr));
    TUniquePtr<IRhiPipeline> base(p.Release(), p.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(base));
}
#endif

} // namespace acs
