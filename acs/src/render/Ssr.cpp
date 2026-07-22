// SPDX-License-Identifier: Apache-2.0
// Screen-Space Reflection 実装
#include "render/Ssr.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs {

namespace {

/** raw SSR パスの HLSL ソース (screen-space DDA ray march + Hi-Z skip-ahead)。 */
const char* kSsrHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer SsrCB : register(b0) {
    float4x4 view_proj;
    float4x4 inv_view_proj;
    float4   eye;             // xyz = world pos
    float4   params;          // x=intensity, y=max_ray_dist, z=frame_jitter, w=thickness_world
    float4x4 prev_view_proj;  // raw march では未使用 (CB layout 整合のため宣言)
    float4   temporal_params; // x=1/width, y=1/height, z=blend
    // Hi-Z: x=enabled, y=mip_count, zw=physical level-0 dimensions
    float4   hiz_params;
};

Texture2D    scene_color    : register(t0);
Texture2D    scene_depth    : register(t1);
Texture2D    normal_gbuffer : register(t2);   // world-space normal
Texture2D    hiz_even       : register(t3);   // level 0,2,4,... at matching mip
Texture2D    hiz_odd        : register(t4);   // level 1,3,5,... at matching mip
SamplerState scene_color_sampler    : register(s0);
SamplerState scene_depth_sampler    : register(s1);
SamplerState normal_gbuffer_sampler : register(s2);
SamplerState hiz_even_sampler       : register(s3);
SamplerState hiz_odd_sampler        : register(s4);

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

// The physical level-0 texture is power-of-two padded, so every mip maps to
// an exact (8 << level)-pixel source block even for odd scene dimensions.
float HiZMinAt(float2 pixel, int level, out float2 block, out float block_size) {
    // Initialize both out parameters and the result before branching.
    // This is redundant mathematically, but required for reliable FXC flow
    // analysis when this helper is called from a nested dynamic loop.
    block = float2(0.0, 0.0);
    block_size = 1.0;
    float min_depth = 0.0;
    float level_scale = exp2((float)level);
    block_size = 8.0 * level_scale;
    block = floor(pixel / block_size);
    float2 mip_size = max(floor(hiz_params.zw / level_scale), float2(1.0, 1.0));
    float2 uv = (block + 0.5) / mip_size;
    if ((level & 1) != 0) {
        min_depth = hiz_odd.SampleLevel(hiz_odd_sampler, uv, (float)level).r;
    } else {
        min_depth = hiz_even.SampleLevel(hiz_even_sampler, uv, (float)level).r;
    }
    return min_depth;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float depth = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).r;
    if (depth >= 0.9999) return float4(0, 0, 0, 0);   // sky pixel、反射なし

    float3 wp = ReconstructWorldPos(v.uv, depth);
    float3 V  = normalize(eye.xyz - wp);
    // normal G-buffer から per-pixel world normal を sample
    float3 N  = normal_gbuffer.SampleLevel(normal_gbuffer_sampler, v.uv, 0).xyz;
    if (dot(N, N) < 1e-6) return float4(0, 0, 0, 0);
    N = normalize(N);
    if (dot(N, V) < 0.0) N = -N;                       // facing 補正 (背面の保険)
    float3 R  = reflect(-V, N);
    if (dot(R, V) < -0.95) return float4(0, 0, 0, 0);  // 真後ろ反射は SSR データ無し

    // ===== screen-space DDA ray march (McGuire & Mara 2014) =====
    // 反射レイを screen 空間へ射影し 1 texel/step で行進する。NDC depth は screen
    // 空間で線形 (射影変換は直線を直線に写し、perspective divide 後の NDC 空間でも
    // レイは直線 → z_ndc は screen 座標の affine 関数。ラスタライザが三角形の depth を
    // 線形補間できるのと同じ原理) なので、レイ depth は端点間の線形補間で正確。world
    // 固定ステップ march は screen 空間でサンプリングが疎になり反射像がレイ方向に
    // 伸びてスメアしていた — DDA で根本解決。hit は depth 区間の straddle で交差を
    // 確定し、surface 奥へ回り込んだら world 距離で occlusion を判定する。
    float2 res    = float2(1.0 / temporal_params.x, 1.0 / temporal_params.y);
    float  maxLen = max(params.y, 0.5);

