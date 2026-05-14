// Screen-Space Reflection 実装 (Phase 34e)
#include "render/Ssr.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

const char* kSsrHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsrCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;            // xyz = world pos
    float4   params;         // x=intensity, y=max_ray_dist, z=step_count, w=thickness
};

Texture2D    scene_color : register(t0);
Texture2D    scene_depth : register(t1);
SamplerState scene_color_sampler : register(s0);
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

// uv [0,1] + depth [0,1] → world pos
float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, inv_view_proj);
    return wp.xyz / max(wp.w, 1e-6);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (depth >= 0.9999) return float4(0, 0, 0, 0);   // sky pixel、反射なし

    float3 wp = ReconstructWorldPos(v.uv, depth);
    float3 V  = normalize(eye.xyz - wp);
    // depth derivatives 由来の screen-space normal
    float3 dpx = ddx(wp);
    float3 dpy = ddy(wp);
    float3 N = normalize(cross(dpy, dpx));
    // facing 補正 (depth-derived normal が反対向くケース)
    if (dot(N, V) < 0.0) N = -N;
    float3 R = reflect(-V, N);
    if (dot(R, V) < -0.95) return float4(0, 0, 0, 0); // 真後ろ反射は無視

    // Ray march (world space で前進、screen に投影)
    // M2 fix: 最終 step が thickness より長くなって near-miss を見逃す問題を回避するため
    // step 数を 48 に増やし、加速率を 0.015 に下げる (合計距離≒max_ray_dist の 1.45 倍)。
    const int   kSteps   = 48;
    const float kStepLen = max(params.y, 0.5) / float(kSteps);   // max_ray_dist / N
    const float thickness = max(params.w, 0.05);
    float3 ray_pos = wp + N * 0.02 + R * 0.02;    // 起点 offset で self-hit 回避
    [loop]
    for (int i = 0; i < kSteps; ++i) {
        ray_pos += R * (kStepLen * (1.0 + float(i) * 0.015));  // 緩めの accelerating step
        float4 clip = mul(float4(ray_pos, 1.0), view_proj);
        if (clip.w <= 0.0) break;                              // 背面
        clip.xyz /= clip.w;
        clip.y = -clip.y;
        if (clip.x < -1.0 || clip.x > 1.0 || clip.y < -1.0 || clip.y > 1.0) break;
        float2 ray_uv = clip.xy * 0.5 + 0.5;
        float scene_d = scene_depth.SampleLevel(scene_depth_sampler, ray_uv, 0).r;
        float ray_d   = clip.z;
        // 衝突判定: ray の depth がシーン depth より奥にある (thickness 以内)
        if (ray_d > scene_d && ray_d - scene_d < thickness) {
            // sky をヒットは skip
            if (scene_d >= 0.9999) break;
            float3 hit_color = scene_color.SampleLevel(scene_color_sampler, ray_uv, 0).rgb;
            // 端のフェードアウト (rect から離れるほど薄める)
            float2 dist2edge = min(ray_uv, 1.0 - ray_uv);
            float edge_fade = saturate(min(dist2edge.x, dist2edge.y) * 8.0);
            return float4(hit_color * params.x * edge_fade, 1.0);
        }
    }
    return float4(0, 0, 0, 0);
}
)";

struct SsrCBLayout {
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

Result<void> Ssr::Init(IRhiDevice& device, Format hdr_format, u32 width, u32 height) noexcept {
    _device = &device;
    _hdr_format = hdr_format;
    _width = width;
    _height = height;

    if (auto r = CreateOutputRT(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size = CBSize<SsrCBLayout>();
    cbd.usage = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _cb = Move(r.Value());

    return Ok();
}

Result<void> Ssr::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    _output.Reset();
    TextureDesc td{};
    td.width  = width;
    td.height = height;
    td.format = _hdr_format;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    _output = Move(r.Value());
    return Ok();
}

Result<void> Ssr::CreatePipeline(IRhiDevice& device) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kSsrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Ssr.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kSsrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Ssr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _ps = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = _hdr_format;
    pd.depth_format  = Format::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = CullMode::None;
    pd.blend_mode    = BlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 2;
    pd.cbuffer_names[0] = "SsrCB";
    pd.texture_names[0] = "scene_color";
    pd.texture_names[1] = "scene_depth";
    pd.static_sampler_count = 2;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[1].filter    = SamplerFilter::Point;
    pd.static_samplers[1].address_u = SamplerAddress::Clamp;
    pd.static_samplers[1].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());
    return Ok();
}

void Ssr::Shutdown() noexcept {
    _pipeline.Reset();
    _cb.Reset();
    _ps.Reset();
    _vs.Reset();
    _output.Reset();
    _device = nullptr;
}

Result<void> Ssr::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 320, "Ssr::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _width = width;
    _height = height;
    return CreateOutputRT(*_device, width, height);
}

void Ssr::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_color, IRhiTexture& scene_depth,
                  const Mat4& view_proj, const Mat4& inv_view_proj,
                  Vec3 eye, f32 intensity) noexcept {
    if (!_output || !_pipeline || !_cb) return;
    SsrCBLayout data{};
    data.view_proj     = view_proj;
    data.inv_view_proj = inv_view_proj;
    data.eye           = Vec4{eye.x, eye.y, eye.z, 1};
    data.params        = Vec4{intensity, /*max_dist=*/12.0f, /*step_count=*/32.0f, /*thickness=*/0.4f};
    _cb->Update(&data, sizeof(data));

    cl.BeginRenderToTexture(*_output, ClearColor{0, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_color);
    cl.SetTexture(1, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_output);
}

} // namespace acs
