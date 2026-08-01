// SPDX-License-Identifier: Apache-2.0
// Screen-Space Global Illumination 実装
#include "render/Ssgi.h"
#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif
#include "foundation/Move.h"
#include "foundation/Log.h"
#include "render/TemporalHistory.h"

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
    float4   temporal_params;  // x=motion mode, y=frame jitter, z=current weight
};

Texture2D    scene_color    : register(t0);
Texture2D    scene_depth    : register(t1);
Texture2D    normal_gbuffer : register(t2);   // world-space normal
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
    // normal G-buffer から per-pixel world normal を sample する。
    // cross(ddx,ddy) は 2x2 quad 単位で faceted になり、hemisphere の
    // ray 方向が段差状にずれて GI のサンプリングがブロック状になるため使わない。
    float3 N = normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz;
    if (dot(N, N) < 1e-6) return float4(0, 0, 0, 1);
    N = normalize(N);
    float3 V = normalize(eye.xyz - wp);
    if (dot(N, V) < 0.0) N = -N;

    // 8 low-discrepancy rays, each refined over 12 world-space steps.
    const int   kRays  = 8;
    const int   kSteps = 12;
    const float kMaxDist = max(params.y, 0.1);
    const float kIntensity = params.x;

    // Rotate the low-discrepancy pattern every frame. The former static pattern
    // could not converge under temporal accumulation and remained stippled.
    float jitter1 = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715)))
                       + temporal_params.y);
    float jitter2 = frac(31.4159265 * frac(dot(v.pos.xy, float2(0.04711057, 0.01183715)))
                       + temporal_params.y * 0.754877666);

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
    [loop]
    for (int r = 0; r < kRays; ++r) {
        // Hammersley-like cosine-weighted hemisphere distribution.
        float phi = 6.2831853 * frac((float(r) + jitter1) * 0.61803398875);
        float u = (float(r) + jitter2) / float(kRays);
        float cos_theta = sqrt(saturate(1.0 - u));
        float sin_theta = sqrt(1.0 - cos_theta * cos_theta);
        // local hemisphere dir (z = up = N)
        float3 ldir = float3(cos(phi) * sin_theta, sin(phi) * sin_theta, cos_theta);
        // local → world
        float3 ray_dir = T * ldir.x + B * ldir.y + N * ldir.z;

        // ray march (世界距離で進める; 画面外 / sky / 距離超過で終了)
        float3 ray_origin = wp + N * 0.02;  // start slightly off the surface
        float3 ray_pos = ray_origin;
        float3 ray_prev = ray_origin;
        float step_len = kMaxDist / float(kSteps);
        bool hit = false;
        float3 hit_color = float3(0, 0, 0);
        float  hit_t = 1.0;                            // hit までの正規化距離 (1=最遠) → 減衰用
        float  hit_edge = 0.0;
        [loop]
        for (int s = 1; s <= kSteps; ++s) {
            ray_pos = ray_origin + ray_dir * (step_len * float(s));
            // world → clip → ndc → uv
            float4 cp = mul(float4(ray_pos, 1.0), view_proj);
            if (cp.w < 1e-4) break;
            float3 ndc = cp.xyz / cp.w;
            if (any(abs(ndc.xy) > 1.0)) break;
            if (ndc.z < 0.0 || ndc.z > 1.0) break;
            float2 hit_uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
            float scene_d = scene_depth.SampleLevel(scene_depth_sampler, hit_uv, 0).r;
            if (scene_d >= 0.9999) {
                // Sky is transparent to the ray, but it still advances the
                // refinement segment. Keeping an older ray_prev can make the
                // next geometry crossing bisect across several stale steps.
                ray_prev = ray_pos;
                continue;
            }
            // Once the ray crosses the depth buffer, refine the world-space
            // segment. A fixed NDC epsilon changes by orders of magnitude with
            // distance and caused both leaks and thick halos.
            if (ndc.z > scene_d) {
                float3 lo = ray_prev;
                float3 hi = ray_pos;
                float2 refined_uv = hit_uv;
                float refined_depth = scene_d;
                [unroll]
                for (int refine = 0; refine < 3; ++refine) {
                    float3 mid = (lo + hi) * 0.5;
                    float4 mc = mul(float4(mid, 1.0), view_proj);
                    float3 mn = mc.xyz / max(mc.w, 1e-6);
                    float2 muv = float2(mn.x * 0.5 + 0.5, -mn.y * 0.5 + 0.5);
                    float md = scene_depth.SampleLevel(scene_depth_sampler, muv, 0).r;
                    if (mn.z > md) {
                        hi = mid;
                        refined_uv = muv;
                        refined_depth = md;
                    } else {
                        lo = mid;
                    }
                }
                float3 scene_wp = ReconstructWorldPos(refined_uv, refined_depth);
                if (distance(hi, scene_wp) <= max(step_len * 0.4, 0.03)) {
                    hit_color = scene_color.SampleLevel(scene_color_sampler, refined_uv, 0).rgb;
                    float2 edge = min(refined_uv, 1.0 - refined_uv);
                    hit_edge = saturate(min(edge.x, edge.y) * 20.0);
                    hit_t = (float(s) - 0.5) / float(kSteps);
                    hit = true;
                }
                break;
            }
            ray_prev = ray_pos;
        }
        if (hit) {
            // cos-weighted contribution (Lambert): N . ray_dir でフィルタ。
            // 距離減衰 (1-t)^2 で寄与を滑らかに 0 へ → kMaxDist でのハード cutoff が作る
            // «四角い» footprint 縁を消し、間接光を距離に応じて自然に減衰させる。
            float ndot = max(dot(N, ray_dir), 0.0);
            float falloff = (1.0 - hit_t) * (1.0 - hit_t);
            gi_sum += hit_color * ndot * falloff * hit_edge;
            gi_cnt += 1;
        }
    }

    float3 gi = (gi_cnt > 0) ? gi_sum / float(kRays) : float3(0, 0, 0);
    gi *= kIntensity;
    return float4(gi, 1.0);
}
)";

