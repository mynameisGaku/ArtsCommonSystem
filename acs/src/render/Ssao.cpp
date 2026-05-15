// Screen-Space Ambient Occlusion 実装 (Phase 34j、HBAO-lite)
#include "render/Ssao.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

const char* kSsaoHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsaoCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;              // xyz = world pos
    float4   params;           // x=intensity, y=radius, z=texel_w, w=texel_h
};

Texture2D    scene_depth : register(t0);
SamplerState scene_depth_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

// uv + depth → world pos
float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, inv_view_proj);
    return wp.xyz / max(wp.w, 1e-6);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (depth >= 0.9999) return float4(1, 1, 1, 1);   // sky → no AO (visibility = 1)

    float3 wp  = ReconstructWorldPos(v.uv, depth);
    // depth-derivative 由来の screen-space normal
    float3 dpx = ddx(wp);
    float3 dpy = ddy(wp);
    float3 N = normalize(cross(dpy, dpx));
    // camera 側を向くように補正
    float3 V = normalize(eye.xyz - wp);
    if (dot(N, V) < 0.0) N = -N;

    // HBAO-lite: 6 方向 × 6 step、view space で sample (Phase 34j-3: noise 抑制で品質向上)
    const int   kDirs  = 6;
    const int   kSteps = 6;
    const float kRadius = max(params.y, 0.05);
    const float kIntensity = params.x;

    // Per-pixel jitter で banding 抑制 (Interleaved Gradient Noise、Jorge Jimenez)。
    // 2 種類用意してそれぞれ direction / step に与え、互いに無相関にしてノイズ模様
    // を「斑」ではなく「微粒」にする。
    float jitter1 = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715))));
    float jitter2 = frac(31.4159265 * frac(dot(v.pos.xy, float2(0.04711057, 0.01183715))));

    // Phase 34j-5: horizon-based occlusion (HBAO 本来の形)。
    // 各 slice 方向で「最も遮蔽の強いサンプル (= horizon)」を 1 つ取り、全 slice
    // で平均する。素朴な「全サンプルの ndot 平均」だと、近接の強い遮蔽が遠方の
    // 弱いサンプルで薄まってしまうが、slice ごとに max を取ることで contact
    // shadow がシャープに残る。物理的にも「1 方向で遮蔽されていればその方向の
    // 光は来ない」= horizon の考え方に沿う。
    float slice_sum = 0.0;
    [unroll]
    for (int d = 0; d < kDirs; ++d) {
        float angle = (float(d) + jitter1) * (3.14159 / float(kDirs));
        float2 dir_uv = float2(cos(angle), sin(angle));
        // 半径を screen-space pixel に変換: 大雑把に depth に比例
        // (透視投影で近い物体は radius 大きく、遠い物体は小さく)
        float screen_radius = kRadius * 0.5 / max(depth, 0.01);
        float horizon = 0.0;       // この slice の最大遮蔽量
        [unroll]
        for (int s = 1; s <= kSteps; ++s) {
            float t = (float(s) + jitter2 * 0.5) / float(kSteps);
            float2 off = dir_uv * screen_radius * t;
            float2 sample_uv = v.uv + off;
            if (sample_uv.x < 0 || sample_uv.x > 1 || sample_uv.y < 0 || sample_uv.y > 1) continue;
            float sample_d = scene_depth.SampleLevel(scene_depth_sampler, sample_uv, 0).r;
            if (sample_d >= 0.9999) continue;
            float3 sample_wp = ReconstructWorldPos(sample_uv, sample_d);
            float3 delta = sample_wp - wp;
            float  dist  = length(delta);
            if (dist < 1e-4 || dist > kRadius) continue;
            float3 dir = delta / dist;
            // sample point が surface normal の上にあれば occluder
            float ndot = max(dot(N, dir), 0.0);
            // 距離 falloff (近いほど影響大、kRadius で 0)
            float falloff = 1.0 - smoothstep(kRadius * 0.5, kRadius, dist);
            horizon = max(horizon, ndot * falloff);    // slice 内は max (= horizon)
        }
        slice_sum += horizon;
    }

    // 全 slice の horizon 平均を遮蔽量とする。horizon-based は sum-average より
    // 遮蔽が強く出るので、呼び出し側 (HelloIbl) の intensity を下げて調整する。
    float ao = saturate(1.0 - (slice_sum / float(kDirs)) * kIntensity);
    return float4(ao, ao, ao, 1.0);
}
)";

// Phase 34j-4: depth-aware bilateral blur。SSAO raw のノイズを、depth 不連続
// (シルエットエッジ) を跨がないように平滑化する。5x5 kernel、spatial gaussian ×
// depth similarity weight。
const char* kSsaoBlurHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsaoCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;     // z=texel_w, w=texel_h
};

Texture2D    ssao_raw    : register(t0);
Texture2D    scene_depth : register(t1);
SamplerState ssao_raw_sampler    : register(s0);
SamplerState scene_depth_sampler : register(s1);

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
    float2 tx = float2(params.z, params.w);
    float center_d = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;

    // 5x5 depth-aware bilateral blur
    float sum = 0.0, wsum = 0.0;
    const int kR = 2;
    [unroll]
    for (int dy = -kR; dy <= kR; ++dy) {
        [unroll]
        for (int dx = -kR; dx <= kR; ++dx) {
            float2 uv = v.uv + float2(dx, dy) * tx;
            float ao = ssao_raw.SampleLevel(ssao_raw_sampler, uv, 0).r;
            float d  = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
            // spatial gaussian (sigma ~= 2 px)
            float sw = exp(-float(dx*dx + dy*dy) / 8.0);
            // depth similarity (edge stopping): depth 差が ~0.005 で weight 半減
            float dd = d - center_d;
            float dw = exp(-dd * dd * 25000.0);
            float w  = sw * dw;
            sum  += ao * w;
            wsum += w;
        }
    }
    float result = (wsum > 1e-5)
                   ? sum / wsum
                   : ssao_raw.SampleLevel(ssao_raw_sampler, v.uv, 0).r;
    return float4(result, result, result, 1.0);
}
)";

