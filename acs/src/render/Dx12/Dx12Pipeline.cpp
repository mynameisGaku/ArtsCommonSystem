// SPDX-License-Identifier: Apache-2.0
// DX12 グラフィックスパイプライン実装
#include "render/Dx12/Dx12Pipeline.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

namespace acs {

namespace {

/**
 * EPrimitiveTopology を DX12 のトポロジ種別へ変換する。
 *
 * @param t 変換元のプリミティブトポロジ。
 * @return 対応する D3D12_PRIMITIVE_TOPOLOGY_TYPE (未対応値は UNDEFINED)。
 */
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

/**
 * ECullMode を DX12 のカリングモードへ変換する。
 *
 * @param m 変換元のカリングモード。
 * @return 対応する D3D12_CULL_MODE。
 */
D3D12_CULL_MODE ToD3DCullMode(ECullMode m) noexcept {
    switch (m) {
        case ECullMode::None:  return D3D12_CULL_MODE_NONE;
        case ECullMode::Front: return D3D12_CULL_MODE_FRONT;
        case ECullMode::Back:  return D3D12_CULL_MODE_BACK;
    }
    return D3D12_CULL_MODE_NONE;
}

/**
 * 既定設定のラスタライザ記述を構築する (solid 塗り、深度クリップ有効)。
 *
 * @param cull 適用するカリングモード。
 * @return 構築した D3D12_RASTERIZER_DESC。
 */
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

/**
 * ブレンドモードに応じた DX12 ブレンド記述を構築する。
 *
 * @details Opaque/AlphaBlend/Additive に対応し、独立ブレンドは無効 (RT0 を全 RT にコピー)。
 * @param mode 適用するブレンドモード。
 * @return 構築した D3D12_BLEND_DESC。
 */
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

/**
 * ECompareFunc を DX12 の比較関数へ変換する。
 *
 * @param f 変換元の比較関数。
 * @return 対応する D3D12_COMPARISON_FUNC。
 */
D3D12_COMPARISON_FUNC ToD3DCompare(ECompareFunc f) noexcept {
    switch (f) {
        case ECompareFunc::Never:        return D3D12_COMPARISON_FUNC_NEVER;
        case ECompareFunc::Less:         return D3D12_COMPARISON_FUNC_LESS;
        case ECompareFunc::Equal:        return D3D12_COMPARISON_FUNC_EQUAL;
        case ECompareFunc::LessEqual:    return D3D12_COMPARISON_FUNC_LESS_EQUAL;
        case ECompareFunc::Greater:      return D3D12_COMPARISON_FUNC_GREATER;
        case ECompareFunc::NotEqual:     return D3D12_COMPARISON_FUNC_NOT_EQUAL;
        case ECompareFunc::GreaterEqual: return D3D12_COMPARISON_FUNC_GREATER_EQUAL;
        case ECompareFunc::Always:       return D3D12_COMPARISON_FUNC_ALWAYS;
    }
    return D3D12_COMPARISON_FUNC_ALWAYS;
}

/**
 * EStencilOp を DX12 のステンシル操作へ変換する。
 *
 * @param o 変換元のステンシル操作。
 * @return 対応する D3D12_STENCIL_OP。
 */
D3D12_STENCIL_OP ToD3DStencilOp(EStencilOp o) noexcept {
    switch (o) {
        case EStencilOp::Keep:     return D3D12_STENCIL_OP_KEEP;
        case EStencilOp::Zero:     return D3D12_STENCIL_OP_ZERO;
        case EStencilOp::Replace:  return D3D12_STENCIL_OP_REPLACE;
        case EStencilOp::IncrSat:  return D3D12_STENCIL_OP_INCR_SAT;
        case EStencilOp::DecrSat:  return D3D12_STENCIL_OP_DECR_SAT;
        case EStencilOp::Invert:   return D3D12_STENCIL_OP_INVERT;
        case EStencilOp::IncrWrap: return D3D12_STENCIL_OP_INCR;
        case EStencilOp::DecrWrap: return D3D12_STENCIL_OP_DECR;
    }
    return D3D12_STENCIL_OP_KEEP;
}

/**
 * 深度・ステンシル記述を構築する。
 *
 * @details 深度比較は LESS_EQUAL 固定。2D 向けに前面・背面で同一のステンシル面を使う。
 * @param enabled 深度テストを有効にするなら true。
 * @param write 深度書き込みを有効にするなら true。
 * @param st ステンシルの有効/マスク/比較関数・各操作を指定する記述。
 * @return 構築した D3D12_DEPTH_STENCIL_DESC。
 */
D3D12_DEPTH_STENCIL_DESC MakeDepthStencil(bool enabled, bool write,
                                          const FStencilDesc& st) noexcept {
    D3D12_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = enabled ? TRUE : FALSE;
    d.DepthWriteMask = write ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    d.StencilEnable = st.enable ? TRUE : FALSE;
    d.StencilReadMask  = st.read_mask;
    d.StencilWriteMask = st.write_mask;
    D3D12_DEPTH_STENCILOP_DESC face{};
    face.StencilFunc        = ToD3DCompare(st.func);
    face.StencilPassOp      = ToD3DStencilOp(st.pass_op);
    face.StencilFailOp      = ToD3DStencilOp(st.fail_op);
    face.StencilDepthFailOp = ToD3DStencilOp(st.depth_fail_op);
    d.FrontFace = face;
    d.BackFace  = face;   // 2D は両面同一 (cull None)
    return d;
}

/**
 * ESamplerFilter を DX12 のフィルタへ変換する。
 *
 * @param f 変換元のサンプラフィルタ。
 * @return 対応する D3D12_FILTER。
 */
D3D12_FILTER ToD3DFilter(ESamplerFilter f) noexcept {
    switch (f) {
        case ESamplerFilter::Point:        return D3D12_FILTER_MIN_MAG_MIP_POINT;
        case ESamplerFilter::Linear:       return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        case ESamplerFilter::Anisotropic:  return D3D12_FILTER_ANISOTROPIC;
    }
    return D3D12_FILTER_MIN_MAG_MIP_LINEAR;
}

/**
 * ESamplerAddress を DX12 のテクスチャアドレスモードへ変換する。
 *
 * @param a 変換元のアドレスモード。
 * @return 対応する D3D12_TEXTURE_ADDRESS_MODE。
 */
D3D12_TEXTURE_ADDRESS_MODE ToD3DAddress(ESamplerAddress a) noexcept {
    switch (a) {
        case ESamplerAddress::Wrap:    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        case ESamplerAddress::Mirror:  return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case ESamplerAddress::Clamp:   return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case ESamplerAddress::Border:  return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    }
    return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
}

/**
 * SamplerDesc から DX12 の静的サンプラ記述を構築する。
 *
 * @details 異方性フィルタ時のみ max_anisotropy を反映し、ピクセルシェーダから可視にする。
 * @param s フィルタ・アドレスモード・LOD 範囲などを指定するサンプラ記述。
 * @param reg 割り当てるシェーダレジスタ番号 (s0..)。
 * @return 構築した D3D12_STATIC_SAMPLER_DESC。
 */
D3D12_STATIC_SAMPLER_DESC MakeStaticSampler(const SamplerDesc& s, u32 reg) noexcept {
    D3D12_STATIC_SAMPLER_DESC d{};
    d.Filter = ToD3DFilter(s.filter);
    d.AddressU = ToD3DAddress(s.address_u);
    d.AddressV = ToD3DAddress(s.address_v);
    d.AddressW = ToD3DAddress(s.address_w);
    d.MipLODBias = 0.0f;
    if (s.filter == ESamplerFilter::Anisotropic) {
        d.MaxAnisotropy = s.max_anisotropy < 1u ? 1u : (s.max_anisotropy > 16u ? 16u : s.max_anisotropy);
    } else {
        d.MaxAnisotropy = 1;
    }
    if (s.comparison) {   // HW 比較 PCF サンプラ (シャドウ SampleCmpLevelZero、lit ⇔ cmp ≤ stored)
        d.Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        d.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    } else {
        d.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    }
    d.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    d.MinLOD = s.min_lod;
    d.MaxLOD = s.max_lod;
    d.ShaderRegister = reg;
    d.RegisterSpace = 0;
    d.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return d;
}

} // namespace

/** PSO と RootSignature を解放する。 */
Dx12Pipeline::~Dx12Pipeline() noexcept {
    Reset();
}

void Dx12Pipeline::Reset() noexcept
{
    ACS_SAFE_RELEASE(m_Pso);
    ACS_SAFE_RELEASE(m_RootSig);
    m_Topology = EPrimitiveTopology::TriangleList;
    m_CbufferSlots = 0;
    m_TextureSlots = 0;
}

/** ルートシグネチャ・入力レイアウト・PSO を構築する (CreateRhiPipeline 経由で呼ばれる)。 */
HrResult Dx12Pipeline::Init(Dx12Device& device, const FPipelineDesc& desc) noexcept {
    HrResult r{};
    Reset();

    if (!device.D3DDevice() || !desc.vs || !desc.vs->Bytecode() || desc.vs->BytecodeSize() == 0 ||
        desc.vs->Stage() != EShaderStage::Vertex) {
        r.hr = E_INVALIDARG;
        return r;
    }
    if (desc.ps && (!desc.ps->Bytecode() || desc.ps->BytecodeSize() == 0 || desc.ps->Stage() != EShaderStage::Pixel)) {
        r.hr = E_INVALIDARG;
        return r;
    }
    if (desc.layout_count > 8 || desc.static_sampler_count > 16 || desc.cbuffer_slots > 16 || desc.texture_slots > 16 ||
        desc.cbuffer_slots > 16u - desc.texture_slots ||
        ToD3DTopologyType(desc.topology) == D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED) {
        r.hr = E_INVALIDARG;
        return r;
    }
    if (desc.ps && ToDxgiFormat(desc.rt_format) == DXGI_FORMAT_UNKNOWN) {
        r.hr = E_INVALIDARG;
        return r;
    }
    if (desc.depth_format != EFormat::Unknown && desc.depth_format != EFormat::D24_UNorm_S8_UInt &&
        desc.depth_format != EFormat::D32_Float) {
        r.hr = E_INVALIDARG;
        return r;
    }

    m_Topology = desc.topology;
    m_CbufferSlots = desc.cbuffer_slots;
    m_TextureSlots = desc.texture_slots;

    // ルートシグネチャを構築する。
    // パラメータ: [N x root CBV (b0..)] + [M x descriptor table (1 SRV @ tN..)]
    constexpr u32 kMaxParams = 16;
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

    // 静的サンプラ (FPipelineDesc.static_samplers は容量 16。FPbrShader は 10 個使うため 4 では不足だった)
    D3D12_STATIC_SAMPLER_DESC samplers[16]{};
    const u32 sampler_count = desc.static_sampler_count;
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
        ACS_SAFE_RELEASE(sig_blob);
        if (err_blob) {
            ACS_LOG_ERROR("RootSignature serialize: %s",
                          static_cast<const char*>(err_blob->GetBufferPointer()));
            err_blob->Release();
        }
        Reset();
        return r;
    }
    if (!sig_blob) {
        ACS_SAFE_RELEASE(err_blob);
        r.hr = E_FAIL;
        Reset();
        return r;
    }
    r.hr = device.D3DDevice()->CreateRootSignature(
        0, sig_blob->GetBufferPointer(), sig_blob->GetBufferSize(),
        IID_PPV_ARGS(&m_RootSig));
    sig_blob->Release();
    if (err_blob) err_blob->Release();
    if (r.IsErr() || !m_RootSig) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
        return r;
    }

    // 入力レイアウトを DX12 形式に変換
    D3D12_INPUT_ELEMENT_DESC ie[8]{};
    for (u32 i = 0; i < desc.layout_count; ++i) {
        if (!desc.layout[i].semantic_name || ToDxgiFormat(desc.layout[i].format) == DXGI_FORMAT_UNKNOWN) {
            r.hr = E_INVALIDARG;
            Reset();
            return r;
        }
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
    pd.pRootSignature = m_RootSig;
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
                                            desc.depth_write, desc.stencil);
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = ToD3DTopologyType(desc.topology);
    if (desc.ps) {
        pd.NumRenderTargets = 1;
        pd.RTVFormats[0]    = ToDxgiFormat(desc.rt_format);
    } else {
        pd.NumRenderTargets = 0;          // depth-only
    }
    pd.DSVFormat = ToDxgiFormat(desc.depth_format);
    pd.SampleDesc.Count = (desc.sample_count > 1) ? desc.sample_count : 1;   // MSAA RT 用 PSO
    pd.InputLayout.pInputElementDescs = desc.layout_count > 0 ? ie : nullptr;
    pd.InputLayout.NumElements = desc.layout_count;

    r.hr = device.D3DDevice()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_Pso));
    if (r.IsErr() || !m_Pso) {
        if (r.IsOk()) r.hr = E_FAIL;
        Reset();
    }
    return r;
}