// depth-aware bilateral blur (RGB)。SSGI raw は 8 ray でも残るサンプリング
// ノイズを、depth 不連続を跨がない 5x5 blur で平滑化する。
const char* kSsgiBlurHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsgiCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;     // z=texel_w, w=texel_h
    float4x4 prev_view_proj;   // blur pass では未使用、CB レイアウト整合のため宣言
    float4   temporal_params;
};

Texture2D    ssgi_raw    : register(t0);
Texture2D    scene_depth : register(t1);
Texture2D    normal_gbuffer : register(t2);
SamplerState ssgi_raw_sampler    : register(s0);
SamplerState scene_depth_sampler : register(s1);
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
    return wp.xyz / max(abs(wp.w), 1e-6);
}

float4 PSMain(VSOut v) : SV_TARGET {
    float2 tx = float2(params.z, params.w);
    float center_d = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (center_d >= 0.9999) return float4(0, 0, 0, 1);
    float center_range = distance(ReconstructWorldPos(v.uv, center_d), eye.xyz);
    float3 center_n = normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz;
    center_n = dot(center_n, center_n) > 1e-6 ? normalize(center_n) : float3(0, 0, 1);

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
            if (d >= 0.9999) continue;
            float sw = exp(-float(dx*dx + dy*dy) / 8.0);
            float sample_range = distance(ReconstructWorldPos(uv, d), eye.xyz);
            float depth_sigma = max(params.y * 0.025, 0.03);
            float dz = (sample_range - center_range) / depth_sigma;
            float dw = exp(-0.5 * dz * dz);
            float3 n = normal_gbuffer.SampleLevel(normal_gbuffer_sampler, uv, 0).xyz;
            float nw = dot(n, n) > 1e-6
                     ? pow(saturate(dot(center_n, normalize(n))), 8.0)
                     : 0.0;
            float w  = sw * dw * nw;
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

// temporal accumulation。blur 済み SSGI を、前フレームの結果
// (history) と reprojection + neighborhood clamp して時間積分する。4 ray の
// ノイズが時間方向にも平均されて大幅に減る。TAA の resolve と同じ構造。
const char* kSsgiTemporalHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsgiCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;
    float4   params;          // z=texel_w, w=texel_h
    float4x4 prev_view_proj;   // reprojection 用
    float4   temporal_params;  // x=motion mode, y=frame jitter, z=current weight
};

