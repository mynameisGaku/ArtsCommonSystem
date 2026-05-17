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
    float4   params;         // x=intensity, y=max_ray_dist, z=frame_jitter, w=thickness
};

Texture2D    scene_color    : register(t0);
Texture2D    scene_depth    : register(t1);
Texture2D    normal_gbuffer : register(t2);   // world-space normal (Phase 34m)
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
    // normal G-buffer から per-pixel world normal を sample (Phase 34m)。
    // 旧 cross(ddx,ddy) は 2x2 quad 単位で faceted になり、曲面の反射ベクトルが
    // 段差状にずれて反射がガビガビになっていた。geometry 由来の補間法線で根本解決。
    float3 N = normalize(normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz);
    // facing 補正 (背面 normal の保険、通常は no-op)
    if (dot(N, V) < 0.0) N = -N;
    float3 R = reflect(-V, N);
    if (dot(R, V) < -0.95) return float4(0, 0, 0, 0); // 真後ろ反射は無視

    // Ray march: world space で粗く前進して geometry を貫いた step を検出し、
    // そのあと binary search で交差点を絞り込む。粗 march だけだと hit 位置が
    // step 長ぶんずれ、反射が縦に滲んでガビガビになる (Phase 34e-fix)。
    const int   kSteps    = 48;
    const float kStepLen  = max(params.y, 0.5) / float(kSteps);   // max_ray_dist / N
    const float thickness = max(params.w, 0.05);

    // per-frame + per-pixel jitter で ray 起点を 1 step 未満ずらす (Phase 34e-3)。
    // binary search 後でも hit/miss 境界が毎フレーム散り、temporal 累積で
    // silhouette のジャギーが時間方向に均される。
    float ign    = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715))));
    float jitter = frac(ign + params.z);          // params.z = 毎フレーム値

    float3 ray_pos  = wp + N * 0.02 + R * (0.02 + kStepLen * jitter);  // 起点 offset + jitter
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

// Phase 34e-3: temporal accumulation。jitter 付き raw SSR を、前フレームの履歴と
// reproject + neighborhood clamp して時間方向に平均する。silhouette のジャギーや
// march の量子化ノイズが大幅に減る。RGBA 全 ch を扱う (.a = hit mask も平滑化され、
// hit/miss 境界がアンチエイリアスされる)。temporal SSGI と同形。
const char* kSsrTemporalHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsrCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;
    float4x4 prev_view_proj;     // Phase 34e-3: reprojection 用
    float4   temporal_params;    // x=texel_w, y=texel_h, z=blend_factor
};

Texture2D    current_ssr : register(t0);   // jitter 付き raw SSR (今フレーム)
Texture2D    history_ssr : register(t1);   // 前フレームの temporal 結果
Texture2D    scene_depth : register(t2);
SamplerState current_ssr_sampler : register(s0);
SamplerState history_ssr_sampler : register(s1);
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

// camera motion 由来の history reproject (反射元サーフェスの動きで履歴をずらす)
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
    float4 cur = current_ssr.SampleLevel(current_ssr_sampler, v.uv, 0);

    float2 huv = ReprojectUv(v.uv);
    if (any(huv < 0.0) || any(huv > 1.0)) huv = v.uv;   // 画面外は静的 fallback
    float4 hist = history_ssr.SampleLevel(history_ssr_sampler, huv, 0);

    // Neighborhood clamp (3x3 of current)。camera 回転時の古い反射の残像を抑える。
    // rgb と hit mask (.a) の両方を clamp する。
    float2 tx = float2(temporal_params.x, temporal_params.y);
    float4 nmin = cur, nmax = cur;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float4 c = current_ssr.SampleLevel(current_ssr_sampler,
                                               v.uv + float2(dx, dy) * tx, 0);
            nmin = min(nmin, c);
            nmax = max(nmax, c);
        }
    }
    hist = clamp(hist, nmin, nmax);

    // exponential moving average。jitter してあるので強めに累積してよい。
    float a = saturate(temporal_params.z);
    if (a < 1e-4) a = 0.1;
    return lerp(hist, cur, a);
}
)";

struct SsrCBLayout {
    Mat4 view_proj;
    Mat4 inv_view_proj;
    Vec4 eye;
    Vec4 params;
    Mat4 prev_view_proj;     // Phase 34e-3: temporal reproject 用
    Vec4 temporal_params;    // Phase 34e-3: x=texel_w, y=texel_h, z=blend_factor
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

    // Phase 34e-3: temporal accumulation の history ping-pong
    for (u32 i = 0; i < 2; ++i) {
        _history[i].Reset();
        auto hr = CreateRhiTexture(device, td);
        if (hr.IsErr()) return Err<void>(hr.Error());
        _history[i] = Move(hr.Value());
    }
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
    pd.texture_slots = 3;
    pd.cbuffer_names[0] = "SsrCB";
    pd.texture_names[0] = "scene_color";
    pd.texture_names[1] = "scene_depth";
    pd.texture_names[2] = "normal_gbuffer";
    pd.static_sampler_count = 3;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Clamp;
    pd.static_samplers[0].address_v = SamplerAddress::Clamp;
    pd.static_samplers[1].filter    = SamplerFilter::Point;
    pd.static_samplers[1].address_u = SamplerAddress::Clamp;
    pd.static_samplers[1].address_v = SamplerAddress::Clamp;
    // normal G-buffer は Point sample (silhouette を跨ぐ法線の線形混色を避ける)
    pd.static_samplers[2].filter    = SamplerFilter::Point;
    pd.static_samplers[2].address_u = SamplerAddress::Clamp;
    pd.static_samplers[2].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    // Phase 34e-3: temporal pipeline (current_ssr + history_ssr + scene_depth → history)。
    // VS は fullscreen-triangle で raw と同形なので _vs を再利用。
    ShaderDesc tps_d{};
    tps_d.stage = ShaderStage::Pixel;
    tps_d.hlsl_source = kSsrTemporalHLSL;
    tps_d.entry_point = "PSMain";
    tps_d.debug_name  = "SsrTemporal.PS";
    if (auto r = CreateRhiShader(device, tps_d); r.IsErr()) return Err<void>(r.Error());
    else _temporal_ps = Move(r.Value());