    float3 w0 = wp + N * 0.02;                         // self-intersection 回避の法線バイアス
    float3 w1 = w0 + R * maxLen;

    float4 c0 = mul(float4(w0, 1.0), view_proj);
    float4 c1 = mul(float4(w1, 1.0), view_proj);
    if (c0.w <= 1.0e-4) return float4(0, 0, 0, 0);     // 起点が near 面手前 → SSR 不能
    // 終点が near 面より手前 (w<=0) なら w が正の範囲で打ち切る (perspective divide 保護)
    if (c1.w <= 1.0e-4) {
        float t = saturate((c0.w - 1.0e-4) / max(c0.w - c1.w, 1.0e-8));
        c1 = lerp(c0, c1, t);
    }

    // perspective divide → screen pixel 座標 + NDC depth
    float3 n0  = c0.xyz / c0.w;
    float3 n1  = c1.xyz / c1.w;
    float2 uv0 = float2(n0.x * 0.5 + 0.5, -n0.y * 0.5 + 0.5);
    float2 uv1 = float2(n1.x * 0.5 + 0.5, -n1.y * 0.5 + 0.5);
    float2 p0  = uv0 * res;
    float2 p1  = uv1 * res;

    // DDA: screen 上の長辺を 1 texel/step で歩く
    float2 dp        = p1 - p0;
    float  stepCount = max(abs(dp.x), abs(dp.y));
    if (stepCount < 1.0) return float4(0, 0, 0, 0);    // レイが画面上ほぼ点 → 反射なし
    float2 dpStep    = dp / stepCount;                 // 1 step の pixel 増分 (長辺 ±1)
    float  dzStep    = (n1.z - n0.z) / stepCount;      // 1 step の NDC depth 増分 (一定)
    // Full Hi-Z keeps longer reflections affordable. The non-Hi-Z fallback
    // retains its former worst-case cost.
    float  marchMax  = min(stepCount, hiz_params.x >= 0.5 ? 1024.0 : 512.0);

    // per-frame + per-pixel jitter で開始位置を 1 texel 未満ずらす (temporal dither)
    float ign    = frac(52.9829189 * frac(dot(v.pos.xy, float2(0.06711056, 0.00583715))));
    float jitter = frac(ign + params.z);

    float2 p = p0   + dpStep * jitter;
    float  z = n0.z + dzStep * jitter;
    float  thicknessW = max(params.w, 0.01);           // hit 受理の world 距離厚み