#if !WITH_RENDER_DILIGENT
/**
 * DX12 用に IRhiPipeline を生成するファクトリ。
 *
 * @details
 * RTTI 無効のためバックエンド名で DX12 を判定し、Dx12Pipeline を構築・初期化して返す。
 * Diligent バックエンド有効時は別実装が提供される。
 * @param device 生成元のデバイス (DX12 でなければエラー)。
 * @param description 構築するパイプラインの記述。
 * @return 生成したパイプラインを保持する TResult、判定・初期化失敗ならエラー。
 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device, const FPipelineDesc& description) noexcept
{
    const char* const backend_name = device.BackendName();
    if (!backend_name ||
        !(backend_name[0] == 'D' && backend_name[1] == 'X' && backend_name[2] == '1' && backend_name[3] == '2')) {
        return ACS_ERR(Render, 50, "CreateRhiPipeline: device is not DX12");
    }

    auto pipeline = MakeUnique<Dx12Pipeline>();
    if (!pipeline) return ACS_ERR(Memory, 52, "Dx12Pipeline allocation failed");

    const HrResult result = pipeline->Init(static_cast<Dx12Device&>(device), description);
    if (result.IsErr()) {
        return ACS_ERR_OS(Render, 51, "Dx12Pipeline::Init failed", static_cast<u32>(result.hr));
    }

    TUniquePtr<IRhiPipeline> base(pipeline.Release(), pipeline.GetAllocator());
    return TResult<TUniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

/**
 * raw DX12 で未対応の compute パイプライン要求を明示的なエラーへ変換する。
 *
 * @details compute コマンド群は現在 Diligent バックエンドだけが実装している。
 * 未定義シンボルにせず、共通レンダラーが機能を安全にスキップできる契約を保つ。
 */
TResult<TUniquePtr<IRhiPipeline>> CreateRhiComputePipeline(IRhiDevice& device,
                                                           const FComputePipelineDesc& description) noexcept
{
    (void)description;
    const char* const backend_name = device.BackendName();
    if (!backend_name || backend_name[0] != 'D' || backend_name[1] != 'X' || backend_name[2] != '1' ||
        backend_name[3] != '2') {
        return ACS_ERR(Render, 52, "CreateRhiComputePipeline: device is not DX12");
    }

    return ACS_ERR(Render, 53, "CreateRhiComputePipeline: raw DX12 compute is not supported");
}
#endif

} // namespace acs
