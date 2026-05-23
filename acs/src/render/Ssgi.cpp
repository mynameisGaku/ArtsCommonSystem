// SPDX-License-Identifier: Apache-2.0
// Screen-Space Global Illumination 実装 (Phase 33c)
#include "render/Ssgi.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

const char* kSsgiHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsgiCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;       // xyz = world pos
    float4   params;    // x=intensity, y=max_distance, z=texel_w, w=texel_h
    float4x4 prev_view_proj;   // raw pass では未使用、CB レイアウト整合のため宣言
};

Texture2D    scene_color    : register(t0);
Texture2D    scene_depth    : register(t1);
Texture2D    normal_gbuffer : register(t2);   // world-space normal (Phase 34m/34o)
SamplerState scene_color_sampler    : register(s0);
SamplerState scene_depth_sampler    : register(s1);
SamplerState normal_gbuffer_sampler : register(s2);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

float3 ReconstructWorldPos(float2 uv, float depth) {
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, inv_view_proj);
    return wp.xyz / max(wp.w, 1e-6);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (depth >= 0.9999) return float4(0, 0, 0, 1);   // sky → no indirect light

    float3 wp = ReconstructWorldPos(v.uv, depth);
    // normal G-buffer から per-pixel world normal を sample (Phase 34o)。
    // 旧 cross(ddx,ddy) は 2x2 quad 単位で faceted になり、hemisphere の
    // ray 方向が段差状にずれて GI のサンプリングがブロック状になっていた。
    float3 N = normalize(normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz);
    float3 V = normalize(eye.xyz - wp);
    if (dot(N, V) < 0.0) N = -N;

    // hemisphere 内に 4 本 ray、各 ray を 8 step で march
    const int   kRays  = 4;
    const int   kSteps = 8;
    const float kMaxDist = max(params.y, 0.1);
    const float kIntensity = params.x;

    // ray direction: hemisphere over N (低エントロピー: 4 方向に固定し IGN で回転)
    float jitter1 = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715))));
    float jitter2 = frac(31.4159265 * frac(dot(v.pos.xy, float2(0.04711057, 0.01183715))));

    // tangent / bitangent (Frisvad 2012 orthonormal basis)
    float3 T;
    if (abs(N.y) > 0.999) {
        T = normalize(cross(N, float3(1, 0, 0)));
    } else {
        T = normalize(cross(N, float3(0, 1, 0)));
    }
    float3 B = cross(N, T);

    float3 gi_sum = float3(0, 0, 0);
    int    gi_cnt = 0;
    [unroll]
    for (int r = 0; r < kRays; ++r) {
        // hemisphere: phi の方は ray index で 4 等分、theta は jitter で散らす
        float phi = (float(r) + jitter1) * (6.28318 / float(kRays));
        float cos_theta = sqrt(max(jitter2 * 0.7 + 0.1, 1e-3));   // cos-weighted (上半球やや偏り)
        float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
        // local hemisphere dir (z = up = N)
        float3 ldir = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
        // local → world
        float3 ray_dir = T * ldir.x + B * ldir.y + N * ldir.z;

        // ray march (世界距離で進める; 画面外 / sky / 距離超過で終了)
        float3 ray_pos = wp + N * 0.02;     // start slightly off the surface (self-intersection guard)
        float step_len = kMaxDist / float(kSteps);
        bool hit = false;
        float3 hit_color = float3(0, 0, 0);
        [unroll]
        for (int s = 1; s <= kSteps; ++s) {
            ray_pos += ray_dir * step_len;
            // world → clip → ndc → uv
            float4 cp = mul(float4(ray_pos, 1.0), view_proj);
            if (cp.w < 1e-4) break;
            float3 ndc = cp.xyz / cp.w;
            if (any(abs(ndc.xy) > 1.0)) break;
            if (ndc.z < 0.0 || ndc.z > 1.0) break;
            float2 hit_uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
            float scene_d = scene_depth.SampleLevel(scene_depth_sampler, hit_uv, 0).r;
            if (scene_d >= 0.9999) continue;          // sky pixel、貫通
            // depth hit 判定: ray の現在 depth が scene depth より奥なら遮蔽
            if (ndc.z > scene_d + 0.0005) {
                hit_color = scene_color.SampleLevel(scene_color_sampler, hit_uv, 0).rgb;
                hit = true;
                break;
            }
        }
        if (hit) {
            // cos-weighted contribution (Lambert): N . ray_dir でフィルタ
            float ndot = max(dot(N, ray_dir), 0.0);
            gi_sum += hit_color * ndot;
            gi_cnt += 1;
        }
    }

    float3 gi = (gi_cnt > 0) ? gi_sum / float(kRays) : float3(0, 0, 0);
    gi *= kIntensity;
    return float4(gi, 1.0);
}
)";

