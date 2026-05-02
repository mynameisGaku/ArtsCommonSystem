// DX12 グラフィックスパイプライン実装
#include "render/Dx12/Dx12Pipeline.h"
#include "render/Dx12/Dx12Device.h"
#include "memory/UniquePtr.h"
#include "foundation/Log.h"

namespace acs {

namespace {

// PrimitiveTopology → DX12 トポロジ種別
D3D12_PRIMITIVE_TOPOLOGY_TYPE ToD3DTopologyType(PrimitiveTopology t) noexcept {
    switch (t) {
        case PrimitiveTopology::PointList:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case PrimitiveTopology::LineList:
        case PrimitiveTopology::LineStrip:     return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        case PrimitiveTopology::TriangleList:
        case PrimitiveTopology::TriangleStrip: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
    return D3D12_PRIMITIVE_TOPOLOGY_TYPE_UNDEFINED;
}

// ラスタライザのデフォルト設定（裏面カリング無効、塗りつぶし）
D3D12_RASTERIZER_DESC DefaultRasterizer() noexcept {
    D3D12_RASTERIZER_DESC r{};
    r.FillMode = D3D12_FILL_MODE_SOLID;
    r.CullMode = D3D12_CULL_MODE_NONE;
    r.FrontCounterClockwise = FALSE;
    r.DepthClipEnable = TRUE;
    r.MultisampleEnable = FALSE;
    r.AntialiasedLineEnable = FALSE;
    return r;
}

// ブレンド・深度のデフォルト設定（不透明、深度テスト無し）
D3D12_BLEND_DESC DefaultBlend() noexcept {
    D3D12_BLEND_DESC b{};
    b.AlphaToCoverageEnable = FALSE;
    b.IndependentBlendEnable = FALSE;
    for (auto& rt : b.RenderTarget) {
        rt.BlendEnable = FALSE;
        rt.LogicOpEnable = FALSE;
        rt.SrcBlend = D3D12_BLEND_ONE;
        rt.DestBlend = D3D12_BLEND_ZERO;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        rt.LogicOp = D3D12_LOGIC_OP_NOOP;
        rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    }
    return b;
}

D3D12_DEPTH_STENCIL_DESC DefaultDepthStencil(bool enabled) noexcept {
    D3D12_DEPTH_STENCIL_DESC d{};
    d.DepthEnable = enabled ? TRUE : FALSE;
    d.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    d.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    d.StencilEnable = FALSE;
    return d;
}

} // namespace

Dx12Pipeline::~Dx12Pipeline() noexcept {
    ACS_SAFE_RELEASE(_pso);
    ACS_SAFE_RELEASE(_root_sig);
}

HrResult Dx12Pipeline::Init(Dx12Device& device, const PipelineDesc& desc) noexcept {
    HrResult r{};
    _topology = desc.topology;

    if (!desc.vs || !desc.ps) { r.hr = E_INVALIDARG; return r; }

    // ルートシグネチャ作成（v1: 引数なしの空シグネチャ）
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 0;
    rsd.pParameters = nullptr;
    rsd.NumStaticSamplers = 0;
    rsd.pStaticSamplers = nullptr;
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

    // パイプラインステート記述
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = _root_sig;
    pd.VS.pShaderBytecode = desc.vs->Bytecode();
    pd.VS.BytecodeLength  = desc.vs->BytecodeSize();
    pd.PS.pShaderBytecode = desc.ps->Bytecode();
    pd.PS.BytecodeLength  = desc.ps->BytecodeSize();
    pd.RasterizerState  = DefaultRasterizer();
    pd.BlendState       = DefaultBlend();
    pd.DepthStencilState = DefaultDepthStencil(desc.depth_format != Format::Unknown);
    pd.SampleMask = UINT_MAX;
    pd.PrimitiveTopologyType = ToD3DTopologyType(desc.topology);
    pd.NumRenderTargets = 1;
    pd.RTVFormats[0] = ToDxgiFormat(desc.rt_format);
    pd.DSVFormat = ToDxgiFormat(desc.depth_format);
    pd.SampleDesc.Count = 1;
    pd.InputLayout.pInputElementDescs = ie;
    pd.InputLayout.NumElements = desc.layout_count;

    r.hr = device.D3DDevice()->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&_pso));
    return r;
}

// ファクトリ
Result<UniquePtr<IRhiPipeline>> CreateRhiPipeline(IRhiDevice& device,
                                                  const PipelineDesc& desc) noexcept {
    const char* bn = device.BackendName();
    if (!(bn[0] == 'D' && bn[1] == 'X' && bn[2] == '1' && bn[3] == '2'))
        return ACS_ERR(Render, 50, "CreateRhiPipeline: device is not DX12");
    Dx12Device* dxd = static_cast<Dx12Device*>(&device);
    auto p = MakeUnique<Dx12Pipeline>();
    HrResult r = p->Init(*dxd, desc);
    if (r.IsErr())
        return ACS_ERR_OS(Render, 51, "Dx12Pipeline::Init failed", static_cast<u32>(r.hr));
    UniquePtr<IRhiPipeline> base(p.Release(), p.GetAllocator());
    return Result<UniquePtr<IRhiPipeline>>(OkInit, Move(base));
}

} // namespace acs