    float2 hit_uv = float2(0, 0);
    bool   hit = false;
    float  hitFrac = 0.0;          // marchMax に対する到達割合 (距離フェード用)
    [loop]
    for (float i = 0.0; i < marchMax; i += 1.0) {
        // ===== Hierarchical-Z skip-ahead =====
        // A coarse level may skip only while its minimum depth is behind the
        // ray, proving that every full-resolution sample covered by the cell is
        // empty in front of it. Stop strictly before the next cell boundary;
        // the regular step below crosses and validates it at full resolution.
        if (hiz_params.x >= 0.5 && i > 0.0 && dzStep > 1.0e-6) {
            if (p.x >= 0.0 && p.x < res.x &&
                p.y >= 0.0 && p.y < res.y) {
                int mip_count = max((int)(hiz_params.y + 0.5), 1);
                [loop]
                for (int level = mip_count - 1; level >= 0; --level) {
                    float2 block = float2(0.0, 0.0);
                    float block_size = 1.0;
                    float coarse_min = HiZMinAt(p, level, block, block_size);
                    if (coarse_min > 0.0 && z + dzStep < coarse_min) {
                        float skip = floor((coarse_min - z) / dzStep);
                        float2 block_min = block * block_size;
                        float2 steps_to_boundary = float2(1e20, 1e20);
                        if (dpStep.x > 1e-6) {
                            steps_to_boundary.x =
                                (block_min.x + block_size - p.x) / dpStep.x;
                        } else if (dpStep.x < -1e-6) {
                            steps_to_boundary.x = (block_min.x - p.x) / dpStep.x;
                        }
                        if (dpStep.y > 1e-6) {
                            steps_to_boundary.y =
                                (block_min.y + block_size - p.y) / dpStep.y;
                        } else if (dpStep.y < -1e-6) {
                            steps_to_boundary.y = (block_min.y - p.y) / dpStep.y;
                        }
                        float block_skip =
                            max(ceil(min(steps_to_boundary.x,
                                         steps_to_boundary.y)) - 1.0, 0.0);
                        skip = min(max(skip, 0.0), block_skip);
                        if (skip > 0.0) {
                            p += dpStep * skip;
                            z += dzStep * skip;
                            i += skip;
                            break;
                        }
                    }
                }
                if (i >= marchMax) break;
            }
        }

        float z_prev = z;
        p += dpStep;
        z += dzStep;
        if (i < 1.0) continue;                         // 起点 texel skip
        float2 uv_s = p / res;
        if (uv_s.x < 0.0 || uv_s.x > 1.0 ||
            uv_s.y < 0.0 || uv_s.y > 1.0) break;       // 画面外で打ち切り
        float sd = scene_depth.SampleLevel(scene_depth_sampler, uv_s, 0).r;
        if (sd >= 0.9999) continue;                    // sky は貫通
        float z_lo = min(z_prev, z);
        float z_hi = max(z_prev, z);
        if (z_hi < sd) continue;                       // レイは surface 手前 → 前進

        // レイが surface depth に到達。straddle (z_lo<=sd<=z_hi) なら当 texel 内で
        // 交差確定。完全に奥 (z_lo>sd) なら world 距離で occlusion かを判定する。
        float3 scene_wp = ReconstructWorldPos(uv_s, sd);
        bool   within   = (z_lo <= sd);
        if (!within) {
            float3 ray_wp = ReconstructWorldPos(uv_s, z_lo);   // レイの surface 最接近点
            within = (distance(ray_wp, scene_wp) <= thicknessW);
        }
        if (within) {
            // 起点至近の交差は self-reflection なので除外
            if (distance(scene_wp, wp) > 0.05) {
                hit     = true;
                hit_uv  = uv_s;
                hitFrac = i / marchMax;
                break;
            }
            continue;                                  // self-hit → 行進継続
        }
        break;                                         // surface 奥 (occluded) → 終了
    }
    if (!hit) return float4(0, 0, 0, 0);

    float3 hit_color = scene_color.SampleLevel(scene_color_sampler, hit_uv, 0).rgb;
    // 画面端フェード (反射先が画面端に近いほど薄める)
    float2 d2e       = min(hit_uv, 1.0 - hit_uv);
    float  edge_fade = saturate(min(d2e.x, d2e.y) * 8.0);
    // 距離フェード: 遠くまで march した反射ほど薄める。長い反射の急な打ち切りや
    // 量子化ノイズを目立たなくし、広い水面で反射が不自然に途切れない。
    float  dist_fade = saturate(1.0 - hitFrac * hitFrac);
    return float4(hit_color * params.x * edge_fade * dist_fade, 1.0);
}
)";

// temporal accumulation。jitter 付き raw SSR を、前フレームの履歴と
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
    float4x4 prev_view_proj;     // reprojection 用
    float4   temporal_params;    // x=texel_w, y=texel_h, z=blend_factor, w=motion_mode
};