    PipelineDesc tpd{};
    tpd.vs            = _vs.Get();
    tpd.ps            = _temporal_ps.Get();
    tpd.topology      = PrimitiveTopology::TriangleList;
    tpd.rt_format     = _hdr_format;
    tpd.depth_format  = Format::Unknown;
    tpd.depth_test    = false;
    tpd.depth_write   = false;
    tpd.cull_mode     = CullMode::None;
    tpd.blend_mode    = BlendMode::Opaque;
    tpd.cbuffer_slots = 1;
    tpd.texture_slots = 3;
    tpd.cbuffer_names[0] = "SsrCB";
    tpd.texture_names[0] = "current_ssr";
    tpd.texture_names[1] = "history_ssr";
    tpd.texture_names[2] = "scene_depth";
    tpd.static_sampler_count = 3;
    for (u32 i = 0; i < 2; ++i) {
        tpd.static_samplers[i].filter    = SamplerFilter::Linear;
        tpd.static_samplers[i].address_u = SamplerAddress::Clamp;
        tpd.static_samplers[i].address_v = SamplerAddress::Clamp;
    }
    tpd.static_samplers[2].filter    = SamplerFilter::Point;
    tpd.static_samplers[2].address_u = SamplerAddress::Clamp;
    tpd.static_samplers[2].address_v = SamplerAddress::Clamp;
    tpd.vertex_stride = 0;
    tpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, tpd); r.IsErr()) return Err<void>(r.Error());
    else _temporal_pipeline = Move(r.Value());
    return Ok();
}

void Ssr::Shutdown() noexcept {
    _temporal_pipeline.Reset();
    _pipeline.Reset();
    _cb.Reset();
    _temporal_ps.Reset();
    _ps.Reset();
    _vs.Reset();
    for (auto& h : _history) h.Reset();
    _output.Reset();
    _temporal_frame = 0;
    _device = nullptr;
}

Result<void> Ssr::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 320, "Ssr::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _width = width;
    _height = height;
    _temporal_frame = 0;     // history は size 違いで使えないので reset
    return CreateOutputRT(*_device, width, height);
}

void Ssr::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_color, IRhiTexture& scene_depth,
                  IRhiTexture& normal_gbuffer,
                  const Mat4& view_proj, const Mat4& inv_view_proj,
                  const Mat4& prev_view_proj,
                  Vec3 eye, f32 intensity) noexcept {
    if (!_output || !_history[0] || !_history[1] ||
        !_pipeline || !_temporal_pipeline || !_cb) return;

    // per-frame jitter 値: frac(frame * 黄金比) で低 discrepancy に散らす。
    // frame は 1024 で wrap して f32 精度内に収める。
    const u32 jf     = _temporal_frame & 1023u;
    const f32 jt     = static_cast<f32>(jf) * 0.61803399f;
    const f32 jitter = jt - static_cast<f32>(static_cast<u32>(jt));

    SsrCBLayout data{};
    data.view_proj      = view_proj;
    data.inv_view_proj  = inv_view_proj;
    data.eye            = Vec4{eye.x, eye.y, eye.z, 1};
    data.params         = Vec4{intensity, /*max_dist=*/12.0f, jitter, /*thickness=*/0.4f};
    data.prev_view_proj = prev_view_proj;
    data.temporal_params = Vec4{ 1.0f / static_cast<f32>(_width),
                                 1.0f / static_cast<f32>(_height),
                                 /*blend=*/0.1f, 0 };
    _cb->Update(&data, sizeof(data));

    // Pass 1: raw SSR (jitter 付き march) → _output
    cl.BeginRenderToTexture(*_output, ClearColor{0, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, scene_color);
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, normal_gbuffer);
    cl.Draw(3);
    cl.EndRenderToTexture(*_output);

    // Pass 2: temporal accumulation → _history[cur]
    const u32 cur  = _temporal_frame & 1u;
    const u32 prev = cur ^ 1u;
    // Cold-start (frame 0): history 未初期化なので raw (_output) を history slot に
    // bind する。reproject はほぼ identity、clamp 後 lerp(raw, raw, a)=raw で garbage 排除。
    IRhiTexture* hist_in = (_temporal_frame == 0u) ? _output.Get() : _history[prev].Get();
    cl.BeginRenderToTexture(*_history[cur], ClearColor{0, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*_temporal_pipeline);
    cl.SetConstantBuffer(0, *_cb);
    cl.SetTexture(0, *_output);        // current (jitter 付き raw)
    cl.SetTexture(1, *hist_in);        // history (or raw on frame 0)
    cl.SetTexture(2, scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*_history[cur]);

    ++_temporal_frame;
}

} // namespace acs