Texture2D    current_gi  : register(t0);   // blur 済み SSGI (今フレーム)
Texture2D    history_gi  : register(t1);   // 前フレームの temporal 結果
Texture2D    scene_depth : register(t2);   // motion mode では motion vector
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
    // Explicit fallback + single exit keeps FXC's legacy flow analysis from
    // treating the helper result as potentially uninitialized.
    float2 reprojected_uv = uv;
    float depth = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
    if (depth >= 0.9999) {
        reprojected_uv = uv;
    } else {
        float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
        clip.y = -clip.y;
        float4 wp = mul(clip, inv_view_proj);
        wp.xyz /= max(wp.w, 1e-6);
        float4 pc = mul(float4(wp.xyz, 1.0), prev_view_proj);
        if (pc.w < 1e-4) {
            reprojected_uv = uv;
        } else {
            float2 pn = pc.xy / pc.w;
            reprojected_uv =
                float2(pn.x * 0.5 + 0.5, -pn.y * 0.5 + 0.5);
        }
    }
    return reprojected_uv;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 cur = current_gi.SampleLevel(current_gi_sampler, v.uv, 0).rgb;

    // motion texture モードなら scene_depth slot を motion vector
    // として再解釈し、動く mesh も含めて history を reproject する。
    // 非モードなら従来の camera-only depth reprojection。
    float2 huv = v.uv;
    if (temporal_params.x >= 0.5) {
        float2 mv = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).rg;
        huv = v.uv + mv;
    } else {
        huv = ReprojectUv(v.uv);
    }
    bool offscreen = any(huv < 0.0) || any(huv > 1.0);
    if (offscreen) huv = v.uv;
    float3 hist_unclipped = history_gi.SampleLevel(history_gi_sampler, huv, 0).rgb;
    float3 hist = hist_unclipped;

    // Variance clipping is more selective than a broad min/max box and avoids
    // leaking saturated indirect light across thin silhouettes.
    float2 tx = float2(params.z, params.w);
    float3 nmin = cur, nmax = cur;
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float3 c = current_gi.SampleLevel(current_gi_sampler,
                                              v.uv + float2(dx, dy) * tx, 0).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
            moment1 += c;
            moment2 += c * c;
        }
    }
    float3 mean = moment1 / 9.0;
    float3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    hist = clamp(hist, max(nmin, mean - sigma * 1.5),
                       min(nmax, mean + sigma * 1.5));

    float cur_luma = dot(cur, float3(0.2126, 0.7152, 0.0722));
    float hist_luma = dot(hist_unclipped, float3(0.2126, 0.7152, 0.0722));
    float relative_delta = abs(cur_luma - hist_luma)
                         / max(max(cur_luma, hist_luma), 0.03);
    float motion_px = length((huv - v.uv) / max(tx, float2(1e-6, 1e-6)));
    float current_weight = saturate(temporal_params.z);
    current_weight = max(current_weight, saturate(relative_delta) * 0.8);
    current_weight = max(current_weight, saturate(motion_px / 16.0) * 0.4);
    if (offscreen) current_weight = 1.0;
    return float4(lerp(hist, cur, current_weight), 1.0);
}
)";

struct FSsgiCbLayout {
    FMat4 view_proj;
    FMat4 inv_view_proj;
    FVec4 eye;
    FVec4 params;
    FMat4 prev_view_proj;
    FVec4 temporal_params;      // x=motion mode, y=jitter, z=current weight
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

/** Compile raw-DX12 bytecode without accessing the render device. */
TResult<CSsgi::FCompiledShaders> CSsgi::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSsgiHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CSsgi.VS";

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSsgiHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CSsgi.PS";

    FShaderDesc blur_ps_d{};
    blur_ps_d.stage = EShaderStage::Pixel;
    blur_ps_d.hlsl_source = kSsgiBlurHLSL;
    blur_ps_d.entry_point = "PSMain";
    blur_ps_d.debug_name  = "SsgiBlur.PS";

    FShaderDesc temporal_ps_d{};
    temporal_ps_d.stage = EShaderStage::Pixel;
    temporal_ps_d.hlsl_source = kSsgiTemporalHLSL;
    temporal_ps_d.entry_point = "PSMain";
    temporal_ps_d.debug_name  = "SsgiTemporal.PS";