Texture2D    current_ssr : register(t0);   // jitter 付き raw SSR (今フレーム)
Texture2D    history_ssr : register(t1);   // 前フレームの temporal 結果
Texture2D    scene_depth : register(t2);   // motion_mode では motion vector として再解釈
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
    float4 cur = current_ssr.SampleLevel(current_ssr_sampler, v.uv, 0);

    // motion texture モードなら scene_depth slot を motion vector として
    // 再解釈し、動く mesh の反射も history を正しく追従させる (SSGI temporal と同形)。
    // 非モードは従来の camera-only depth reprojection。
    float2 huv = v.uv;
    if (temporal_params.w >= 0.5) {
        float2 mv = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).rg;
        huv = v.uv + mv;
    } else {
        huv = ReprojectUv(v.uv);
    }
    bool offscreen = any(huv < 0.0) || any(huv > 1.0);
    if (offscreen) huv = v.uv;
    float4 hist_unclipped = history_ssr.SampleLevel(history_ssr_sampler, huv, 0);
    float4 hist = hist_unclipped;

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
    float cur_luma = dot(cur.rgb, float3(0.2126, 0.7152, 0.0722));
    float hist_luma = dot(hist_unclipped.rgb, float3(0.2126, 0.7152, 0.0722));
    float relative_delta = abs(cur_luma - hist_luma)
                         / max(max(cur_luma, hist_luma), 0.05);
    float motion_px = length((huv - v.uv)
                    / max(float2(temporal_params.x, temporal_params.y),
                          float2(1e-6, 1e-6)));
    a = max(a, saturate(relative_delta) * 0.75);
    a = max(a, abs(cur.a - hist_unclipped.a) * 0.9);
    a = max(a, saturate(motion_px / 16.0) * 0.4);
    // A current miss must clear a stale hit immediately; otherwise bright
    // reflections leave a one-frame fringe while moving off a silhouette.
    if (cur.a < 0.05 && hist_unclipped.a > 0.05) a = 1.0;
    if (offscreen) a = 1.0;
    return lerp(hist, cur, a);
}
)";

struct FSsrCbLayout {
    FMat4 view_proj;
    FMat4 inv_view_proj;
    FVec4 eye;
    FVec4 params;
    FMat4 prev_view_proj;     // temporal reproject 用
    FVec4 temporal_params;    // x=texel_w, y=texel_h, z=blend_factor
    FVec4 hiz_params;         // x=enabled, y=mip_count, z/w=physical level-0 size
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

TResult<void> FSsr::Init(IRhiDevice& device, EFormat hdr_format, u32 width, u32 height) noexcept {
    m_Device = &device;
    m_HdrFormat = hdr_format;
    m_Width = width;
    m_Height = height;

    if (auto r = CreateOutputRT(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipeline(device); r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size = CBSize<FSsrCbLayout>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, cbd); r.IsErr()) return Err<void>(r.Error());
    else m_Cb = Move(r.Value());

    return Ok();
}

TResult<void> FSsr::CreateOutputRT(IRhiDevice& device, u32 width, u32 height) noexcept {
    m_Output.Reset();
    FTextureDesc td{};
    td.width  = width;
    td.height = height;
    td.format = m_HdrFormat;
    td.is_render_target = true;
    auto r = CreateRhiTexture(device, td);
    if (r.IsErr()) return Err<void>(r.Error());
    m_Output = Move(r.Value());

    // temporal accumulation の history ping-pong
    for (u32 i = 0; i < 2; ++i) {
        m_History[i].Reset();
        auto hr = CreateRhiTexture(device, td);
        if (hr.IsErr()) return Err<void>(hr.Error());
        m_History[i] = Move(hr.Value());
    }
    return Ok();
}

TResult<void> FSsr::CreatePipeline(IRhiDevice& device) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSsrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "FSsr.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr()) return Err<void>(r.Error());
    else m_Vs = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSsrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "FSsr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr()) return Err<void>(r.Error());
    else m_Ps = Move(r.Value());