// Phase 33c-2: depth-aware bilateral blur (RGB)。SSGI raw は 4 ray のみで
// 強ノイズなので、depth 不連続を跨がない 5x5 blur で平滑化する。
const char* kSsgiBlurHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsgiCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;     // z=texel_w, w=texel_h
    float4x4 prev_view_proj;   // blur pass では未使用、CB レイアウト整合のため宣言
};

Texture2D    ssgi_raw    : register(t0);
Texture2D    scene_depth : register(t1);
SamplerState ssgi_raw_sampler    : register(s0);
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

    float3 sum = float3(0, 0, 0);
    float  wsum = 0.0;
    const int kR = 2;
    [unroll]
    for (int dy = -kR; dy <= kR; ++dy) {
        [unroll]
        for (int dx = -kR; dx <= kR; ++dx) {
            float2 uv = v.uv + float2(dx, dy) * tx;
            float3 gi = ssgi_raw.SampleLevel(ssgi_raw_sampler, uv, 0).rgb;
            float  d  = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
            float sw = exp(-float(dx*dx + dy*dy) / 8.0);
            float dd = d - center_d;
            float dw = exp(-dd * dd * 25000.0);
            float w  = sw * dw;
            sum  += gi * w;
            wsum += w;
        }
    }
    float3 result = (wsum > 1e-5)
                    ? sum / wsum
                    : ssgi_raw.SampleLevel(ssgi_raw_sampler, v.uv, 0).rgb;
    return float4(result, 1.0);
}
)";

// Phase 33c-3: temporal accumulation。blur 済み SSGI を、前フレームの結果
// (history) と reprojection + neighborhood clamp して時間積分する。4 ray の
// ノイズが時間方向にも平均されて大幅に減る。TAA の resolve と同じ構造。
const char* kSsgiTemporalHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsgiCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;          // z=texel_w, w=texel_h
    float4x4 prev_view_proj;   // Phase 33c-3: reprojection 用
    float4   temporal_params;  // Phase 34f-3: x=motion_texture_mode
};

Texture2D    current_gi  : register(t0);   // blur 済み SSGI (今フレーム)
Texture2D    history_gi  : register(t1);   // 前フレームの temporal 結果
Texture2D    scene_depth : register(t2);   // Phase 34f-3: motion mode では motion vector
SamplerState current_gi_sampler  : register(s0);
SamplerState history_gi_sampler  : register(s1);
SamplerState scene_depth_sampler : register(s2);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

VSOut VSMain(uint id : SV_VertexID) {
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.uv  = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}

// camera motion 由来の history reproject (TAA ComputeMotionUv と同形)
float2 ReprojectUv(float2 uv) {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
    if (depth >= 0.9999) return uv;
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, inv_view_proj);
    wp.xyz /= max(wp.w, 1e-6);
    float4 pc = mul(float4(wp.xyz, 1.0), prev_view_proj);
    if (pc.w < 1e-4) return uv;
    float2 pn = pc.xy / pc.w;
    return float2(pn.x * 0.5 + 0.5, -pn.y * 0.5 + 0.5);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 cur = current_gi.SampleLevel(current_gi_sampler, v.uv, 0).rgb;

    // Phase 34f-3: motion texture モードなら scene_depth slot を motion vector
    // として再解釈し、動く mesh も含めて history を reproject する。
    // 非モードなら従来の camera-only depth reprojection。
    float2 huv;
    if (temporal_params.x >= 0.5) {
        float2 mv = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).rg;
        huv = v.uv + mv;
    } else {
        huv = ReprojectUv(v.uv);
    }
    if (any(huv < 0.0) || any(huv > 1.0)) huv = v.uv;   // 画面外は静的 fallback
    float3 hist = history_gi.SampleLevel(history_gi_sampler, huv, 0).rgb;

    // Neighborhood clamp (3x3 of current) で disocclusion ghost を抑える
    float2 tx = float2(params.z, params.w);
    float3 nmin = cur, nmax = cur;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float3 c = current_gi.SampleLevel(current_gi_sampler,
                                              v.uv + float2(dx, dy) * tx, 0).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
        }
    }
    hist = clamp(hist, nmin, nmax);

    // exponential moving average (current 12%)
    return float4(lerp(hist, cur, 0.12), 1.0);
}
)";