    auto vertex = MakeUnique<FDx12Shader>();
    if (!vertex)
        return ACS_ERR(Memory, 381, "SSGI vertex shader allocation failed");
    const FHrResult vertex_result = vertex->Init(vs_d);
    if (vertex_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 382, "SSGI vertex shader CPU compile failed",
            static_cast<u32>(vertex_result.hr));
    }

    auto main_pixel = MakeUnique<FDx12Shader>();
    if (!main_pixel)
        return ACS_ERR(Memory, 383, "SSGI main pixel shader allocation failed");
    const FHrResult main_pixel_result = main_pixel->Init(ps_d);
    if (main_pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 384, "SSGI main pixel shader CPU compile failed",
            static_cast<u32>(main_pixel_result.hr));
    }

    auto blur_pixel = MakeUnique<FDx12Shader>();
    if (!blur_pixel)
        return ACS_ERR(Memory, 385, "SSGI blur pixel shader allocation failed");
    const FHrResult blur_pixel_result = blur_pixel->Init(blur_ps_d);
    if (blur_pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 386, "SSGI blur pixel shader CPU compile failed",
            static_cast<u32>(blur_pixel_result.hr));
    }

    auto temporal_pixel = MakeUnique<FDx12Shader>();
    if (!temporal_pixel)
        return ACS_ERR(
            Memory, 387, "SSGI temporal pixel shader allocation failed");
    const FHrResult temporal_pixel_result = temporal_pixel->Init(temporal_ps_d);
    if (temporal_pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 388, "SSGI temporal pixel shader CPU compile failed",
            static_cast<u32>(temporal_pixel_result.hr));
    }

    FCompiledShaders compiled{};
    compiled.vertex = TUniquePtr<IRhiShader>(
        vertex.Release(), vertex.GetAllocator());
    compiled.main_pixel = TUniquePtr<IRhiShader>(
        main_pixel.Release(), main_pixel.GetAllocator());
    compiled.blur_pixel = TUniquePtr<IRhiShader>(
        blur_pixel.Release(), blur_pixel.GetAllocator());
    compiled.temporal_pixel = TUniquePtr<IRhiShader>(
        temporal_pixel.Release(), temporal_pixel.GetAllocator());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 389,
        "SSGI CPU compilation is available only on the raw DX12 backend");
#endif
}