    FPipelineDesc pd{};
    pd.vs            = m_Vs.Get();
    pd.ps            = m_Ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = m_HdrFormat;
    pd.depth_format  = EFormat::Unknown;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 5;
    pd.cbuffer_names[0] = "SsrCB";
    pd.texture_names[0] = "scene_color";
    pd.texture_names[1] = "scene_depth";
    pd.texture_names[2] = "normal_gbuffer";
    pd.texture_names[3] = "hiz_even";
    pd.texture_names[4] = "hiz_odd";
    pd.static_sampler_count = 5;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[1].filter    = ESamplerFilter::Point;
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    // normal G-buffer は Point sample (silhouette を跨ぐ法線の線形混色を避ける)
    pd.static_samplers[2].filter    = ESamplerFilter::Point;
    pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    // Hi-Z は Point sample (8x8 ブロック境界をまたぐ補間で false skip を防ぐ)
    pd.static_samplers[3].filter    = ESamplerFilter::Point;
    pd.static_samplers[3].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[3].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[4] = pd.static_samplers[3];
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr()) return Err<void>(r.Error());
    else m_Pipeline = Move(r.Value());

    // temporal pipeline (current_ssr + history_ssr + scene_depth → history)。
    // VS は fullscreen-triangle で raw と同形なので m_Vs を再利用。
    FShaderDesc tps_d{};
    tps_d.stage = EShaderStage::Pixel;
    tps_d.hlsl_source = kSsrTemporalHLSL;
    tps_d.entry_point = "PSMain";
    tps_d.debug_name  = "SsrTemporal.PS";
    if (auto r = CreateRhiShader(device, tps_d); r.IsErr()) return Err<void>(r.Error());
    else m_TemporalPs = Move(r.Value());

    FPipelineDesc tpd{};
    tpd.vs            = m_Vs.Get();
    tpd.ps            = m_TemporalPs.Get();
    tpd.topology      = EPrimitiveTopology::TriangleList;
    tpd.rt_format     = m_HdrFormat;
    tpd.depth_format  = EFormat::Unknown;
    tpd.depth_test    = false;
    tpd.depth_write   = false;
    tpd.cull_mode     = ECullMode::None;
    tpd.blend_mode    = EBlendMode::Opaque;
    tpd.cbuffer_slots = 1;
    tpd.texture_slots = 3;
    tpd.cbuffer_names[0] = "SsrCB";
    tpd.texture_names[0] = "current_ssr";
    tpd.texture_names[1] = "history_ssr";
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

void FSsr::Shutdown() noexcept {
    m_TemporalPipeline.Reset();
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_TemporalPs.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    for (auto& h : m_History) h.Reset();
    m_Output.Reset();
    m_TemporalFrame = 0;
    m_Device = nullptr;
}

TResult<void> FSsr::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 320, "FSsr::Resize before Init");
    if (width == m_Width && height == m_Height) return Ok();
    m_Width = width;
    m_Height = height;
    m_TemporalFrame = 0;     // history は size 違いで使えないので reset
    return CreateOutputRT(*m_Device, width, height);
}

