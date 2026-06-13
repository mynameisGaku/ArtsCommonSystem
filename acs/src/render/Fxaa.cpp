// SPDX-License-Identifier: Apache-2.0
// FXAA フルスクリーンパス実装
#include "render/Fxaa.h"
#include "foundation/Move.h"

namespace acs {

namespace {

/**
 * FXAA シェーダの HLSL ソース。
 *
 * @details
 * FXAA 3.11 の簡易品質版 (中心 + 対角 4 近傍)。SV_VertexID で fullscreen 三角形を
 * 生成するので頂点バッファ不要 (FBlit と同じパターン)。入力サイズは GetDimensions で
 * 取得するため cbuffer も不要。低コントラスト画素は早期 return で素通しし、ベタ塗りや
 * 文字のにじみを防ぐ。
 */
const char* kFxaaHLSL = R"(
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

float Luma(float3 c) { return dot(c, float3(0.299, 0.587, 0.114)); }

float4 PSMain(VSOut v) : SV_TARGET {
    uint w, h;
    src.GetDimensions(w, h);
    float2 px = float2(1.0 / w, 1.0 / h);

    float3 rgbM  = src.SampleLevel(src_sampler, v.uv, 0).rgb;
    float3 rgbNW = src.SampleLevel(src_sampler, v.uv + px * float2(-1, -1), 0).rgb;
    float3 rgbNE = src.SampleLevel(src_sampler, v.uv + px * float2( 1, -1), 0).rgb;
    float3 rgbSW = src.SampleLevel(src_sampler, v.uv + px * float2(-1,  1), 0).rgb;
    float3 rgbSE = src.SampleLevel(src_sampler, v.uv + px * float2( 1,  1), 0).rgb;
    float lM  = Luma(rgbM);
    float lNW = Luma(rgbNW), lNE = Luma(rgbNE);
    float lSW = Luma(rgbSW), lSE = Luma(rgbSE);
    float lMin = min(lM, min(min(lNW, lNE), min(lSW, lSE)));
    float lMax = max(lM, max(max(lNW, lNE), max(lSW, lSE)));

    // 低コントラストはエッジ無しとみなして素通し (ぼけ防止 + 早期 return)
    if (lMax - lMin < max(0.0312, lMax * 0.125)) return float4(rgbM, 1.0);

    // 対角近傍の輝度勾配からエッジ接線方向を推定する
    float2 dir;
    dir.x = -((lNW + lNE) - (lSW + lSE));
    dir.y =  ((lNW + lSW) - (lNE + lSE));
    float dirReduce = max((lNW + lNE + lSW + lSE) * 0.25 * 0.125, 1.0 / 128.0);
    float rcpDirMin = 1.0 / (min(abs(dir.x), abs(dir.y)) + dirReduce);
    dir = clamp(dir * rcpDirMin, -8.0, 8.0) * px;

    // エッジに沿った 2 タップ (近距離) と 4 タップ (遠距離) のブレンド
    float3 rgbA = 0.5 * (src.SampleLevel(src_sampler, v.uv + dir * (1.0 / 3.0 - 0.5), 0).rgb
                       + src.SampleLevel(src_sampler, v.uv + dir * (2.0 / 3.0 - 0.5), 0).rgb);
    float3 rgbB = rgbA * 0.5 + 0.25 * (src.SampleLevel(src_sampler, v.uv + dir * -0.5, 0).rgb
                                     + src.SampleLevel(src_sampler, v.uv + dir *  0.5, 0).rgb);
    // 遠距離タップが近傍レンジを外れたらエッジ越え → 近距離側を採用
    float lB = Luma(rgbB);
    float3 col = (lB < lMin || lB > lMax) ? rgbA : rgbB;
    return float4(col, 1.0);
}
)";

} // namespace

/** FXAA 用 VS/PS をコンパイルし、rt_format に合わせた PSO を生成する。 */
TResult<void> FFxaa::Init(IRhiDevice& device, EFormat rt_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kFxaaHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FFxaa.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    m_Vs = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kFxaaHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FFxaa.PS";
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
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    m_Pipeline = Move(pl_r.Value());

    return Ok();
}

/** パイプラインとシェーダを解放する。 */
void FFxaa::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** 現在 bind 中のターゲットへ src を FXAA 解決しつつフルスクリーン三角形で描く。 */
void FFxaa::Apply(IRhiCommandList& cmd, IRhiTexture& src) noexcept {
    if (!m_Pipeline) return;
    cmd.SetPipeline(*m_Pipeline);
    cmd.SetTexture(0, src);
    cmd.Draw(3, 0);                          // fullscreen 三角形 (頂点バッファ無し)
}

} // namespace acs