TResult<void> CSsgi::Init(IRhiDevice& device, u32 width, u32 height) noexcept {
    FCompiledShaders compiled{};

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSsgiHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CSsgi.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.vertex = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSsgiHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CSsgi.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.main_pixel = Move(r.Value());

    FShaderDesc blur_ps_d{};
    blur_ps_d.stage = EShaderStage::Pixel;
    blur_ps_d.hlsl_source = kSsgiBlurHLSL;
    blur_ps_d.entry_point = "PSMain";
    blur_ps_d.debug_name  = "SsgiBlur.PS";
    if (auto r = CreateRhiShader(device, blur_ps_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.blur_pixel = Move(r.Value());

    FShaderDesc temporal_ps_d{};
    temporal_ps_d.stage = EShaderStage::Pixel;
    temporal_ps_d.hlsl_source = kSsgiTemporalHLSL;
    temporal_ps_d.entry_point = "PSMain";
    temporal_ps_d.debug_name  = "SsgiTemporal.PS";
    if (auto r = CreateRhiShader(device, temporal_ps_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.temporal_pixel = Move(r.Value());

    return InitWithCompiledShaders(
        device, Move(compiled), width, height);
}

TResult<void> CSsgi::InitWithCompiledShaders(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    u32 width,
    u32 height) noexcept {
    if (shaders.vertex.Get() == nullptr ||
        shaders.main_pixel.Get() == nullptr ||
        shaders.blur_pixel.Get() == nullptr ||
        shaders.temporal_pixel.Get() == nullptr) {
        return ACS_ERR(Render, 390, "SSGI compiled shader set is incomplete");
    }

    // Build the complete replacement off to the side.  Startup can retry an
    // effect after a device/allocation failure, so publishing shaders, render
    // targets or PSOs one at a time would leave a mixture of generations.
    CSsgi candidate;
    candidate.m_Device = &device;
    candidate.m_Vs = Move(shaders.vertex);
    candidate.m_Ps = Move(shaders.main_pixel);
    candidate.m_BlurPs = Move(shaders.blur_pixel);
    candidate.m_TemporalPs = Move(shaders.temporal_pixel);

    if (auto r = candidate.CreateOutputRT(device, width, height); r.IsErr()) return r;
    candidate.m_Width  = width;
    candidate.m_Height = height;
    if (auto r = candidate.CreatePipeline(device); r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size = CBSize<FSsgiCbLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else candidate.m_Cb = Move(r.Value());

    Shutdown();
    m_Device = candidate.m_Device;
    m_Width = candidate.m_Width;
    m_Height = candidate.m_Height;
    m_Output = Move(candidate.m_Output);
    m_BlurOutput = Move(candidate.m_BlurOutput);
    for (u32 i = 0; i < 2; ++i) m_History[i] = Move(candidate.m_History[i]);
    m_Vs = Move(candidate.m_Vs);
    m_Ps = Move(candidate.m_Ps);
    m_BlurPs = Move(candidate.m_BlurPs);
    m_TemporalPs = Move(candidate.m_TemporalPs);
    m_Pipeline = Move(candidate.m_Pipeline);
    m_BlurPipeline = Move(candidate.m_BlurPipeline);
    m_TemporalPipeline = Move(candidate.m_TemporalPipeline);
    m_Cb = Move(candidate.m_Cb);
    m_TemporalFrame = 0;
    m_OutputValid = false;

    return Ok();
}

TResult<void> CSsgi::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    FTextureDesc td{};
    td.width  = width;
    td.height = height;
    // RGB 用、低帯域。R11G11B10F が HDR-friendly でメモリも小さい。
    td.format = EFormat::R11G11B10_Float;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    TUniquePtr<IRhiTexture> output = Move(r.Value());

    // blur 後の RT
    auto br = CreateRhiTexture(device, td);
    if (br.IsErr()) return Err<void>(br.Error());
    TUniquePtr<IRhiTexture> blur_output = Move(br.Value());

    // temporal accumulation の history ping-pong
    TUniquePtr<IRhiTexture> history[2];
    for (u32 i = 0; i < 2; ++i) {
        auto hr = CreateRhiTexture(device, td);
        if (hr.IsErr()) return Err<void>(hr.Error());
        history[i] = Move(hr.Value());
    }

    // Commit only after the complete set exists.  A failed resize therefore
    // keeps the previous frame's mutually compatible output/history set.
    m_Output = Move(output);
    m_BlurOutput = Move(blur_output);
    for (u32 i = 0; i < 2; ++i) m_History[i] = Move(history[i]);
    return Ok();
}

TResult<void> CSsgi::CreatePipeline(IRhiDevice& device) noexcept {
    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
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
    else m_Pipeline = Move(r.Value());

    // blur pipeline (ssgi_raw + scene_depth → blurred)。
    // VS は本体と同じ fullscreen-triangle なので m_Vs を再利用。
    FPipelineDesc bpd{};
    bpd.vs            = m_Vs.Get();
    bpd.ps            = m_BlurPs.Get();
    bpd.topology      = EPrimitiveTopology::TriangleList;
    bpd.rt_format     = EFormat::R11G11B10_Float;
    bpd.depth_format  = EFormat::Unknown;
    bpd.depth_test    = false;
    bpd.depth_write   = false;
    bpd.cull_mode     = ECullMode::None;
    bpd.blend_mode    = EBlendMode::Opaque;
    bpd.cbuffer_slots = 1;
    bpd.texture_slots = 3;
    bpd.cbuffer_names[0] = "SsgiCB";
    bpd.texture_names[0] = "ssgi_raw";
    bpd.texture_names[1] = "scene_depth";
    bpd.texture_names[2] = "normal_gbuffer";
    bpd.static_sampler_count = 3;
    bpd.static_samplers[0].filter    = ESamplerFilter::Linear;
    bpd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    bpd.static_samplers[1].filter    = ESamplerFilter::Point;
    bpd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    bpd.static_samplers[2].filter    = ESamplerFilter::Linear;
    bpd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    bpd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    bpd.vertex_stride = 0;
    bpd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, bpd); r.IsErr()) return Err<void>(r.Error());
    else m_BlurPipeline = Move(r.Value());

    // temporal pipeline (current_gi + history_gi + scene_depth → history)
    FPipelineDesc tpd{};
    tpd.vs            = m_Vs.Get();
    tpd.ps            = m_TemporalPs.Get();
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
    else m_TemporalPipeline = Move(r.Value());
    return Ok();
}

void CSsgi::Shutdown() noexcept {
    m_TemporalPipeline.Reset();
    m_BlurPipeline.Reset();
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_TemporalPs.Reset();
    m_BlurPs.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    for (auto& h : m_History) h.Reset();
    m_BlurOutput.Reset();
    m_Output.Reset();
    m_TemporalFrame = 0;
    m_OutputValid = false;
    m_Width = 0;
    m_Height = 0;
    m_Device = nullptr;
}

TResult<void> CSsgi::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 340, "CSsgi::Resize before Init");
    if (width == m_Width && height == m_Height) return Ok();
    if (auto r = CreateOutputRT(*m_Device, width, height); r.IsErr()) return r;
    m_Width  = width;
    m_Height = height;
    m_TemporalFrame = 0;     // history は size 違いで使えないので reset
    m_OutputValid = false;
    return Ok();
}

