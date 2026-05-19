// フルスクリーン texture コピー実装 (Phase 35-3b)
#include "render/Blit.h"
#include "foundation/Move.h"

namespace acs {

namespace {

// シンプルな fullscreen blit シェーダ。SV_VertexID で 3 頂点の fullscreen 三角形を
// 生成し、source texture を素 sample して出力する。頂点バッファ無しで Draw(3) で
// 描画できる (SSR / SSGI / PostProcess と同じパターン)。
const char* kBlitHLSL = R"(
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

float4 PSMain(VSOut v) : SV_TARGET {
    return src.SampleLevel(src_sampler, v.uv, 0);
}
)";

} // namespace

Result<void> Blit::Init(IRhiDevice& device, Format rt_format) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kBlitHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Blit.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    _vs = Move(vs_r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kBlitHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Blit.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    _ps = Move(ps_r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = Format::Unknown;       // depth 不使用
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;        // 3 頂点の fullscreen 三角形
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 1;
    pd.texture_names[0] = "src";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    // 頂点バッファ無し (SV_VertexID 駆動): vertex_stride=0, layout_count=0 (既定値)
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    _pipeline = Move(pl_r.Value());

    return Ok();
}

void Blit::Shutdown() noexcept {
    _pipeline.Reset();
    _ps.Reset();
    _vs.Reset();
}

void Blit::Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst) noexcept {
    if (!_pipeline) return;
    // 全 pixel が src で上書きされるので clear 不要 → load 版で開始する。
    // (本コミットで追加した BeginRenderToTextureLoad の自然な利用例)。
    cmd.BeginRenderToTextureLoad(dst, nullptr);
    cmd.SetPipeline(*_pipeline);
    cmd.SetTexture(0, src);
    cmd.Draw(3, 0);                          // fullscreen 三角形 (頂点バッファ無し)
    cmd.EndRenderToTexture(dst);
}

} // namespace acs
