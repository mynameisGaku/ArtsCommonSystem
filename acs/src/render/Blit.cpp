// SPDX-License-Identifier: Apache-2.0
// フルスクリーン texture コピー実装
#include "render/Blit.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * fullscreen blit シェーダの HLSL ソース。
 *
 * @details
 * SV_VertexID で 3 頂点の fullscreen 三角形を生成し、source texture を素 sample して
 * 出力する。頂点バッファ無しで Draw(3) で描画できる (SSR / SSGI / FPostProcess と
 * 同じパターン)。
 */
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

/** ブリット用 VS/PS をコンパイルし、rt_format に合わせた PSO を生成する。 */
TResult<void> FBlit::Init(IRhiDevice& device, EFormat rt_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kBlitHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FBlit.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kBlitHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FBlit.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    m_Ps = Move(ps_r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = EFormat::Unknown;       // depth 不使用
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;        // 3 頂点の fullscreen 三角形
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 0;
    pd.texture_slots = 1;
    pd.texture_names[0] = "src";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    // 頂点バッファ無し (SV_VertexID 駆動): vertex_stride=0, layout_count=0 (既定値)
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** パイプラインとシェーダを解放する。 */
void FBlit::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** load 版の RT 開始でフルスクリーン三角形を描画し src を dst へ上書きコピーする。 */
void FBlit::Copy(IRhiCommandList& cmd, IRhiTexture& src, IRhiTexture& dst) noexcept {
    if (!m_Pipeline) return;
    // 全 pixel が src で上書きされるので clear 不要 → load 版で開始する。
    // (本コミットで追加した BeginRenderToTextureLoad の自然な利用例)。
    cmd.BeginRenderToTextureLoad(dst, nullptr);
    cmd.SetPipeline(*m_Pipeline);
    cmd.SetTexture(0, src);
    cmd.Draw(3, 0);                          // fullscreen 三角形 (頂点バッファ無し)
    cmd.EndRenderToTexture(dst);
}

} // namespace acs