struct SsgiCBLayout {
    Mat4 view_proj;
    Mat4 inv_view_proj;
    Vec4 eye;
    Vec4 params;
    Mat4 prev_view_proj;       // Phase 33c-3
    Vec4 temporal_params;      // Phase 34f-3: x=motion_texture_mode (0/1)
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> Ssgi::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    _device = &device;
    _width  = width;
    _height = height;

    if (auto r = CreateOutputRT(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size = CBSize<SsgiCBLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else _cb = Move(r.Value());

    return Ok();
}

Result<void> Ssgi::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    _output.Reset();
    _blur_output.Reset();
    TextureDesc td{};
    td.width  = width;
    td.height = height;
    // RGB 用、低帯域。R11G11B10F が HDR-friendly でメモリも小さい。
    td.format = EFormat::R11G11B10_Float;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    _output = Move(r.Value());

    // Phase 33c-2: blur 後の RT
    auto br = CreateRhiTexture(device, td);
    if (br.IsErr()) return Err<void>(br.Error());
    _blur_output = Move(br.Value());

    // Phase 33c-3: temporal accumulation の history ping-pong
    for (u32 i = 0; i < 2; ++i) {
        _history[i].Reset();
        auto hr = CreateRhiTexture(device, td);
        if (hr.IsErr()) return Err<void>(hr.Error());
        _history[i] = Move(hr.Value());
    }
    return Ok();
}

Result<void> Ssgi::CreatePipeline(IRhiDevice& device) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSsgiHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Ssgi.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSsgiHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Ssgi.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else _ps = Move(r.Value());

    PipelineDesc pd{};
    pd.vs            = _vs.Get();
    pd.ps            = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = EFormat::R11G11B10_Float;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 3;
    pd.cbuffer_names[0] = "SsgiCB";
    pd.texture_names[0] = "scene_color";
    pd.texture_names[1] = "scene_depth";
    pd.texture_names[2] = "normal_gbuffer";
    pd.static_sampler_count = 3;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[1].filter    = ESamplerFilter::Point;       // depth は離散値
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[2].filter    = ESamplerFilter::Point;       // 法線は per-pixel 厳密に
    pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    // Phase 33c-2: blur pipeline (ssgi_raw + scene_depth → blurred)。
    // VS は本体と同じ fullscreen-triangle なので _vs を再利用。
    ShaderDesc bps_d{};
    bps_d.stage = EShaderStage::Pixel;
    bps_d.hlsl_source = kSsgiBlurHLSL;
    bps_d.entry_point = "PSMain";
    bps_d.debug_name  = "SsgiBlur.PS";
    if (auto r = CreateRhiShader(device, bps_d); r.IsErr()) return Err<void>(r.Error());
    else _blur_ps = Move(r.Value());

    PipelineDesc bpd{};
    bpd.vs            = _vs.Get();
    bpd.ps            = _blur_ps.Get();
    bpd.topology      = EPrimitiveTopology::TriangleList;
    bpd.rt_format     = EFormat::R11G11B10_Float;
    bpd.depth_format  = EFormat::Unknown;
    bpd.depth_test    = false;
    bpd.depth_write   = false;
    bpd.cull_mode     = ECullMode::None;
    bpd.blend_mode    = EBlendMode::Opaque;
    bpd.cbuffer_slots = 1;
    bpd.texture_slots = 2;
    bpd.cbuffer_names[0] = "SsgiCB";
    bpd.texture_names[0] = "ssgi_raw";
    bpd.texture_names[1] = "scene_depth";
    bpd.static_sampler_count = 2;
    bpd.static_samplers[0].filter    = ESamplerFilter::Linear;
    bpd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    bpd.static_samplers[1].filter    = ESamplerFilter::Point;
    bpd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    bpd.vertex_stride = 0;
    bpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, bpd); r.IsErr()) return Err<void>(r.Error());
    else _blur_pipeline = Move(r.Value());

    // Phase 33c-3: temporal pipeline (current_gi + history_gi + scene_depth → history)
    ShaderDesc tps_d{};
    tps_d.stage = EShaderStage::Pixel;
    tps_d.hlsl_source = kSsgiTemporalHLSL;
    tps_d.entry_point = "PSMain";
    tps_d.debug_name  = "SsgiTemporal.PS";
    if (auto r = CreateRhiShader(device, tps_d); r.IsErr()) return Err<void>(r.Error());
    else _temporal_ps = Move(r.Value());

    PipelineDesc tpd{};
    tpd.vs            = _vs.Get();
    tpd.ps            = _temporal_ps.Get();
    tpd.topology      = EPrimitiveTopology::TriangleList;
    tpd.rt_format     = EFormat::R11G11B10_Float;
    tpd.depth_format  = EFormat::Unknown;
    tpd.depth_test    = false;
    tpd.depth_write   = false;
    tpd.cull_mode     = ECullMode::None;
    tpd.blend_mode    = EBlendMode::Opaque;
    tpd.cbuffer_slots = 1;
    tpd.texture_slots = 3;
    tpd.cbuffer_names[0] = "SsgiCB";
    tpd.texture_names[0] = "current_gi";
    tpd.texture_names[1] = "history_gi";
    tpd.texture_names[2] = "scene_depth";
    tpd.static_sampler_count = 3;
    for (u32 i = 0; i < 2; ++i) {
        tpd.static_samplers[i].filter    = ESamplerFilter::Linear;
        tpd.static_samplers[i].address_u = ESamplerAddress::Clamp;
        tpd.static_samplers[i].address_v = ESamplerAddress::Clamp;
    }
    tpd.static_samplers[2].filter    = ESamplerFilter::Point;
    tpd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    tpd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    tpd.vertex_stride = 0;
    tpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, tpd); r.IsErr()) return Err<void>(r.Error());
    else _temporal_pipeline = Move(r.Value());
    return Ok();
}