void FSsr::Render(IRhiDevice& /*device*/, IRhiCommandList& cl,
                  IRhiTexture& scene_color, IRhiTexture& scene_depth,
                  IRhiTexture& normal_gbuffer,
                  const FMat4& view_proj, const FMat4& inv_view_proj,
                  const FMat4& prev_view_proj,
                  FVec3 eye, f32 intensity,
                  IRhiTexture* motion_texture,
                  IRhiTexture* hiz_even,
                  IRhiTexture* hiz_odd,
                  u32 hiz_mip_count) noexcept {
    if (!m_Output || !m_History[0] || !m_History[1] ||
        !m_Pipeline || !m_TemporalPipeline || !m_Cb) return;

    // per-frame jitter 値: frac(frame * 黄金比) で低 discrepancy に散らす。
    // frame は 1024 で wrap して f32 精度内に収める。
    const u32 jf     = m_TemporalFrame & 1023u;
    const f32 jt     = static_cast<f32>(jf) * 0.61803399f;
    const f32 jitter = jt - static_cast<f32>(static_cast<u32>(jt));

    // A single legacy texture keeps the old level-0 path. Full hierarchy is
    // enabled only when both parity textures and a valid mip count are present.
    u32 usable_hiz_mips = hiz_even ? (hiz_mip_count > 0 ? hiz_mip_count : 1u) : 0u;
    if (!hiz_odd && usable_hiz_mips > 1u) usable_hiz_mips = 1u;
    if (hiz_even && usable_hiz_mips > hiz_even->MipLevels())
        usable_hiz_mips = hiz_even->MipLevels();
    if (hiz_odd && usable_hiz_mips > hiz_odd->MipLevels())
        usable_hiz_mips = hiz_odd->MipLevels();
    const f32 hiz_enabled = usable_hiz_mips > 0u ? 1.0f : 0.0f;
    const f32 hiz_w = hiz_even ? static_cast<f32>(hiz_even->Width()) : 0.0f;
    const f32 hiz_h = hiz_even ? static_cast<f32>(hiz_even->Height()) : 0.0f;

    FSsrCbLayout data{};
    data.view_proj      = view_proj;
    data.inv_view_proj  = inv_view_proj;
    data.eye            = FVec4{eye.x, eye.y, eye.z, 1};
    data.params         = FVec4{intensity, /*max_dist=*/12.0f, jitter, /*thickness_world=*/0.3f};
    data.prev_view_proj = prev_view_proj;
    data.temporal_params = FVec4{ 1.0f / static_cast<f32>(m_Width),
                                 1.0f / static_cast<f32>(m_Height),
                                 /*blend=*/0.1f,
                                 /*motion_mode=*/motion_texture ? 1.0f : 0.0f };
    data.hiz_params     = FVec4{hiz_enabled, static_cast<f32>(usable_hiz_mips),
                                hiz_w, hiz_h};
    m_Cb->Update(&data, sizeof(data));

    // Pass 1: raw SSR (jitter 付き march) → m_Output
    cl.BeginRenderToTexture(*m_Output, FClearColor{0, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, scene_color);
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, normal_gbuffer);
    // Hi-Z slots: disabled paths still receive valid fallback SRVs for strict
    // backends. A legacy single texture is sampled only at level 0.
    cl.SetTexture(3, hiz_even ? *hiz_even : scene_depth);
    cl.SetTexture(4, hiz_odd ? *hiz_odd : (hiz_even ? *hiz_even : scene_depth));
    cl.Draw(3);
    cl.EndRenderToTexture(*m_Output);

    // Pass 2: temporal accumulation → m_History[cur]
    const u32 cur  = m_TemporalFrame & 1u;
    const u32 prev = cur ^ 1u;
    // Cold-start (frame 0): history 未初期化なので raw (m_Output) を history slot に
    // bind する。reproject はほぼ identity、clamp 後 lerp(raw, raw, a)=raw で garbage 排除。
    IRhiTexture* hist_in = (m_TemporalFrame == 0u) ? m_Output.Get() : m_History[prev].Get();
    cl.BeginRenderToTexture(*m_History[cur], FClearColor{0, 0, 0, 0}, nullptr, 1.0f);
    cl.SetPipeline(*m_TemporalPipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_Output);        // current (jitter 付き raw)
    cl.SetTexture(1, *hist_in);        // history (or raw on frame 0)
    // motion texture (あれば) で動く mesh の反射 ghost を消す。
    // shader が temporal_params.w で解釈を切替えるので PSO の slot 数は不変。
    cl.SetTexture(2, motion_texture ? *motion_texture : scene_depth);
    cl.Draw(3);
    cl.EndRenderToTexture(*m_History[cur]);

    ++m_TemporalFrame;
}

} // namespace acs
