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

    // Ray march: world space で粗く前進して geometry を貫いた step を検出し、
    // そのあと binary search で交差点を絞り込む。粗 march だけだと hit 位置が
    // step 長ぶんずれ、反射が縦に滲んでガビガビになる (Phase 34e-fix)。
    const int   kSteps    = 48;
    const float kStepLen  = max(params.y, 0.5) / float(kSteps);   // max_ray_dist / N
    const float thickness = max(params.w, 0.05);

    float3 ray_pos  = wp + N * 0.02 + R * 0.02;   // 起点 offset で self-hit 回避
    float3 prev_pos = ray_pos;                    // 直前 step (geometry 手前のはず)
    bool   hit = false;
    [loop]
    for (int i = 0; i < kSteps; ++i) {
        prev_pos = ray_pos;
        ray_pos += R * (kStepLen * (1.0 + float(i) * 0.015));  // 緩めの accelerating step
        float4 clip = mul(float4(ray_pos, 1.0), view_proj);
        if (clip.w <= 0.0) break;                              // 背面
        clip.xyz /= clip.w;
        clip.y = -clip.y;
        if (clip.x < -1.0 || clip.x > 1.0 || clip.y < -1.0 || clip.y > 1.0) break;
        float2 ray_uv = clip.xy * 0.5 + 0.5;
        float scene_d = scene_depth.SampleLevel(scene_depth_sampler, ray_uv, 0).r;
        if (scene_d >= 0.9999) continue;                       // sky は貫通
        // 衝突判定: ray depth がシーン depth より奥 (thickness 以内)
        if (clip.z > scene_d && clip.z - scene_d < thickness) {
            hit = true;
            break;
        }
    }
    if (!hit) return float4(0, 0, 0, 0);

    // Binary search: prev_pos (geometry 手前) と ray_pos (奥) の間で交差点を
    // 二分探索する。8 回で step を 1/256 に絞れるので反射が鋭くなる。
    float3 lo = prev_pos;
    float3 hi = ray_pos;
    [unroll]
    for (int b = 0; b < 8; ++b) {
        float3 mid = (lo + hi) * 0.5;
        float4 mc  = mul(float4(mid, 1.0), view_proj);
        mc.xyz /= max(mc.w, 1e-6);
        mc.y = -mc.y;
        float2 mid_uv = mc.xy * 0.5 + 0.5;
        float  md = scene_depth.SampleLevel(scene_depth_sampler, mid_uv, 0).r;
        if (mc.z > md) hi = mid;     // mid は geometry より奥 → 交差は手前側
        else           lo = mid;    // mid は手前 → 交差は奥側
    }

    // 絞り込んだ交差点で scene color を sample
    float4 fc = mul(float4(hi, 1.0), view_proj);
    fc.xyz /= max(fc.w, 1e-6);
    fc.y = -fc.y;
    float2 final_uv  = fc.xy * 0.5 + 0.5;
    float3 hit_color = scene_color.SampleLevel(scene_color_sampler, final_uv, 0).rgb;
    // 端のフェードアウト (画面端へ近づくほど薄める)
    float2 dist2edge = min(final_uv, 1.0 - final_uv);
    float  edge_fade = saturate(min(dist2edge.x, dist2edge.y) * 8.0);
    return float4(hit_color * params.x * edge_fade, 1.0);
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