void Ssgi::Shutdown() noexcept {
    _temporal_pipeline.Reset();
    _blur_pipeline.Reset();
    _pipeline.Reset();
    _cb.Reset();
    _temporal_ps.Reset();
    _blur_ps.Reset();
    _ps.Reset();
    _vs.Reset();
    for (auto& h : _history) h.Reset();
    _blur_output.Reset();
    _output.Reset();
    _temporal_frame = 0;
    _device = nullptr;
}

Result<void> Ssgi::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 340, "Ssgi::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _width  = width;
    _height = height;
    _temporal_frame = 0;     // history は size 違いで使えないので reset
    return CreateOutputRT(*_device, width, height);
}

void Ssgi::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_color,
                  IRhiTexture& scene_depth,
                  IRhiTexture& normal_gbuffer,
                  const Mat4& view_proj, const Mat4& inv_view_proj,
                  const Mat4& prev_view_proj,
                  Vec3 eye, f32 intensity, f32 max_distance,
                  IRhiTexture* motion_texture) noexcept {
    if (!_output || !_blur_output || !_history[0] || !_history[1] ||
        !_pipeline || !_blur_pipeline || !_temporal_pipeline || !_cb) return;
    SsgiCBLayout data{};
    data.view_proj      = view_proj;
    data.inv_view_proj  = inv_view_proj;
    data.eye            = Vec4{eye.x, eye.y, eye.z, 1};
    data.params         = Vec4{intensity, max_distance,
                               1.0f / static_cast<f32>(_width),
                               1.0f / static_cast<f32>(_height)};
    data.prev_view_proj = prev_view_proj;
    // Phase 34f-3: motion texture が指定されていれば temporal pass を motion mode に。
    data.temporal_params = Vec4{ motion_texture ? 1.0f : 0.0f, 0, 0, 0 };
    _cb->Update(&data, sizeof(data));

    // Pass 1: SSGI raw → _output
    cl.BeginRenderToTexture(*_output, ClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_color);
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, normal_gbuffer);
    cl.Draw(3);
    cl.EndRenderToTexture(*_output);

    // Pass 2 (Phase 33c-2): depth-aware bilateral blur → _blur_output
    cl.BeginRenderToTexture(*_blur_output, ClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_blur_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, *_output);       // SSGI raw
    cl.SetTexture(1, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_blur_output);

    // Pass 3 (Phase 33c-3): temporal accumulation → _history[cur]
    const u32 cur  = _temporal_frame & 1u;
    const u32 prev = cur ^ 1u;
    // Cold-start: frame 0 は history が未初期化なので blur 結果を history slot に
    // bind する (lerp(blur, blur, a) = blur で garbage 排除)。翌フレーム以降は
    // 実 history を使う。
    IRhiTexture* hist_in = (_temporal_frame == 0u) ? _blur_output.Get()
                                                   : _history[prev].Get();
    cl.BeginRenderToTexture(*_history[cur], ClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*_temporal_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, *_blur_output);   // current (blur 済み)
    cl.SetTexture(1, *hist_in);        // history (or blur on frame 0)
    // Phase 34f-3: t2 は motion texture (あれば) または scene_depth。
    // shader 側が temporal_params.x で解釈を切替えるので PSO の slot 数は不変。
    cl.SetTexture(2, motion_texture ? *motion_texture : scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_history[cur]);

    ++_temporal_frame;
}

} // namespace acs