void CSsgi::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_color,
                  IRhiTexture& scene_depth,
                  IRhiTexture& normal_gbuffer,
                  const FMat4& view_proj, const FMat4& inv_view_proj,
                  const FMat4& prev_view_proj,
                  FVec3 eye, f32 intensity, f32 max_distance,
                  IRhiTexture* motion_texture) noexcept {
    m_OutputValid = false;
    if (!m_Output || !m_BlurOutput || !m_History[0] || !m_History[1] ||
        !m_Pipeline || !m_BlurPipeline || !m_TemporalPipeline || !m_Cb) return;
    const auto temporal = ResolveTemporalHistoryFrame(
        m_TemporalFrame,
        view_proj,
        prev_view_proj,
        0.1f,
        motion_texture != nullptr);
    FSsgiCbLayout data{};
    data.view_proj      = view_proj;
    data.inv_view_proj  = inv_view_proj;
    data.eye            = FVec4{eye.x, eye.y, eye.z, 1};
    data.params         = FVec4{intensity, max_distance,
                               1.0f / static_cast<f32>(m_Width),
                               1.0f / static_cast<f32>(m_Height)};
    data.prev_view_proj = temporal.previous_view_projection;
    // motion texture が指定されていれば temporal pass を motion mode に。
    // A 64-phase coprime sequence rotates raw GI sampling between frames.
    const f32 frame_jitter =
        static_cast<f32>((m_TemporalFrame * 23u) & 63u) * (1.0f / 64.0f);
    data.temporal_params = FVec4{
        temporal.motion_vectors_enabled ? 1.0f : 0.0f,
        frame_jitter,
        temporal.current_frame_weight,
        0.0f};
    m_Cb->Update(&data, sizeof(data));

    // Pass 1: SSGI raw → m_Output
    cl.BeginRenderToTexture(*m_Output, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, scene_color);
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, normal_gbuffer);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_Output);

    // Pass 2: depth-aware bilateral blur → m_BlurOutput
    cl.BeginRenderToTexture(*m_BlurOutput, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*m_BlurPipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_Output);       // SSGI raw
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, normal_gbuffer);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_BlurOutput);

    // Pass 3: temporal accumulation → m_History[cur]
    const u32 cur  = m_TemporalFrame & 1u;
    const u32 prev = cur ^ 1u;
    // Cold-start: frame 0 は history が未初期化なので blur 結果を history slot に
    // bind する (lerp(blur, blur, a) = blur で garbage 排除)。翌フレーム以降は
    // 実 history を使う。
    IRhiTexture* hist_in = (m_TemporalFrame == 0u) ? m_BlurOutput.Get()
                                                   : m_History[prev].Get();
    cl.BeginRenderToTexture(*m_History[cur], FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    cl.SetPipeline(*m_TemporalPipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_BlurOutput);   // current (blur 済み)
    cl.SetTexture(1, *hist_in);        // history (or blur on frame 0)
    // t2 は motion texture (あれば) または scene_depth。
    // shader 側が temporal_params.x で解釈を切替えるので PSO の slot 数は不変。
    cl.SetTexture(
        2,
        temporal.motion_vectors_enabled ? *motion_texture : scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_History[cur]);

    ++m_TemporalFrame;
    m_OutputValid = true;
}

} // namespace acs