struct SsaoCBLayout {
    Mat4 view_proj;
    Mat4 inv_view_proj;
    Vec4 eye;
    Vec4 params;
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> Ssao::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    _device = &device;
    _width = width;
    _height = height;

    if (auto r = CreateOutputRT(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size = CBSize<SsaoCBLayout>();
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _cb = Move(r.Value());

    return Ok();
}

Result<void> Ssao::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    _output.Reset();
    _blur_output.Reset();
    TextureDesc td{};
    td.width  = width;
    td.height = height;
    // RGBA8 だが .r のみ使用 (R8_UNorm を Format に追加するより既存型流用が小コスト)
    td.format = Format::R8G8B8A8_UNorm;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    _output = Move(r.Value());

    // Phase 34j-4: blur 後の RT (同フォーマット / 同サイズ)
    auto br = CreateRhiTexture(device, td);
    if (br.IsErr()) return Err<void>(br.Error());
    _blur_output = Move(br.Value());
    return Ok();
}

Result<void> Ssao::CreatePipeline(IRhiDevice& device) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kSsaoHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Ssao.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kSsaoHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Ssao.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _ps = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = Format::R8G8B8A8_UNorm;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 1;
    pd.cbuffer_names[0] = "SsaoCB";
    pd.texture_names[0] = "scene_depth";
    pd.static_sampler_count = 1;
    pd.static_samplers[0].filter    = SamplerFilter::Point;       // depth は離散値 → Point
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    // Phase 34j-4: blur pipeline (ssao_raw + scene_depth → blurred)。
    // VS は SSAO 本体と同じ fullscreen-triangle なので _vs を再利用する
    // (kSsaoHLSL と kSsaoBlurHLSL の VSMain は同一構造)。
    ShaderDesc bps_d{};
    bps_d.stage = ShaderStage::Pixel;
    bps_d.hlsl_source = kSsaoBlurHLSL;
    bps_d.entry_point = "PSMain";
    bps_d.debug_name  = "SsaoBlur.PS";
    if (auto r = CreateRhiShader(device, bps_d); r.IsErr()) return Err<void>(r.Error());
    else _blur_ps = Move(r.Value());

    PipelineDesc bpd{};
    bpd.vs            = _vs.Get();
    bpd.ps            = _blur_ps.Get();
    bpd.topology      = PrimitiveTopology::TriangleList;
    bpd.rt_format     = Format::R8G8B8A8_UNorm;
    bpd.depth_format  = Format::Unknown;
    bpd.depth_test    = false;
    bpd.depth_write   = false;
    bpd.cull_mode     = CullMode::None;
    bpd.blend_mode    = BlendMode::Opaque;
    bpd.cbuffer_slots = 1;
    bpd.texture_slots = 2;
    bpd.cbuffer_names[0] = "SsaoCB";
    bpd.texture_names[0] = "ssao_raw";
    bpd.texture_names[1] = "scene_depth";
    bpd.static_sampler_count = 2;
    bpd.static_samplers[0].filter    = SamplerFilter::Linear;     // AO は linear で補間
    bpd.static_samplers[0].address_u = SamplerAddress::Clamp;
    bpd.static_samplers[0].address_v = SamplerAddress::Clamp;
    bpd.static_samplers[1].filter    = SamplerFilter::Point;      // depth は離散値
    bpd.static_samplers[1].address_u = SamplerAddress::Clamp;
    bpd.static_samplers[1].address_v = SamplerAddress::Clamp;
    bpd.vertex_stride = 0;
    bpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, bpd); r.IsErr()) return Err<void>(r.Error());
    else _blur_pipeline = Move(r.Value());
    return Ok();
}

void Ssao::Shutdown() noexcept {
    _blur_pipeline.Reset();
    _pipeline.Reset();
    _cb.Reset();
    _blur_ps.Reset();
    _ps.Reset();
    _vs.Reset();
    _blur_output.Reset();
    _output.Reset();
    _device = nullptr;
}

Result<void> Ssao::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 330, "Ssao::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _width = width;
    _height = height;
    return CreateOutputRT(*_device, width, height);
}

void Ssao::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_depth,
                  const Mat4& view_proj, const Mat4& inv_view_proj,
                  Vec3 eye, f32 intensity, f32 radius) noexcept {
    if (!_output || !_blur_output || !_pipeline || !_blur_pipeline || !_cb) return;
    SsaoCBLayout data{};
    data.view_proj     = view_proj;
    data.inv_view_proj = inv_view_proj;
    data.eye           = Vec4{eye.x, eye.y, eye.z, 1};
    data.params        = Vec4{intensity, radius,
                               1.0f / static_cast<f32>(_width),
                               1.0f / static_cast<f32>(_height)};
    _cb->Update(&data, sizeof(data));

    // Pass 1: SSAO raw → _output
    cl.BeginRenderToTexture(*_output, ClearColor{1, 1, 1, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_output);

    // Pass 2 (Phase 34j-4): depth-aware bilateral blur → _blur_output
    cl.BeginRenderToTexture(*_blur_output, ClearColor{1, 1, 1, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_blur_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, *_output);       // SSAO raw
    cl.SetTexture(1, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_blur_output);
}

} // namespace acs
