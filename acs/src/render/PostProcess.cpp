// SPDX-License-Identifier: Apache-2.0
// CPostProcess (Bloom + ACES Tonemap) 実装
#include "render/PostProcess.h"
#include "render/RenderGraphTransientAlias.h"
#include "foundation/Move.h"
#include "foundation/Log.h"
#include "math/Vec.h"
#include "render/TemporalHistory.h"

#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif

#include <cmath>
#include <cstring>

namespace acs {

namespace {

f32 ClampFinite(f32 value, f32 fallback, f32 minimum,
                f32 maximum) noexcept {
    if (!std::isfinite(value)) value = fallback;
    if (value < minimum) value = minimum;
    if (value > maximum) value = maximum;
    return value;
}

FVec3 ClampFinite(FVec3 value, FVec3 fallback, f32 minimum,
                  f32 maximum) noexcept {
    return FVec3{
        ClampFinite(value.x, fallback.x, minimum, maximum),
        ClampFinite(value.y, fallback.y, minimum, maximum),
        ClampFinite(value.z, fallback.z, minimum, maximum),
    };
}

bool MatrixIsFinite(const FMat4& value) noexcept {
    for (u32 row = 0; row < 4; ++row) {
        for (u32 column = 0; column < 4; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

bool MatrixHasFiniteInverse(const FMat4& value) noexcept {
    return MatrixIsFinite(value) && MatrixIsFinite(Inverse(value));
}

// 全画面 3 角形 (頂点バッファ無しで 3 頂点を SV_VertexID から作る)
const char* kFullscreenVS = R"(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID) {
    VSOut o;
    // (0,0)→(0,0), (2,0)→(1,0), (0,2)→(0,1) で全画面を覆う
    float2 uv = float2((vid << 1) & 2, vid & 2);
    o.uv = uv;
    o.pos = float4(uv * 2.0 - 1.0, 0.0, 1.0);
    o.pos.y = -o.pos.y;
    return o;
}
)";

// 抽出: 輝度 > threshold のみを通す (Knee curve でソフトヒザ)
const char* kExtractPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    float4 params1;   // x=gamma, y=texel_w, z=texel_h, w=pad
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 SafeHdr(float3 color) {
    // Reject non-finite input locally instead of allowing one malformed
    // emissive pixel to poison the entire bloom chain.  Do not use the FP16
    // storage limit as a finiteness test: later FP32 exposure math may validly
    // exceed 65504 before tone mapping.
    // Use an ordered comparison instead of the HLSL finiteness intrinsic.  The raw DX12
    // backend still compiles through FXC/SM5, where the intrinsic has produced
    // an all-false vector in optimized pixel shaders on some drivers.  NaN and
    // infinity both fail this comparison, while the range is far beyond any
    // physically useful HDR radiance.
    return all(abs(color) < 1.0e30) ? max(color, 0.0) : 0.0;
}

float4 PSMain(VSOut v) : SV_TARGET {
    // The destination is half resolution. Prefilter its corresponding 2x2
    // source footprint so sub-pixel highlights do not blink during camera motion.
    float2 texel = float2(params1.y, params1.z);
    // `src` already includes adapted exposure when auto exposure is enabled,
    // while params0.w is the common manual exposure / EV compensation. Apply
    // that manual term before thresholding so bloom and the tonemapped scene
    // respond to the same final exposure. The neutral value remains exactly
    // one and the sanitized CPU contract bounds the multiplier.
    float manual_exposure = params0.w;
    float3 c0 = SafeHdr(src.SampleLevel(src_sampler, v.uv + texel * float2(-0.5, -0.5), 0).rgb) * manual_exposure;
    float3 c1 = SafeHdr(src.SampleLevel(src_sampler, v.uv + texel * float2( 0.5, -0.5), 0).rgb) * manual_exposure;
    float3 c2 = SafeHdr(src.SampleLevel(src_sampler, v.uv + texel * float2(-0.5,  0.5), 0).rgb) * manual_exposure;
    float3 c3 = SafeHdr(src.SampleLevel(src_sampler, v.uv + texel * float2( 0.5,  0.5), 0).rgb) * manual_exposure;
    // Karis weighting prevents an isolated firefly from dominating the footprint.
    float w0 = rcp(1.0 + max(c0.r, max(c0.g, c0.b)));
    float w1 = rcp(1.0 + max(c1.r, max(c1.g, c1.b)));
    float w2 = rcp(1.0 + max(c2.r, max(c2.g, c2.b)));
    float w3 = rcp(1.0 + max(c3.r, max(c3.g, c3.b)));
    float3 c = (c0 * w0 + c1 * w1 + c2 * w2 + c3 * w3)
             / max(w0 + w1 + w2 + w3, 1e-5);
    float l = max(max(c.r, c.g), c.b);
    float t = params0.x;                       // threshold
    // soft-knee prefilter (Unity/UE 風): 閾値付近をなめらかに立ち上げ、bloom の onset を自然に。
    float knee = max(t * 0.6, 1e-4);           // knee 幅 = threshold の 60%
    float soft = clamp(l - t + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee);
    float contrib = max(soft, l - t);          // 閾値以下は soft 曲線、以上は線形
    float w = contrib / max(l, 1e-4);          // over-threshold 比で重み付け (firefly 抑制)
    return float4(c * w, 1.0);
}
)";

// Downsample: 13-tap の partial-box filter。
// 5 つの partial-box average を輝度重み付きで blend する。
//
// フィルター配置 (t = 元テクセル 1 個分):
//   A . B . C
//   . J . K .
//   D . E . F      (center = E)
//   . L . M .
//   G . H . I
//
// 5 box (各 4-tap average) を:
//   center box (JKLM): weight 0.5
//   4 corner box (ABDE, BCEF, DEGH, EFHI): each weight 0.125
const char* kDownsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;   // y=texel_w, z=texel_h
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float2 t = float2(params1.y, params1.z);
    // 外周 (radius 2 texel)
    float3 a = src.Sample(src_sampler, v.uv + t * float2(-2.0, -2.0)).rgb;
    float3 b = src.Sample(src_sampler, v.uv + t * float2( 0.0, -2.0)).rgb;
    float3 c = src.Sample(src_sampler, v.uv + t * float2( 2.0, -2.0)).rgb;
    float3 d = src.Sample(src_sampler, v.uv + t * float2(-2.0,  0.0)).rgb;
    float3 e = src.Sample(src_sampler, v.uv).rgb;
    float3 f = src.Sample(src_sampler, v.uv + t * float2( 2.0,  0.0)).rgb;
    float3 g = src.Sample(src_sampler, v.uv + t * float2(-2.0,  2.0)).rgb;
    float3 h = src.Sample(src_sampler, v.uv + t * float2( 0.0,  2.0)).rgb;
    float3 i = src.Sample(src_sampler, v.uv + t * float2( 2.0,  2.0)).rgb;
    // 内側 (radius 1 texel) — 中心 box
    float3 j = src.Sample(src_sampler, v.uv + t * float2(-1.0, -1.0)).rgb;
    float3 k = src.Sample(src_sampler, v.uv + t * float2( 1.0, -1.0)).rgb;
    float3 l = src.Sample(src_sampler, v.uv + t * float2(-1.0,  1.0)).rgb;
    float3 m = src.Sample(src_sampler, v.uv + t * float2( 1.0,  1.0)).rgb;

    // 5 partial box average
    float3 c0 = (j + k + l + m) * 0.25;   // 中心 box (weight 0.5)
    float3 c1 = (a + b + d + e) * 0.25;   // 左上
    float3 c2 = (b + c + e + f) * 0.25;   // 右上
    float3 c3 = (d + e + g + h) * 0.25;   // 左下
    float3 c4 = (e + f + h + i) * 0.25;   // 右下

    float3 sum = c0 * 0.5 + (c1 + c2 + c3 + c4) * 0.125;
    return float4(sum, 1.0);
}
)";

// Upsample: 下段を円形フィルタし、上段の元画像と energy-preserving に合成する。
const char* kUpsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // z=radius
    float4 params1;   // y=texel_w, z=texel_h
    float4 params2;
    float4 params3;   // z=scatter
};
Texture2D    src : register(t0);
Texture2D    base_tex : register(t1);
SamplerState src_sampler : register(s0);
SamplerState base_tex_sampler : register(s1);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float r = params0.z;
    float2 t = float2(params1.y, params1.z) * r;
    // 円形カーネル (中心 + 2 リング、計 13 tap)。軸整列の tent/box だと明るい点光源 (太陽) の
    // ブルームが «四角» に広がるため、円周上に散らして «丸い» ブルームにする。
    float3 sum = src.Sample(src_sampler, v.uv).rgb * 0.20;
    // 内リング (半径 1)、6 方向、各 0.085
    sum += src.Sample(src_sampler, v.uv + float2( 1.000,  0.000) * t).rgb * 0.085;
    sum += src.Sample(src_sampler, v.uv + float2( 0.500,  0.866) * t).rgb * 0.085;
    sum += src.Sample(src_sampler, v.uv + float2(-0.500,  0.866) * t).rgb * 0.085;
    sum += src.Sample(src_sampler, v.uv + float2(-1.000,  0.000) * t).rgb * 0.085;
    sum += src.Sample(src_sampler, v.uv + float2(-0.500, -0.866) * t).rgb * 0.085;
    sum += src.Sample(src_sampler, v.uv + float2( 0.500, -0.866) * t).rgb * 0.085;
    // 外リング (半径 2、30°オフセット)、6 方向、各 0.0483
    sum += src.Sample(src_sampler, v.uv + float2( 1.732,  1.000) * t).rgb * 0.0483;
    sum += src.Sample(src_sampler, v.uv + float2( 0.000,  2.000) * t).rgb * 0.0483;
    sum += src.Sample(src_sampler, v.uv + float2(-1.732,  1.000) * t).rgb * 0.0483;
    sum += src.Sample(src_sampler, v.uv + float2(-1.732, -1.000) * t).rgb * 0.0483;
    sum += src.Sample(src_sampler, v.uv + float2( 0.000, -2.000) * t).rgb * 0.0483;
    sum += src.Sample(src_sampler, v.uv + float2( 1.732, -1.000) * t).rgb * 0.0483;
    // lerp なら一定輝度入力は何段あっても一定のまま。旧 additive の mip 数比例の
    // 白化を防ぎつつ、scatter で広がりだけを調整できる。
    float3 base = base_tex.Sample(base_tex_sampler, v.uv).rgb;
    return float4(lerp(base, sum, saturate(params3.z)), 1.0);
}
)";

// Separable Gaussian blur (DirectXTK 風): 1 次元 13-tap ガウス。H パス + V パスで «円形» の
// 2D ガウスになる。box mip と違い点光源 (発光体/太陽) が «四角» にならない。
// オフセット方向は params1.yz (H=(texel_w,0)/V=(0,texel_h))、広がりは params0.z。
const char* kGaussianBlurPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // z = blur amount (texel step スケール)
    float4 params1;   // y,z = directional texel offset (H or V)
    float4 params2; float4 params3;
    float4 cg0; float4 cg_lift; float4 cg_gain; float4 cas_params; float4 taa_params;
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 PSMain(VSOut v) : SV_TARGET {
    float2 step = float2(params1.y, params1.z) * max(params0.z, 0.01);
    // sigma~3 の正規化 13-tap ガウス重み (中心 + 6 対)
    const float w0 = 0.137000;
    const float w1 = 0.130000, w2 = 0.110000, w3 = 0.083000;
    const float w4 = 0.056000, w5 = 0.034000, w6 = 0.018500;
    float3 c = src.Sample(src_sampler, v.uv).rgb * w0;
    c += src.Sample(src_sampler, v.uv + step * 1.0).rgb * w1;
    c += src.Sample(src_sampler, v.uv - step * 1.0).rgb * w1;
    c += src.Sample(src_sampler, v.uv + step * 2.0).rgb * w2;
    c += src.Sample(src_sampler, v.uv - step * 2.0).rgb * w2;
    c += src.Sample(src_sampler, v.uv + step * 3.0).rgb * w3;
    c += src.Sample(src_sampler, v.uv - step * 3.0).rgb * w3;
    c += src.Sample(src_sampler, v.uv + step * 4.0).rgb * w4;
    c += src.Sample(src_sampler, v.uv - step * 4.0).rgb * w4;
    c += src.Sample(src_sampler, v.uv + step * 5.0).rgb * w5;
    c += src.Sample(src_sampler, v.uv - step * 5.0).rgb * w5;
    c += src.Sample(src_sampler, v.uv + step * 6.0).rgb * w6;
    c += src.Sample(src_sampler, v.uv - step * 6.0).rgb * w6;
    return float4(c, 1.0);
}
)";

// TAA resolve: current HDR + history HDR + depth →
// reprojected & neighborhood-clamped blend。
//
// 別 CB (TaaReproj at b1) に view_proj + inv_view_proj + prev_view_proj を入れて、
// camera motion 由来の motion vec を計算。reproject_enabled = 0 のときは motion=0 で
// 静的 reprojection になる。
const char* kTaaResolvePS = R"(
#pragma pack_matrix(row_major)
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;       // y=texel_w, z=texel_h
    float4 params2;
    float4 params3;
    float4 cg0;
    float4 cg_lift;
    float4 cg_gain;
    float4 cas_params;
    float4 taa_params;    // x=blend_factor、y=reproject_enabled、z=motion_texture_mode、w=reactive_mask
};
cbuffer TaaReproj : register(b1) {
    float4x4 taa_inv_view_proj;
    float4x4 taa_prev_view_proj;
    float4 taa_camera_position;
};
Texture2D    current_hdr : register(t0);
Texture2D    history_hdr : register(t1);
Texture2D    scene_depth : register(t2);
Texture2D    reactive_mask : register(t3);
Texture2D    reactive_scene_depth : register(t4);
SamplerState current_hdr_sampler : register(s0);
SamplerState history_hdr_sampler : register(s1);
SamplerState scene_depth_sampler : register(s2);
SamplerState reactive_mask_sampler : register(s3);
SamplerState reactive_scene_depth_sampler : register(s4);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

// YCoCg 変換 + AABB clip (Karis TAA 2014)。RGB の per-channel clamp はクロマゴーストを通すが、
// 輝度/色差を分離した YCoCg で AABB の «中心へ向けて clip» すると色のにじみ尾を強く抑えられる。
float3 SafeHdr(float3 color) {
    return all(abs(color) < 1.0e30) ? max(color, 0.0) : 0.0;
}
float3 RGB2YCoCg(float3 c) {
    return float3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                  0.5  * c.r            - 0.5  * c.b,
                 -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
float3 YCoCg2RGB(float3 c) {
    return float3(c.x + c.y - c.z, c.x + c.z, c.x - c.y - c.z);
}
float3 ClipAABB(float3 amin, float3 amax, float3 p) {
    float3 center = 0.5 * (amax + amin);
    float3 ext    = 0.5 * (amax - amin) + 1e-5;
    float3 d      = p - center;
    float3 ad     = abs(d / ext);
    float  m      = max(ad.x, max(ad.y, ad.z));
    return (m > 1.0) ? center + d / m : p;   // AABB 外なら中心へ向けて境界まで clip
}

float2 ComputeMotionUv(float2 uv) {
    // 現フレームの depth から world pos を復元、前フレームの VP で clip pos を計算、
    // ndc → uv に戻して motion vec を作る。camera 動きのみ反映 (object 動きは見えない)。
    // FXC は helper 内の早期終了を経由した値を未初期化と誤判定することがある。
    // fallback を先に定義し、全分岐を単一の出口に合流させる。
    float2 reprojected_uv = uv;
    float depth = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
    if (depth >= 0.9999) {
        reprojected_uv = uv;                    // sky は motion 0 (history そのまま)
    } else {
        float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
        clip.y = -clip.y;
        float4 wp = mul(clip, taa_inv_view_proj);
        wp.xyz /= max(wp.w, 1e-6);
        float4 prev_clip = mul(float4(wp.xyz, 1.0), taa_prev_view_proj);
        if (prev_clip.w < 1e-4) {
            reprojected_uv = uv;
        } else {
            float2 prev_ndc = prev_clip.xy / prev_clip.w;
            reprojected_uv =
                float2(prev_ndc.x * 0.5 + 0.5, -prev_ndc.y * 0.5 + 0.5);
        }
    }
    return reprojected_uv;
}

float SceneDistanceAt(float2 uv) {
    float depth = reactive_scene_depth.SampleLevel(
        reactive_scene_depth_sampler, uv, 0).r;
    // FXC can flag a helper with an initialized early-exit path as X4000.
    // Publish one explicitly initialized result through a single exit instead.
    float sceneDistance = 1e30;
    if (depth < 1.0) {
        float4 clip = float4(
            uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), depth, 1.0);
        float4 world = mul(clip, taa_inv_view_proj);
        world /= max(abs(world.w), 1e-6);
        sceneDistance = length(world.xyz - taa_camera_position.xyz);
    }
    return sceneDistance;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 cur = SafeHdr(
        current_hdr.SampleLevel(current_hdr_sampler, v.uv, 0).rgb);

    // History を sample する位置を決める:
    //   taa_params.z >= 0.5: motion texture モード。scene_depth slot を
    //     motion vector (camera+object 動き) として再解釈し、そのまま reproject する。
    //     動く mesh も正しく history を引けるので ghost / trail が消える。
    //   taa_params.y >= 0.5: depth reprojection モード (camera 動きのみ)。
    float2 hist_uv = v.uv;
    if (taa_params.z >= 0.5) {
        float2 mv = scene_depth.SampleLevel(scene_depth_sampler, v.uv, 0).rg;
        hist_uv = v.uv + mv;
    } else if (taa_params.y >= 0.5) {
        hist_uv = ComputeMotionUv(v.uv);
    }
    // 画面外に飛んだ (disocclusion / 画面端進入) 場合は history を «棄却» する。
    // ★以前は v.uv に resample して 90% history を blend していたが、それは «間違った位置の» 古い色を
    // 混ぜて端でゴースト/スメアになる。Karis TAA に倣い off-screen は current 100% にする。
    bool offscreen = any(hist_uv < 0.0) || any(hist_uv > 1.0);
    if (offscreen) hist_uv = v.uv;
    float3 hist_unclipped = SafeHdr(
        history_hdr.SampleLevel(history_hdr_sampler, hist_uv, 0).rgb);
    float3 hist = hist_unclipped;

    // Variance clipping in YCoCg. A raw min/max box admits isolated outliers and
    // leaves long chroma trails; intersecting it with mean +/- sigma is much
    // more stable around thin geometry and flashing emissive pixels.
    float2 tx = float2(params1.y, params1.z);
    float3 curY = RGB2YCoCg(cur);
    float3 nmin = curY, nmax = curY;
    float3 moment1 = 0.0;
    float3 moment2 = 0.0;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            float3 c = RGB2YCoCg(SafeHdr(current_hdr.SampleLevel(
                current_hdr_sampler, v.uv + float2(dx, dy) * tx, 0).rgb));
            nmin = min(nmin, c);
            nmax = max(nmax, c);
            moment1 += c;
            moment2 += c * c;
        }
    }
    float3 mean = moment1 / 9.0;
    float3 sigma = sqrt(max(moment2 / 9.0 - mean * mean, 0.0));
    float3 clip_min = max(nmin, mean - sigma * 1.25);
    float3 clip_max = min(nmax, mean + sigma * 1.25);
    hist = YCoCg2RGB(ClipAABB(clip_min, clip_max, RGB2YCoCg(hist)));

    float a = saturate(taa_params.x);
    if (a < 1e-4) a = 0.1;          // ガード (CB 0 で全 history になるのを避ける)
    // Become responsive when reprojected history strongly disagrees with the
    // current sample. This is a lightweight reactive mask for animated
    // emissives/particles where no material mask is available.
    float cur_luma  = max(curY.x, 0.0);
    float hist_luma = max(RGB2YCoCg(hist_unclipped).x, 0.0);
    float luma_delta = abs(cur_luma - hist_luma) / max(max(cur_luma, hist_luma), 0.05);
    float motion_px = length((hist_uv - v.uv) / max(tx, float2(1e-6, 1e-6)));
    a = max(a, saturate(luma_delta * 1.5) * 0.85);
    a = max(a, saturate(motion_px / 24.0) * 0.35);
    // Some effects (volumetric clouds in particular) already own a temporal
    // reconstruction. A second global history produces trails and blurs their
    // full-resolution edge reconstruction. The resolved cloud mask is generated
    // before composition, so it still contains clouds hidden behind terrain.
    // Reproduce the composite's ray-distance visibility test before making a
    // pixel reactive; otherwise the entire foreground loses geometry TAA.
    float reactive = 0.0;
    if (taa_params.w >= 0.5) {
        float sceneDistance = SceneDistanceAt(v.uv);
        float2 reactiveHit = reactive_mask.SampleLevel(
            reactive_mask_sampler, v.uv, 0).rg;
        float tolerance = max(0.05, sceneDistance * 0.001);
        bool cloudVisible = reactiveHit.y >= 0.001 &&
                            reactiveHit.x <= 250000.0 &&
                            reactiveHit.x < sceneDistance - tolerance;
        reactive = cloudVisible ? reactiveHit.y : 0.0;

        // A one-pixel dilation prevents trails at a moving cloud silhouette.
        // Only clear-sky pixels use neighboring raw coverage: on geometry,
        // every reactive decision remains the exact center ray-distance test.
        if (sceneDistance > 250000.0) {
            [unroll]
            for (int ry = -1; ry <= 1; ++ry) {
                [unroll]
                for (int rx = -1; rx <= 1; ++rx) {
                    if (rx == 0 && ry == 0)
                        continue; // center is already in reactiveHit
                    float2 hit = reactive_mask.SampleLevel(
                        reactive_mask_sampler,
                        v.uv + float2(rx, ry) * tx, 0).rg;
                    if (hit.x <= 250000.0)
                        reactive = max(reactive, hit.y);
                }
            }
        }
    }
    a = max(a, smoothstep(0.001, 0.02, reactive));
    if (offscreen) a = 1.0;         // disocclusion: history 棄却 → current 100% (ゴースト防止)
    return float4(lerp(hist, cur, a), 1.0);
}
)";

// Tonemap + cinematic post-FX: chromatic aberration → HDR mix → tonemap → vignette → grain → gamma
const char* kTonemapPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    float4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind (0=ACES, 1=AgX, 2=Reinhard ext)
    float4 params2;   // x=vignette_intensity, y=vignette_radius, z=chromatic_aberration, w=grain_intensity
    float4 params3;   // x=grain_time, y=ssr_intensity, z=bloom_scatter, w=SSR adapted-exposure mode
    float4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    float4 cg_lift;   // xyz=lift (shadow offset)
    float4 cg_gain;   // xyz=gain (highlight multiplier)
    // x=cas_strength、tonemap描画のyzw=選択輪郭色。
    float4 cas_params;
    // TAA描画=x:blend/y:reproject/z:motion/w:reactive、tonemap描画=x:輪郭強さ/y:幅/z:有効。
    float4 taa_params;
};
Texture2D    hdr   : register(t0);
Texture2D    bloom : register(t1);
Texture2D    ssr   : register(t2);
Texture2D    adapted_exposure : register(t3);
Texture2D    selection_mask : register(t4);
SamplerState hdr_sampler   : register(s0);
SamplerState bloom_sampler : register(s1);
SamplerState ssr_sampler   : register(s2);
SamplerState adapted_exposure_sampler : register(s3);
SamplerState selection_mask_sampler : register(s4);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 SafeHdr(float3 color) {
    return all(abs(color) < 1.0e30) ? max(color, 0.0) : 0.0;
}

float3 ACESFilm(float3 x) {
    // Stephen Hill (@self_shadow) ACES fit = WickedEngine の ACESFitted と同形。
    // Narkowicz 2016 近似は過飽和＆色相シフト (赤→橙、ハイライトが白に抜けない) で «filmic でない»
    // 見た目だったため、hue 保存の input/output マトリクス版へ。行列は pack_matrix 非依存にするため
    // 明示 dot 積で展開 (転置の曖昧さを排除)。
    float3 v;
    v.r = dot(x, float3(0.59719, 0.35458, 0.04823));   // ACESInputMat
    v.g = dot(x, float3(0.07600, 0.90834, 0.01566));
    v.b = dot(x, float3(0.02840, 0.13383, 0.83777));
    float3 na = v * (v + 0.0245786) - 0.000090537;       // RRTAndODTFit
    float3 nb = v * (0.983729 * v + 0.4329510) + 0.238081;
    v = na / nb;
    float3 o;
    o.r = dot(v, float3( 1.60475, -0.53108, -0.07367));  // ACESOutputMat
    o.g = dot(v, float3(-0.10208,  1.10813, -0.00605));
    o.b = dot(v, float3(-0.00327, -0.07276,  1.07602));
    return saturate(o);
}

// AgX (Troy Sobotka 2024、Filament の実装に近い形)。
// ACES より highlight 圧縮が緩く、彩度の暴発が少ない (UE5 デフォルトに近い見え方)。
float3 AgxLog(float3 x) {
    return clamp((log2(max(x, 1e-10)) + 12.47393) / (4.026069 + 12.47393), 0.0, 1.0);
}
float3 AgxLook(float3 x) {
    // 6th 次多項式 sigmoid
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return + 15.5 * x4 * x2
           - 40.14 * x4 * x
           + 31.96 * x4
           - 6.868 * x2 * x
           + 0.4298 * x2
           + 0.1191 * x
           - 0.00232;
}
float3 AgxTonemap(float3 x) {
    // AgX's inset/outset transforms are essential: applying the contrast curve
    // independently in linear sRGB shifts hue and over-saturates hot highlights.
    float3 inset;
    inset.r = dot(x, float3(0.8424790623, 0.0784336000, 0.0792237451));
    inset.g = dot(x, float3(0.0423282423, 0.8784686365, 0.0791661275));
    inset.b = dot(x, float3(0.0423756549, 0.0784336000, 0.8791429738));
    float3 encoded = AgxLook(AgxLog(max(inset, 0.0)));

    float3 outset;
    // The reference applies the inverse as mul(value, matrix), so each output
    // channel is a dot product with one matrix column.
    outset.r = dot(encoded, float3( 1.1968790051, -0.0980208811, -0.0990297441));
    outset.g = dot(encoded, float3(-0.0528968518,  1.1519031299, -0.0989611768));
    outset.b = dot(encoded, float3(-0.0529716355, -0.0980434501,  1.1510736726));
    // The common output path applies the sRGB OETF, so return linear display RGB.
    return saturate(pow(max(outset, 0.0), 2.2));
}

// Reinhard 拡張 (Lottes/Hable 風)
float3 ReinhardExt(float3 x, float white2) {
    return (x * (1.0 + x / white2)) / (1.0 + x);
}

float3 Tonemap(float3 c, int kind) {
    // Keep a defined default and one exit so FXC can prove every selector
    // branch initializes the result.
    float3 result = float3(0.0, 0.0, 0.0);
    if (kind == 1) {
        result = AgxTonemap(c);
    } else if (kind == 2) {
        result = ReinhardExt(c, 16.0);
    } else {
        result = ACESFilm(c);
    }
    return result;
}

// FColor grading: tonemap 後 (LDR) に適用する ASC-CDL 風補正。
// 適用順序:
//   1) lift + gain (SOP の S/O 部分): shadow offset + highlight multiplier
//   2) contrast (SOP の Power 相当、簡易 pivot=0.5 線形): スロープ調整
//   3) temperature / tint: 色温度シフト (RGB シフト方式、CIE chroma は近似)
//   4) saturation (SAT、最後): Rec.709 luminance との lerp
// 注: 標準 CDL は SOP → SAT。本実装は temp/tint を SAT の前に置いている (映画 teal-orange
// grade の典型) ため、sat=0 (モノクロ化) でも temp/tint shift が完全には消えない。
// neutral 期待挙動が要るなら SAT 後にもう一度 luma 取り直しが必要だが、現状の cinematic
// 用途では問題なし。
float3 ColorGrade(float3 c) {
    // lift + gain
    c = c * cg_gain.rgb + cg_lift.rgb;
    // contrast (pivot=0.5)
    c = (c - 0.5) * cg0.y + 0.5;
    c = max(c, 0.0);
    // temperature: 暖色 (+1) で red↑ / blue↓
    c.r += cg0.z * 0.10;     // temp +1 → +0.1 red
    c.b -= cg0.z * 0.10;     // temp +1 → -0.1 blue
    // tint: ASC-CDL / DaVinci 規約に合わせ +1 で magenta (R+B↑、G↓)、-1 で green
    c.g -= cg0.w * 0.10;     // tint +1 → -0.1 green
    c.r += cg0.w * 0.05;
    c.b += cg0.w * 0.05;
    // saturation (Rec.709 luminance)
    float lum = dot(c, float3(0.2126, 0.7152, 0.0722));
    c = lerp(float3(lum, lum, lum), c, cg0.x);
    return max(c, 0.0);
}

// Interleaved Gradient Noise (Jimenez 2014) — film grain / dither 用。低コストで
// blue-noise に近い分布になり、白ノイズより縞・塊が出にくい。
//   p = screen pixel 座標 (v.pos.xy)。解像度非依存の 1px 粒。
//   t = フレーム時間。パターンを毎フレーム平行移動して時間方向にちらつかせる。
float IGN(float2 p, float t) {
    p += t * float2(5.588238, 1.715728);
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) Chromatic aberration: 中心から放射方向に R/G/B を分けてサンプル。
    //    中心はシャープ・周辺ほど分離が強くなるよう pow で形状化し、各チャンネルを
    //    放射方向に 2 タップ平均して硬いフリンジを滑らかなスメアにする (安物の単純
    //    1 タップずらしを避ける)。
    float2 center = float2(0.5, 0.5);
    float2 dir = v.uv - center;
    float dist_radial = length(dir);
    float ca = params2.z;
    float3 hdr_col;
    if (ca > 1e-5) {
        // dir はもともと端ほど大きい。さらに pow(.,1.5) (convex) で中心をシャープに
        // 保ち、分離を周辺へ寄せる。
        float  shape = pow(saturate(dist_radial * 1.41421356), 1.5);   // 0(中心)→1(端)
        float2 ofs   = dir * ca * (1.0 + shape);
        float3 far_positive = SafeHdr(
            hdr.Sample(hdr_sampler, v.uv + ofs).rgb);
        float3 near_positive = SafeHdr(
            hdr.Sample(hdr_sampler, v.uv + ofs * 0.5).rgb);
        float3 center_sample = SafeHdr(
            hdr.Sample(hdr_sampler, v.uv).rgb);
        float3 near_negative = SafeHdr(
            hdr.Sample(hdr_sampler, v.uv - ofs * 0.5).rgb);
        float3 far_negative = SafeHdr(
            hdr.Sample(hdr_sampler, v.uv - ofs).rgb);
        hdr_col.r = (far_positive.r + near_positive.r) * 0.5;
        hdr_col.g = center_sample.g;
        hdr_col.b = (far_negative.b + near_negative.b) * 0.5;
    } else {
        hdr_col = SafeHdr(hdr.Sample(hdr_sampler, v.uv).rgb);
    }
    hdr_col *= params0.w;       // exposure

    // CAS sharpening (AMD FSR 簡略版、HDR-aware):
    // 注: center に CA が乗っていて neighbor が乗っていない非対称があるが、
    // 既定 ca=0.002 (≒ 0.4 texel) の小オフセットで実害は無視できる。CA を強くするとき
    // (>0.01) は CAS と併用しないか、neighbor 側にも CA を適用するべき (4 倍コスト)。
    // AMD FSR 原典は LDR (0..1) 前提で `min(amin, 1-amax)` を headroom 推定に使う。
    // HDR で amax > 1 の場合 `1-amax < 0` で ratio=0 になり sharpen が消える bug がある。
    // → 修正: amp 計算は Reinhard 圧縮 (`x / (1+x)`) で 0..1 域に写してから行い、
    //         sharpen 本体は HDR original 値に対して適用する。
    if (cas_params.x > 1e-4) {
        float2 px = float2(params1.y, params1.z);
        // neighbor は CA なしで raw HDR を read (CA は装飾、CAS は構造保持)
        float3 nN = SafeHdr(hdr.SampleLevel(hdr_sampler, v.uv + float2(0,    -px.y), 0).rgb) * params0.w;
        float3 nS = SafeHdr(hdr.SampleLevel(hdr_sampler, v.uv + float2(0,     px.y), 0).rgb) * params0.w;
        float3 nE = SafeHdr(hdr.SampleLevel(hdr_sampler, v.uv + float2( px.x, 0   ), 0).rgb) * params0.w;
        float3 nW = SafeHdr(hdr.SampleLevel(hdr_sampler, v.uv + float2(-px.x, 0   ), 0).rgb) * params0.w;
        // Reinhard 圧縮 (`x / (1+x)`) で 0..1 域に写す。amax < 1 が保証されるので
        // `1 - amax > 0` で AMD FSR の headroom 計算が破綻しない。
        float3 rC = hdr_col / (1.0 + hdr_col);
        float3 rN = nN      / (1.0 + nN);
        float3 rS = nS      / (1.0 + nS);
        float3 rE = nE      / (1.0 + nE);
        float3 rW = nW      / (1.0 + nW);
        float3 amin = min(min(min(rN, rS), min(rE, rW)), rC);
        float3 amax = max(max(max(rN, rS), max(rE, rW)), rC);
        float3 ratio = saturate(min(amin, 1.0 - amax) / max(amax, 1e-5));
        float3 amp = sqrt(ratio);
        float3 w = -amp * (cas_params.x * 0.125);    // 負係数、neighbor を減算で sharpen
        // sharpen 本体は HDR original 値に対して適用 (Reinhard で評価した amp/w を使う)
        hdr_col = (hdr_col + (nN + nS + nE + nW) * w) / (1.0 + 4.0 * w);
    }

    hdr_col = SafeHdr(hdr_col);
    float3 bloom_col = SafeHdr(
        bloom.Sample(bloom_sampler, v.uv).rgb) * params0.y;
    // SSR is authored in the same scene-linear domain as hdr. SceneInput()
    // already contains adapted exposure, so apply that 1x1 value to the
    // separately supplied SSR exactly once, then apply the common manual
    // exposure/EV compensation exactly once. The mode is enabled only when a
    // valid SSR texture and a completed auto-exposure result both exist.
    float ssr_exposure = params0.w;
    [branch]
    if (params3.w >= 0.5) {
        float adapted = adapted_exposure.SampleLevel(
            adapted_exposure_sampler, float2(0.5, 0.5), 0).r;
        adapted = abs(adapted) <= 65504.0
            ? max(adapted, 0.0)
            : 1.0;
        ssr_exposure *= adapted;
    }
    float3 ssr_col = SafeHdr(
        ssr.Sample(ssr_sampler, v.uv).rgb) *
        (params3.y * ssr_exposure);
    // Tone-map arithmetic is FP32, so values above FP16's storage limit are
    // valid here. Saturate only the final finite radiance entering the fit.
    float3 mixed = min(SafeHdr(hdr_col + bloom_col + ssr_col), 65504.0);

    // 2) Tonemap
    int kind = (int)params1.w;
    float3 mapped = Tonemap(mixed, kind);

    // 2.5) FColor grading (tonemap 後 LDR で適用)
    mapped = ColorGrade(mapped);

    // 3) Vignette (radial darkening)
    float vig_r = max(params2.y, 1e-4);
    float vig_i = params2.x;
    float vig = smoothstep(1.0, vig_r, dist_radial * 1.414);     // 1.414 ~= sqrt(2)
    mapped *= lerp(1.0 - vig_i, 1.0, vig);

    // 4) Film grain (Gamma 前): screen-space IGN で解像度非依存の 1px 粒。
    //    grain_time で毎フレーム動かし、luma に応じて量を変えて暗部を潰さない。
    float g_i = params2.w;
    if (g_i > 1e-5) {
        float n    = IGN(v.pos.xy, params3.x) - 0.5;
        float luma = dot(mapped, float3(0.299, 0.587, 0.114));
        mapped += n * g_i * (0.35 + 0.65 * luma);
    }

    // 5) sRGB encode: 真の sRGB OETF (WickedEngine ApplySRGBCurve と同形)。swapchain は plain UNORM
    //    なので HW sRGB は無く、ここで一度だけエンコード。pow(1/2.2) は shadow の toe が無く darks を
    //    僅かに潰す (muddy) ので、線形 toe + 1/2.4 ガンマの正規 sRGB 曲線へ。
    {
        float3 lin = max(mapped, 0.0);
        float3 hi  = 1.055 * pow(lin, 1.0 / 2.4) - 0.055;
        float3 lo  = 12.92 * lin;
        mapped = float3(lin.r < 0.0031308 ? lo.r : hi.r,
                        lin.g < 0.0031308 ? lo.g : hi.g,
                        lin.b < 0.0031308 ? lo.b : hi.b);
    }

    // The public gamma control is a display trim around the neutral sRGB
    // transfer. gamma=2.2 leaves the standards-based OETF above unchanged;
    // larger values brighten and smaller values darken without replacing its
    // linear toe with an inaccurate pure power curve.
    mapped = pow(max(mapped, 0.0), 2.2 / max(params1.x, 1.0));

    // 5.5) depth test済みmaskの外側だけを固定4 ringで膨張する。
    // cas_params.yzw=表示色、taa_params.xyz=強さ/pixel幅/有効flag。
    [branch]
    if (taa_params.z >= 0.5) {
        float center_mask = selection_mask.SampleLevel(selection_mask_sampler, v.uv, 0).a;
        float neighbor_mask = 0.0;
        int rings = (int)ceil(clamp(taa_params.y, 1.0, 4.0));
        [unroll]
        for (int ring = 1; ring <= 4; ++ring) {
            if (ring > rings) continue;
            float2 offset = float2(params1.y, params1.z) * ring;
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2( offset.x, 0.0), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2(-offset.x, 0.0), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2(0.0,  offset.y), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2(0.0, -offset.y), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2( offset.x,  offset.y), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2(-offset.x,  offset.y), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2( offset.x, -offset.y), 0).a);
            neighbor_mask = max(neighbor_mask, selection_mask.SampleLevel(selection_mask_sampler, v.uv + float2(-offset.x, -offset.y), 0).a);
        }
        float outline = saturate(neighbor_mask - center_mask);
        mapped = lerp(mapped, saturate(cas_params.yzw), saturate(outline * taa_params.x));
    }

    // 6) Dither: 8-bit 量子化前に ±1 LSB の三角分布 (TPDF) ノイズを足し、空・bloom の裾・
    //    vignette・グレーディングのシャドウに出る等高線状バンディングを消す。ほぼゼロコスト
    //    で「安っぽさ」に最も効く。2 つの IGN を独立化して足すと三角分布 (TPDF) になる。
    //    ※ d2 は軸別オフセット + 別の時間位相にする。同一定数を両軸へ足すと IGN の滑らかな
    //      勾配上を平行移動するだけで d1 と相関し、TPDF にならないため。
    float d1   = IGN(v.pos.xy, params3.x);
    float d2   = IGN(v.pos.xy + float2(113.0, 71.0), params3.x * 0.37 + 0.5);
    mapped += (d1 + d2 - 1.0) * (1.0 / 255.0);

    return float4(mapped, 1.0);
}
)";

// ==== Auto-exposure ====
// シーンの平均輝度を GPU で測定し、露出を自動算出する。3 種のパスで構成:
//   1) Luma extract/downsample: m_HdrRt → log2 輝度 → mip chain で 1x1 まで縮約
//   2) Exposure adapt: 1x1 平均輝度から目標露出を出し、前フレーム露出へ指数補間
//   3) Exposure apply: m_HdrRt に露出を掛けて m_ExposedRt へ
// tonemapはHDR、bloom、SSR、露出、任意の選択maskを読む。

// Luma extract: HDR → log2 輝度。出力 texel が覆う 2x2 source 領域を 4-tap 平均。
// 幾何平均 (log 空間平均) にすることで少数の高輝度ピクセルに過敏にならない。
const char* kLumaExtractPS = R"(
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;   // y=texel_w, z=texel_h (source = m_HdrRt)
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float LogLuma(float3 c) {
    c = all(abs(c) <= 65504.0) ? max(c, 0.0) : 0.0;
    float l = dot(c, float3(0.2126, 0.7152, 0.0722));
    return log2(max(l, 1e-4));
}
float4 PSMain(VSOut v) : SV_TARGET {
    float2 t = float2(params1.y, params1.z);
    float a = LogLuma(src.SampleLevel(src_sampler, v.uv + float2(-0.5,-0.5) * t, 0).rgb);
    float b = LogLuma(src.SampleLevel(src_sampler, v.uv + float2( 0.5,-0.5) * t, 0).rgb);
    float c = LogLuma(src.SampleLevel(src_sampler, v.uv + float2(-0.5, 0.5) * t, 0).rgb);
    float d = LogLuma(src.SampleLevel(src_sampler, v.uv + float2( 0.5, 0.5) * t, 0).rgb);
    return float4((a + b + c + d) * 0.25, 0, 0, 0);
}
)";

// Luma downsample: log2 輝度を 4-tap box average で半分に縮約。
const char* kLumaDownsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;   // y=texel_w, z=texel_h (source mip)
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float2 t = float2(params1.y, params1.z);
    float a = src.SampleLevel(src_sampler, v.uv + float2(-0.5,-0.5) * t, 0).r;
    float b = src.SampleLevel(src_sampler, v.uv + float2( 0.5,-0.5) * t, 0).r;
    float c = src.SampleLevel(src_sampler, v.uv + float2(-0.5, 0.5) * t, 0).r;
    float d = src.SampleLevel(src_sampler, v.uv + float2( 0.5, 0.5) * t, 0).r;
    return float4((a + b + c + d) * 0.25, 0, 0, 0);
}
)";

// Exposure adapt: 平均 log 輝度 → 目標露出 → eye adaptation (前フレームへ指数補間)。
// 1x1 RT へ出力。warm=0 (cold start) のときは補間せず目標を直接採用する。
const char* kExposurePS = R"(
cbuffer AutoExp : register(b0) {
    float4 a0;   // x=key, y=min_exp, z=max_exp, w=speed
    float4 a1;   // x=dt, y=warm (0=cold start), zw=pad
};
Texture2D    avg_luma : register(t0);
Texture2D    prev_exp : register(t1);
SamplerState avg_luma_sampler : register(s0);
SamplerState prev_exp_sampler : register(s1);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float avg_log = avg_luma.SampleLevel(avg_luma_sampler, float2(0.5, 0.5), 0).r;
    float avg_lum = exp2(avg_log);                  // 幾何平均輝度
    float target  = a0.x / max(avg_lum, 1e-4);      // key / luminance
    target = clamp(target, a0.y, a0.z);
    float prev = prev_exp.SampleLevel(prev_exp_sampler, float2(0.5, 0.5), 0).r;
    float result = target;
    if (a1.y >= 0.5) {
        // 指数補間の eye adaptation (フレームレート非依存の順応)
        float k = 1.0 - exp(-max(a1.x, 0.0) * a0.w);
        result = prev + (target - prev) * k;
    }
    return float4(result, 0, 0, 0);
}
)";

// Exposure apply: m_HdrRt に順応済み露出 (1x1) を掛けて m_ExposedRt へ。
const char* kExposeApplyPS = R"(
Texture2D    hdr      : register(t0);
Texture2D    exposure : register(t1);
SamplerState hdr_sampler      : register(s0);
SamplerState exposure_sampler : register(s1);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float  e = exposure.SampleLevel(exposure_sampler, float2(0.5, 0.5), 0).r;
    float3 c = hdr.SampleLevel(hdr_sampler, v.uv, 0).rgb;
    e = abs(e) <= 65504.0 ? max(e, 0.0) : 1.0;
    c = all(abs(c) <= 65504.0) ? max(c, 0.0) : 0.0;
    return float4(c * e, 1.0);
}
)";

// 各パスで使う共通の動的 CB レイアウト
struct FPostCbLayout {
    FVec4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    FVec4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind
    FVec4 params2;   // x=vignette_intensity, y=vignette_radius, z=ca, w=grain
    FVec4 params3;   // x=grain_time, y=ssr_intensity, z=bloom_scatter, w=SSR adapted-exposure mode
    FVec4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    FVec4 cg_lift;   // xyz=lift
    FVec4 cg_gain;   // xyz=gain
    // x=cas_strength、tonemap描画のyzw=選択輪郭色。
    FVec4 cas_params;
    // TAA描画=x:blend/y:reproject/z:motion/w:reactive、tonemap描画=x:輪郭強さ/y:幅/z:有効。
    FVec4 taa_params;
};

// TAA reprojection 用の別 CB (b1 で bind)。
struct FTaaReprojCBLayout {
    FMat4 inv_view_proj;
    FMat4 prev_view_proj;
    FVec4 camera_position;
};

// auto-exposure 用 CB (Exposure adapt パスで b0 に bind)。
struct FAutoExposureCBLayout {
    FVec4 a0;   // x=key, y=min_exp, z=max_exp, w=speed
    FVec4 a1;   // x=dt, y=warm (0=cold start)
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

// 全画面三角形 (頂点バッファ無し) のパイプライン共通設定
void FillFullscreenLayout(FPipelineDesc& pd) noexcept {
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    pd.cull_mode     = ECullMode::None;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.depth_format  = EFormat::Unknown;
}

TResult<CPostProcess::FCompiledShaders> CompilePostShadersWithDevice(
    IRhiDevice& device, bool compile_async) noexcept {
    CPostProcess::FCompiledShaders compiled{};
    auto compile = [&](EShaderStage stage, const char* source,
                       const char* entry_point, const char* debug_name,
                       TUniquePtr<IRhiShader>& output) noexcept
        -> TResult<void> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = source;
        description.entry_point = entry_point;
        description.debug_name = debug_name;
        description.compile_async = compile_async;
        auto result = CreateRhiShader(device, description);
        if (result.IsErr()) return Err<void>(result.Error());
        output = Move(result.Value());
        return Ok();
    };

    if (auto r = compile(EShaderStage::Vertex, kFullscreenVS, "VSMain",
                         "Fullscreen.VS", compiled.fullscreen_vertex);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExtractPS, "PSMain",
                         "Bloom.Extract", compiled.extract_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kDownsamplePS, "PSMain",
                         "Bloom.Downsample", compiled.downsample_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kUpsamplePS, "PSMain",
                         "Bloom.Upsample", compiled.upsample_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kGaussianBlurPS, "PSMain",
                         "Bloom.Gaussian", compiled.gaussian_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kTaaResolvePS, "PSMain",
                         "Taa.Resolve", compiled.taa_resolve_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kTonemapPS, "PSMain",
                         "Tonemap", compiled.tonemap_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kLumaExtractPS, "PSMain",
                         "Luma.Extract", compiled.luma_extract_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kLumaDownsamplePS, "PSMain",
                         "Luma.Downsample", compiled.luma_downsample_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExposurePS, "PSMain",
                         "Exposure.Adapt", compiled.exposure_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExposeApplyPS, "PSMain",
                         "Exposure.Apply", compiled.exposure_apply_pixel);
        r.IsErr()) return Err<CPostProcess::FCompiledShaders>(r.Error());

    // FXAA は任意機能なので、シェーダ準備に失敗しても既存の直接トーンマップは
    // 利用できる。成功した場合だけ同じ描画基盤のコンパイル成果を移す。
    if (auto fxaa = CFxaa::CompileShaders(device, compile_async); fxaa.IsOk()) {
        compiled.fxaa_vertex = Move(fxaa.Value().vertex);
        compiled.fxaa_pixel = Move(fxaa.Value().pixel);
    }

    return TResult<CPostProcess::FCompiledShaders>(
        OkInit, Move(compiled));
}

} // namespace

CPostProcess::~CPostProcess() noexcept {
    Shutdown();
}

EShaderStatus CPostProcess::FCompiledShaders::Status() const noexcept {
    IRhiShader* const shaders[] = {
        fullscreen_vertex.Get(),
        extract_pixel.Get(),
        downsample_pixel.Get(),
        upsample_pixel.Get(),
        gaussian_pixel.Get(),
        taa_resolve_pixel.Get(),
        tonemap_pixel.Get(),
        luma_extract_pixel.Get(),
        luma_downsample_pixel.Get(),
        exposure_pixel.Get(),
        exposure_apply_pixel.Get(),
    };

    bool compiling = false;
    for (IRhiShader* shader : shaders) {
        if (!shader) return EShaderStatus::Failed;
        const EShaderStatus status = shader->Status();
        if (status == EShaderStatus::Failed) return EShaderStatus::Failed;
        if (status == EShaderStatus::Compiling) compiling = true;
    }

    // FXAA は任意シェーダ。両方が提供された場合だけ非同期コンパイルの完了を
    // 待つが、失敗しても必須のトーンマップ処理は Ready とみなす。
    if (fxaa_vertex && fxaa_pixel) {
        compiling = compiling
            || fxaa_vertex->Status() == EShaderStatus::Compiling
            || fxaa_pixel->Status() == EShaderStatus::Compiling;
    }
    return compiling ? EShaderStatus::Compiling : EShaderStatus::Ready;
}

void FPostProcessParams::Sanitize() noexcept {
    const FPostProcessParams defaults{};
    bloom_threshold = ClampFinite(
        bloom_threshold, defaults.bloom_threshold, 0.0f, 65504.0f);
    bloom_intensity = ClampFinite(
        bloom_intensity, defaults.bloom_intensity, 0.0f, 16.0f);
    bloom_radius = ClampFinite(
        bloom_radius, defaults.bloom_radius, 0.0f, 16.0f);
    bloom_scatter = ClampFinite(
        bloom_scatter, defaults.bloom_scatter, 0.0f, 1.0f);
    exposure = ClampFinite(exposure, defaults.exposure, 0.0f, 64.0f);
    gamma = ClampFinite(gamma, defaults.gamma, 1.0f, 4.0f);
    if (tonemap_kind < 0 || tonemap_kind > 2) {
        tonemap_kind = defaults.tonemap_kind;
    }
    vignette_intensity = ClampFinite(
        vignette_intensity, defaults.vignette_intensity, 0.0f, 1.0f);
    vignette_radius = ClampFinite(
        vignette_radius, defaults.vignette_radius, 0.0f, 1.0f);
    chromatic_aberration = ClampFinite(
        chromatic_aberration, defaults.chromatic_aberration, 0.0f, 0.05f);
    grain_intensity = ClampFinite(
        grain_intensity, defaults.grain_intensity, 0.0f, 0.25f);
    if (!std::isfinite(grain_time)) grain_time = defaults.grain_time;
    ssr_intensity = ClampFinite(
        ssr_intensity, defaults.ssr_intensity, 0.0f, 8.0f);
    cg_saturation = ClampFinite(
        cg_saturation, defaults.cg_saturation, 0.0f, 4.0f);
    cg_contrast = ClampFinite(
        cg_contrast, defaults.cg_contrast, 0.0f, 4.0f);
    cg_temperature = ClampFinite(
        cg_temperature, defaults.cg_temperature, -1.0f, 1.0f);
    cg_tint = ClampFinite(cg_tint, defaults.cg_tint, -1.0f, 1.0f);
    cg_lift = ClampFinite(cg_lift, defaults.cg_lift, -2.0f, 2.0f);
    cg_gain = ClampFinite(cg_gain, defaults.cg_gain, 0.0f, 8.0f);
    cas_strength = ClampFinite(
        cas_strength, defaults.cas_strength, 0.0f, 1.0f);
    taa_blend_factor = ClampFinite(
        taa_blend_factor, defaults.taa_blend_factor, 0.0f, 1.0f);
    if (!MatrixHasFiniteInverse(taa_view_proj_no_jitter)) {
        taa_view_proj_no_jitter = FMat4::Identity();
    }
    if (!MatrixHasFiniteInverse(taa_prev_view_proj_no_jitter)) {
        taa_prev_view_proj_no_jitter = FMat4::Identity();
    }
    taa_camera_position = ClampFinite(
        taa_camera_position, defaults.taa_camera_position, -1.0e9f, 1.0e9f);
    auto_exposure_key = ClampFinite(
        auto_exposure_key, defaults.auto_exposure_key, 0.0001f, 16.0f);
    auto_exposure_min = ClampFinite(
        auto_exposure_min, defaults.auto_exposure_min, 0.0001f, 64.0f);
    auto_exposure_max = ClampFinite(
        auto_exposure_max, defaults.auto_exposure_max,
        auto_exposure_min, 64.0f);
    auto_exposure_speed = ClampFinite(
        auto_exposure_speed, defaults.auto_exposure_speed, 0.0f, 64.0f);
    delta_time = ClampFinite(
        delta_time, defaults.delta_time, 0.0f, 1.0f);
}

TResult<CPostProcess::FCompiledShaders>
CPostProcess::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    FCompiledShaders compiled{};
    auto compile = [](EShaderStage stage, const char* source,
                      const char* entry_point, const char* debug_name,
                      TUniquePtr<IRhiShader>& output) noexcept
        -> TResult<void> {
        FShaderDesc description{};
        description.stage = stage;
        description.hlsl_source = source;
        description.entry_point = entry_point;
        description.debug_name = debug_name;

        auto shader = MakeUnique<FDx12Shader>();
        if (!shader) {
            return ACS_ERR(
                Memory, 720, "Post-process shader allocation failed");
        }
        const FHrResult result = shader->Init(description);
        if (result.IsErr()) {
            return ACS_ERR_OS(
                Render, 721, "Post-process shader CPU compile failed",
                static_cast<u32>(result.hr));
        }
        auto* allocator = shader.GetAllocator();
        output = TUniquePtr<IRhiShader>(shader.Release(), allocator);
        return Ok();
    };

    if (auto r = compile(EShaderStage::Vertex, kFullscreenVS, "VSMain",
                         "Fullscreen.VS", compiled.fullscreen_vertex);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExtractPS, "PSMain",
                         "Bloom.Extract", compiled.extract_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kDownsamplePS, "PSMain",
                         "Bloom.Downsample", compiled.downsample_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kUpsamplePS, "PSMain",
                         "Bloom.Upsample", compiled.upsample_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kGaussianBlurPS, "PSMain",
                         "Bloom.Gaussian", compiled.gaussian_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kTaaResolvePS, "PSMain",
                         "Taa.Resolve", compiled.taa_resolve_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kTonemapPS, "PSMain",
                         "Tonemap", compiled.tonemap_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kLumaExtractPS, "PSMain",
                         "Luma.Extract", compiled.luma_extract_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kLumaDownsamplePS, "PSMain",
                         "Luma.Downsample", compiled.luma_downsample_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExposurePS, "PSMain",
                         "Exposure.Adapt", compiled.exposure_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());
    if (auto r = compile(EShaderStage::Pixel, kExposeApplyPS, "PSMain",
                         "Exposure.Apply", compiled.exposure_apply_pixel);
        r.IsErr()) return Err<FCompiledShaders>(r.Error());

    // FXAA は任意機能。失敗時も PostProcess の通常表示を維持する。
    if (auto fxaa = CFxaa::CompileShadersCpu(); fxaa.IsOk()) {
        compiled.fxaa_vertex = Move(fxaa.Value().vertex);
        compiled.fxaa_pixel = Move(fxaa.Value().pixel);
    }

    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 722,
        "Post-process CPU compilation is available only on raw DX12");
#endif
}

TResult<CPostProcess::FCompiledShaders>
CPostProcess::BeginCompileShadersAsync(IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 723,
            "Post-process backend-managed asynchronous compilation is "
            "unsupported");
    }
    return CompilePostShadersWithDevice(device, true);
}

TResult<void> CPostProcess::Init(IRhiDevice& device, u32 width, u32 height,
                                EFormat color_format) noexcept {
    auto compiled = CompilePostShadersWithDevice(device, false);
    if (compiled.IsErr()) return Err<void>(compiled.Error());
    return InitWithCompiledShaders(
        device, Move(compiled.Value()), width, height, color_format);
}

TResult<void> CPostProcess::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    u32 width, u32 height, EFormat color_format) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 724, "Post-process compiled shader set is not ready");
    }
    if (width == 0) width = 1;
    if (height == 0) height = 1;

    // Construct a complete replacement away from the published renderer.
    // No externally visible state changes until every allocation and PSO has
    // succeeded, so startup may retry without destroying a valid stack.
    CPostProcess candidate;
    candidate.m_Device = &device;
    candidate.m_ColorFormat = color_format;
    candidate.m_Width = width;
    candidate.m_Height = height;

    if (auto r = candidate.CreateRenderTargets(device, width, height);
        r.IsErr()) return Err<void>(r.Error());

    FBufferDesc cbd{};
    cbd.size         = CBSize<FPostCbLayout>();
    cbd.usage        = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    for (u32 i = 0; i < kPostCbRing; ++i) {
        auto cbr = CreateRhiBuffer(device, cbd);
        if (cbr.IsErr()) return Err<void>(cbr.Error());
        candidate.m_CbPost[i] = Move(cbr.Value());
    }
    candidate.m_PostCbCursor = 0;

    // TaaReproj CB (b1)
    FBufferDesc rcbd{};
    rcbd.size = CBSize<FTaaReprojCBLayout>();
    rcbd.usage = EBufferUsage::Uniform;
    rcbd.cpu_writable = true;
    auto rcbr = CreateRhiBuffer(device, rcbd);
    if (rcbr.IsErr()) return Err<void>(rcbr.Error());
    candidate.m_CbTaaReproj = Move(rcbr.Value());

    // auto-exposure 用 CB
    FBufferDesc acbd{};
    acbd.size = CBSize<FAutoExposureCBLayout>();
    acbd.usage = EBufferUsage::Uniform;
    acbd.cpu_writable = true;
    auto acbr = CreateRhiBuffer(device, acbd);
    if (acbr.IsErr()) return Err<void>(acbr.Error());
    candidate.m_CbAuto = Move(acbr.Value());

    // depth が未指定だった時のための 1x1 fallback (depth>=0.9999 になるよう 255 で fill)
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = far_depth; td.initial_data_size = 4;
    auto dfb = CreateRhiTexture(device, td);
    if (dfb.IsErr()) return Err<void>(dfb.Error());
    candidate.m_TaaDepthFb = Move(dfb.Value());

    // 未使用の bloom / SSR slot に stale texture を残さないための黒 fallback。
    const u8 black_rgba[4] = { 0, 0, 0, 255 };
    FTextureDesc black_desc{};
    black_desc.width = 1; black_desc.height = 1;
    black_desc.format = EFormat::R8G8B8A8_UNorm;
    black_desc.initial_data = black_rgba;
    black_desc.initial_data_size = 4;
    auto black = CreateRhiTexture(device, black_desc);
    if (black.IsErr()) return Err<void>(black.Error());
    candidate.m_BlackFb = Move(black.Value());

    if (auto r = candidate.CreatePipelines(device, Move(shaders));
        r.IsErr()) return Err<void>(r.Error());

    Shutdown();
    *this = Move(candidate);
    return Ok();
}

void CPostProcess::Shutdown() noexcept {
    m_FxaaInput.Reset();
    m_Fxaa.Reset();
    m_BlackFb.Reset();
    m_TaaDepthFb.Reset();
    m_CbAuto.Reset();
    m_CbTaaReproj.Reset();
    for (auto& cb : m_CbPost) cb.Reset();
    m_PostCbCursor = 0;
    m_PipeExposeApply.Reset();
    m_PipeExposure.Reset();
    m_PipeLumaDown.Reset();
    m_PipeLumaExtract.Reset();
    m_PipeTonemap.Reset();
    m_PipeTaaResolve.Reset();
    m_PipeGaussian.Reset();
    m_PipeUpsample.Reset();
    m_PipeDownsample.Reset();
    m_PipeExtract.Reset();
    m_PsExposeApply.Reset();
    m_PsExposure.Reset();
    m_PsLumaDown.Reset();
    m_PsLumaExtract.Reset();
    m_PsTonemap.Reset();
    m_PsTaaResolve.Reset();
    m_PsGaussian.Reset();
    m_PsUpsample.Reset();
    m_PsDownsample.Reset();
    m_PsExtract.Reset();
    m_VsFullscreen.Reset();
    m_ExposedRt.Reset();
    for (auto& e : m_Exposure)   e.Reset();
    for (auto& m : m_LumaMips)  m.Reset();
    m_LumaMipCount = 0;
    for (auto& t : m_Taa) t.Reset();
    for (auto& m : m_BloomMips) m.Reset();
    for (auto& m : m_BloomTmp)  m.Reset();
    m_HdrRt.Reset();
    m_TaaFrame  = 0;
    m_AutoFrame = 0;
    m_BloomOutputValid = false;
    m_TaaOutputValid = false;
    m_ExposureOutputValid = false;
    m_TransientAliasPlan = {};
    m_Device = nullptr;
}

TResult<void> CPostProcess::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 300, "CPostProcess::Resize before Init");
    if (width == 0) width = 1;
    if (height == 0) height = 1;
    if (width == m_Width && height == m_Height) return Ok();

    // Build the complete replacement away from the published frame graph.
    // A failed allocation must not invalidate the old-size HDR target, TAA
    // history, exposure history, or their frame indices: the caller can keep
    // presenting a stable clear and retry this or a later size next frame.
    CPostProcess candidate;
    candidate.m_HdrFormat = m_HdrFormat;
    candidate.m_LumaFormat = m_LumaFormat;
    if (auto created =
            candidate.CreateRenderTargets(*m_Device, width, height);
        created.IsErr()) {
        return Err<void>(created.Error());
    }

    m_HdrRt = Move(candidate.m_HdrRt);
    for (u32 i = 0; i < kBloomMips; ++i) {
        m_BloomMips[i] = Move(candidate.m_BloomMips[i]);
        m_BloomTmp[i] = Move(candidate.m_BloomTmp[i]);
    }
    for (u32 i = 0; i < 2u; ++i) {
        m_Taa[i] = Move(candidate.m_Taa[i]);
        m_Exposure[i] = Move(candidate.m_Exposure[i]);
    }
    for (u32 i = 0; i < kMaxLumaMips; ++i)
        m_LumaMips[i] = Move(candidate.m_LumaMips[i]);
    m_LumaMipCount = candidate.m_LumaMipCount;
    m_ExposedRt = Move(candidate.m_ExposedRt);
    // FXAA の中間 LDR は新しい寸法で遅延再作成する。FXAAシェーダとPSOは
    // 描画先形式のみで決まるため、そのまま再利用できる。
    m_FxaaInput.Reset();
    m_TransientAliasPlan = candidate.m_TransientAliasPlan;
    m_Width  = width;
    m_Height = height;
    m_TaaFrame  = 0;     // reset TAA state on resize (history は size 違いで使えない)
    m_AutoFrame = 0;     // reset auto-exposure state on resize
    m_BloomOutputValid = false;
    m_TaaOutputValid = false;
    m_ExposureOutputValid = false;
    return Ok();
}

namespace {

/** 固定 PostProcess graph の一時リソース寿命を集計する。 */
FRenderGraphAliasPlanSummary BuildPostProcessTransientAliasPlan(u32 width, u32 height, u32 luma_mip_count) noexcept {
    // Luma 一時リソースの最大合計。
    constexpr u32 max_resource_count = 13;
    // Luma render target heap の互換クラス。
    constexpr u64 luma_compatibility = 2;

    // 固定 graph の寿命入力。
    FRenderGraphResourceLifetime lifetimes[max_resource_count]{};
    // 現在追加済みの寿命数。
    u32 resource_count = 0;

    // Luma chain は隣接 mip が同一 pass で重なり、1 段飛ばしだけが候補となる。
    u32 luma_width = width;
    u32 luma_height = height;
    for (u32 mip_index = 0; mip_index < luma_mip_count; ++mip_index) {
        luma_width = luma_width > 1 ? luma_width / 2 : 1;
        luma_height = luma_height > 1 ? luma_height / 2 : 1;
        lifetimes[resource_count++] = FRenderGraphResourceLifetime{0x200u + mip_index, luma_compatibility, static_cast<u64>(luma_width) * static_cast<u64>(luma_height) * 4u, mip_index, mip_index + 1u, true, true};
    }

    // 候補計画は GPU 所有権を変更せず集計だけを公開する。
    CRenderGraphTransientAliasPlanner planner;
    if (!planner.Build(lifetimes, resource_count)) {
        return {};
    }
    return planner.Summary();
}

} // namespace

TResult<void> CPostProcess::CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // メイン HDR RT
    FTextureDesc td{};
    td.width  = w;
    td.height = h;
    td.format = m_HdrFormat;
    td.is_render_target = true;
    auto hr = CreateRhiTexture(device, td);
    if (hr.IsErr()) return Err<void>(hr.Error());
    m_HdrRt = Move(hr.Value());

    // Bloom mip chain (1/2, 1/4, 1/8, 1/16, 1/32)
    u32 mw = w, mh = h;
    for (u32 i = 0; i < kBloomMips; ++i) {
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
        FTextureDesc bd{};
        bd.width  = mw;
        bd.height = mh;
        bd.format = m_HdrFormat;
        bd.is_render_target = true;
        auto br = CreateRhiTexture(device, bd);
        if (br.IsErr()) return Err<void>(br.Error());
        m_BloomMips[i] = Move(br.Value());
        // Upsample を含む全段の ping-pong 先を確保する。
        auto btr = CreateRhiTexture(device, bd);
        if (btr.IsErr()) return Err<void>(btr.Error());
        m_BloomTmp[i] = Move(btr.Value());
    }

    // TAA history ping-pong RT: HDR と同サイズ + 同フォーマット
    for (u32 i = 0; i < 2; ++i) {
        FTextureDesc tt{};
        tt.width  = w;
        tt.height = h;
        tt.format = m_HdrFormat;
        tt.is_render_target = true;
        auto tr = CreateRhiTexture(device, tt);
        if (tr.IsErr()) return Err<void>(tr.Error());
        m_Taa[i] = Move(tr.Value());
    }

    // ---- Auto-exposure ----
    // Luma mip chain: m_HdrRt の 1/2 から 1x1 まで縮約する。最深段 (1x1) に
    // シーン平均 log2 輝度が入る。フォーマットは 1ch あれば足りるが、RT として
    // 実績のある R16G16_Float (BRDF LUT と同形式) を使う。
    {
        u32 lw = w, lh = h;
        m_LumaMipCount = 0;
        for (u32 i = 0; i < kMaxLumaMips; ++i) {
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            FTextureDesc ld{};
            ld.width  = lw;
            ld.height = lh;
            ld.format = m_LumaFormat;
            ld.is_render_target = true;
            auto lr = CreateRhiTexture(device, ld);
            if (lr.IsErr()) return Err<void>(lr.Error());
            m_LumaMips[i] = Move(lr.Value());
            ++m_LumaMipCount;
            if (lw == 1 && lh == 1) break;
        }
    }
    // 順応済み露出 (1x1 ping-pong)
    for (u32 i = 0; i < 2; ++i) {
        FTextureDesc ed{};
        ed.width  = 1;
        ed.height = 1;
        ed.format = m_LumaFormat;
        ed.is_render_target = true;
        auto er = CreateRhiTexture(device, ed);
        if (er.IsErr()) return Err<void>(er.Error());
        m_Exposure[i] = Move(er.Value());
    }
    // 露出適用後の HDR (下流パスが読む)
    {
        FTextureDesc xd{};
        xd.width  = w;
        xd.height = h;
        xd.format = m_HdrFormat;
        xd.is_render_target = true;
        auto xr = CreateRhiTexture(device, xd);
        if (xr.IsErr()) return Err<void>(xr.Error());
        m_ExposedRt = Move(xr.Value());
    }
    m_TransientAliasPlan = BuildPostProcessTransientAliasPlan(w, h, m_LumaMipCount);
    return Ok();
}

TResult<void> CPostProcess::CreatePipelines(
    IRhiDevice& device, FCompiledShaders&& shaders) noexcept {
    if (shaders.Status() != EShaderStatus::Ready) {
        return ACS_ERR(
            Render, 724, "Post-process compiled shader set is not ready");
    }

    // ---- Pipelines ----
    // Extract: HDR → bloom_mips[0]、Opaque blend
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.extract_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeExtract = Move(r.Value());
    }
    // Downsample: bloom_mips[i] → bloom_mips[i+1]、Opaque
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.downsample_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeDownsample = Move(r.Value());
    }
    // upsample: lower mip + current mip → ping-pong target、正規化した opaque blend
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.upsample_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 2;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.texture_names[1] = "base_tex";
        pd.static_sampler_count = 2;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeUpsample = Move(r.Value());
    }
    // Gaussian blur: separable (H/V)、Opaque (置換)。各 mip を円形にぼかす。
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.gaussian_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeGaussian = Move(r.Value());
    }
    // TAA Resolve: current HDR + history HDR + depth/motion + reactive mask → resolved HDR、Opaque
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.taa_resolve_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 2;       // b0=Post, b1=TaaReproj
        pd.texture_slots = 5;
        pd.cbuffer_names[0] = "Post";
        pd.cbuffer_names[1] = "TaaReproj";
        pd.texture_names[0] = "current_hdr";
        pd.texture_names[1] = "history_hdr";
        pd.texture_names[2] = "scene_depth";
        pd.texture_names[3] = "reactive_mask";
        pd.texture_names[4] = "reactive_scene_depth";
        pd.static_sampler_count = 5;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        pd.static_samplers[2].filter    = ESamplerFilter::Point;     // depth は離散値
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[3].filter    = ESamplerFilter::Point;
        pd.static_samplers[3].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[3].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[4].filter    = ESamplerFilter::Point;
        pd.static_samplers[4].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[4].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeTaaResolve = Move(r.Value());
    }

    // Tonemap: HDR + bloom + ssr + 任意の選択mask → backbuffer、Opaque
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.tonemap_pixel.Get();
        pd.rt_format = m_ColorFormat;
        pd.cbuffer_slots = 1;
        // t0=HDR、t1=bloom、t2=SSR、t3=補正露出、t4=選択mask。
        pd.texture_slots = 5;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "bloom";
        pd.texture_names[2] = "ssr";
        pd.texture_names[3] = "adapted_exposure";
        pd.texture_names[4] = "selection_mask";
        pd.static_sampler_count = 5;
        for (u32 i = 0; i < 5; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeTonemap = Move(r.Value());
    }

    // ---- Auto-exposure pipelines ----
    // Luma Extract: m_HdrRt → m_LumaMips[0]、log2 輝度
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.luma_extract_pixel.Get();
        pd.rt_format = m_LumaFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeLumaExtract = Move(r.Value());
    }
    // Luma Downsample: m_LumaMips[i] → m_LumaMips[i+1]
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.luma_downsample_pixel.Get();
        pd.rt_format = m_LumaFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeLumaDown = Move(r.Value());
    }
    // Exposure Adapt: avg luma (1x1) + prev exposure (1x1) → 順応済み露出 (1x1)
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.exposure_pixel.Get();
        pd.rt_format = m_LumaFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 2;
        pd.cbuffer_names[0] = "AutoExp";
        pd.texture_names[0] = "avg_luma";
        pd.texture_names[1] = "prev_exp";
        pd.static_sampler_count = 2;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeExposure = Move(r.Value());
    }
    // Exposure Apply: m_HdrRt + 露出 (1x1) → m_ExposedRt
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = shaders.fullscreen_vertex.Get();
        pd.ps = shaders.exposure_apply_pixel.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 0;
        pd.texture_slots = 2;
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "exposure";
        pd.static_sampler_count = 2;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeExposeApply = Move(r.Value());
    }

    // FXAA は任意機能。コンパイル処理がシェーダを準備できた場合だけPSOを
    // 作り、失敗時は Render が従来の直接トーンマップへ戻れるようにする。
    if (shaders.fxaa_vertex && shaders.fxaa_pixel) {
        auto fxaa = MakeUnique<CFxaa>();
        if (fxaa) {
            CFxaa::FCompiledShaders fxaa_shaders{};
            fxaa_shaders.vertex = Move(shaders.fxaa_vertex);
            fxaa_shaders.pixel = Move(shaders.fxaa_pixel);
            const auto result = fxaa->InitWithCompiledShaders(
                device, Move(fxaa_shaders), m_ColorFormat);
            if (result.IsOk()) {
                m_Fxaa = Move(fxaa);
            } else {
                ACS_LOG_WARN(
                    "CPostProcess: optional FXAA pipeline unavailable; "
                    "direct tonemap remains active: %s",
                    result.Error().message);
            }
        }
    }

    // Shader ownership is transferred only after every PSO is known-good.
    // A failed owner-thread commit therefore leaves the compiled set reusable.
    m_VsFullscreen = Move(shaders.fullscreen_vertex);
    m_PsExtract = Move(shaders.extract_pixel);
    m_PsDownsample = Move(shaders.downsample_pixel);
    m_PsUpsample = Move(shaders.upsample_pixel);
    m_PsGaussian = Move(shaders.gaussian_pixel);
    m_PsTaaResolve = Move(shaders.taa_resolve_pixel);
    m_PsTonemap = Move(shaders.tonemap_pixel);
    m_PsLumaExtract = Move(shaders.luma_extract_pixel);
    m_PsLumaDown = Move(shaders.luma_downsample_pixel);
    m_PsExposure = Move(shaders.exposure_pixel);
    m_PsExposeApply = Move(shaders.exposure_apply_pixel);
    return Ok();
}

void CPostProcess::Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index, const FPostProcessParams& params, IRhiTexture* selection_mask, FVec3 selection_color, f32 selection_intensity, f32 selection_thickness_pixels) noexcept {
    m_BloomOutputValid = false;
    m_TaaOutputValid = false;
    m_ExposureOutputValid = false;
    if (!m_HdrRt || !m_PipeExtract) return;
    m_PostCbCursor = 0;
    FPostProcessParams safe_params = params;
    safe_params.Sanitize();

    // maskと設定が一組でも不正なら、通常のpost processへ戻す。NaNは範囲比較を
    // 通らないため、production側で追加の標準library判定を要しない。
    const bool selection_outline_valid =
        selection_mask != nullptr
        && selection_mask->Width() == swapchain.Width()
        && selection_mask->Height() == swapchain.Height()
        && selection_color.x >= 0.0f && selection_color.x <= 1.0f
        && selection_color.y >= 0.0f && selection_color.y <= 1.0f
        && selection_color.z >= 0.0f && selection_color.z <= 1.0f
        && selection_intensity > 0.0f && selection_intensity <= 4.0f
        && selection_thickness_pixels > 0.0f
        && selection_thickness_pixels <= 4.0f;
    if (!selection_outline_valid) {
        selection_mask = nullptr;
        selection_color = FVec3{0.0f, 0.0f, 0.0f};
        selection_intensity = 0.0f;
        selection_thickness_pixels = 0.0f;
    }

    // Metering and adaptation inspect raw scene-linear HDR. Exposure is
    // deliberately applied only after TAA below: accumulating
    // exposure-scaled history makes every eye-adaptation change look like
    // scene motion and creates long brightness trails.
    const bool auto_exp = safe_params.auto_exposure_enabled
                          && m_PipeLumaExtract && m_ExposedRt;
    bool exposure_adapted = false;
    if (auto_exp) {
        exposure_adapted =
            Pass_LumaReduce(cmd) &&
            Pass_ExposureAdapt(cmd, safe_params);
    } else {
        // Re-enabling after a long disabled interval must cold-start from the
        // current luminance instead of adapting from a stale exposure texture.
        m_AutoFrame = 0;
    }

    // Resolve raw scene-linear HDR against scene-linear history. Downstream
    // selection is centralized in SceneInput(), after optional exposure.
    if (safe_params.taa_enabled) {
        m_TaaOutputValid = Pass_TaaResolve(cmd, safe_params);
    } else {
        // A disabled temporal pass invalidates its logical history.  Without
        // this reset, re-enabling TAA after an animated-cloud interval would
        // blend against the last pre-cloud frame, producing a long stale-image
        // flash.  Frame zero already uses current HDR as the history input.
        m_TaaFrame = 0;
    }

    if (auto_exp && exposure_adapted) {
        IRhiTexture* exposure_source = m_HdrRt.Get();
        if (m_TaaOutputValid && m_Taa[m_TaaFrame % 2]) {
            exposure_source = m_Taa[m_TaaFrame % 2].Get();
        }
        if (exposure_source) {
            m_ExposureOutputValid =
                Pass_ExposureApply(cmd, *exposure_source);
        }
    }

    if (safe_params.bloom_enabled && safe_params.bloom_intensity > 0.0f) {
        // 1) Extract: HDR (もしくは TAA resolved) → mip[0]
        bool bloom_complete = Pass_Extract(cmd, safe_params);

        // 2) Downsample: mip[i] → mip[i+1]。13-tap filter 自体が低域を滑らかにする。
        for (u32 i = 0; i + 1 < kBloomMips; ++i) {
            if (bloom_complete) {
                bloom_complete = Pass_Downsample(cmd, i);
            }
        }

        // 2.5) 高周波側の 2 段だけを円形 Gaussian で整形する。深い mip は既に
        //      downsample + circular upsample で十分滑らかなので、全段 H/V の過剰な霧化を避ける。
        const u32 gaussian_mips = kBloomMips < 2 ? kBloomMips : 2;
        for (u32 i = 0; i < gaussian_mips; ++i) {
            if (bloom_complete) {
                bloom_complete =
                    Pass_GaussianBlur(cmd, i, true, 1.0f);
            }
            if (bloom_complete) {
                bloom_complete =
                    Pass_GaussianBlur(cmd, i, false, 1.0f);
            }
        }

        // 3) Upsample: lower と current を scatter 付き正規化補間する。
        //    一定輝度の energy を mip 数で増幅せず、発光体の芯と広がりを両立する。
        for (u32 i = kBloomMips - 1; i > 0; --i) {
            const f32 r = safe_params.bloom_radius
                * (0.90f + static_cast<f32>(i - 1) * 0.08f);
            if (bloom_complete) {
                bloom_complete = Pass_Upsample(
                    cmd, i - 1, r, safe_params.bloom_scatter);
            }
        }
        m_BloomOutputValid = bloom_complete;
    }

    // 4) Tonemap: HDR (or TAA resolved) + mip[0] → LDR backbuffer.
    // FXAA はトーンマップとガンマ補正の後にだけ掛ける。TAA の有効出力へ重ねると
    // temporally resolved edge を再び空間ぼかしするため、明示的に抑止する。
    const bool use_fxaa = safe_params.fxaa_enabled
        && !m_TaaOutputValid
        && EnsureFxaaResources();
    if (use_fxaa && Pass_Tonemap(cmd, swapchain, buffer_index, safe_params, m_FxaaInput.Get(), selection_mask, selection_color, selection_intensity, selection_thickness_pixels)) {
        cmd.BeginRenderToSwapchain(swapchain, buffer_index, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
        FViewport viewport{};
        viewport.width = static_cast<f32>(swapchain.Width());
        viewport.height = static_cast<f32>(swapchain.Height());
        cmd.SetViewport(viewport);
        FScissorRect scissor{};
        scissor.right = static_cast<i32>(swapchain.Width());
        scissor.bottom = static_cast<i32>(swapchain.Height());
        cmd.SetScissor(scissor);
        m_Fxaa->Apply(cmd, *m_FxaaInput);
        // FXAA が全画面を上書きした後も、swapchain pass を必ず閉じる。
        cmd.EndRenderToSwapchain(swapchain, buffer_index);
    } else {
        // FXAA資源・中間RT・トーンマップが一つでも使えない場合は、中間へ描いて
        // から戻ることをせず、従来の直接経路で画面を成立させる。
        Pass_Tonemap(cmd, swapchain, buffer_index, safe_params, nullptr, selection_mask, selection_color, selection_intensity, selection_thickness_pixels);
    }

    // Advance temporal cursors only after their complete output was recorded.
    // Failed or partial passes preserve the last complete history for retry
    // and never publish an unwritten target in this frame.
    if (m_TaaOutputValid) {
        m_TaaFrame++;
    }
    if (m_ExposureOutputValid) {
        m_AutoFrame++;
    }
}

void CPostProcess::Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index, const FPostProcessParams& params) noexcept {
    Render(cmd, swapchain, buffer_index, params, nullptr, FVec3{0.0f, 0.0f, 0.0f}, 0.0f, 0.0f);
}

namespace {
void UpdatePostCB(IRhiBuffer* cb, const FPostProcessParams& p, f32 texel_w, f32 texel_h, FVec3 selection_color = FVec3{}, f32 selection_intensity = 0.0f, f32 selection_thickness_pixels = 0.0f) noexcept {
    if (!cb) return;
    FPostCbLayout l{};
    l.params0 = FVec4{ p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.exposure };
    l.params1 = FVec4{ p.gamma, texel_w, texel_h, static_cast<f32>(p.tonemap_kind) };
    l.params2 = FVec4{ p.vignette_intensity, p.vignette_radius,
                      p.chromatic_aberration, p.grain_intensity };
    l.params3 = FVec4{
        p.grain_time, p.ssr_intensity, p.bloom_scatter,
        p.auto_exposure_enabled ? 1.0f : 0.0f};
    l.cg0     = FVec4{ p.cg_saturation, p.cg_contrast, p.cg_temperature, p.cg_tint };
    l.cg_lift = FVec4{ p.cg_lift.x, p.cg_lift.y, p.cg_lift.z, 0 };
    l.cg_gain = FVec4{ p.cg_gain.x, p.cg_gain.y, p.cg_gain.z, 0 };
    l.cas_params = FVec4{
        p.cas_strength < 0 ? 0.0f : p.cas_strength,
        selection_color.x, selection_color.y, selection_color.z};
    // reproject_enabled は taa_depth_texture が指定されてるかで判定。
    const f32 reproject_enabled = (p.taa_enabled && p.taa_depth_texture) ? 1.0f : 0.0f;
    // motion texture があれば motion mode (depth reprojection より優先)。
    const f32 motion_mode = (p.taa_enabled && p.taa_motion_texture) ? 1.0f : 0.0f;
    const f32 reactive_mode =
        (p.taa_enabled && p.taa_reactive_texture) ? 1.0f : 0.0f;
    l.taa_params = FVec4{ p.taa_blend_factor < 0 ? 0.0f : p.taa_blend_factor,
                         reproject_enabled, motion_mode, reactive_mode };
    if (selection_intensity > 0.0f && selection_thickness_pixels > 0.0f) {
        // Tonemap drawだけは同じ未使用領域を選択輪郭へ読み替える。TAA drawは従来値のまま。
        l.taa_params = FVec4{
            selection_intensity, selection_thickness_pixels, 1.0f, 0.0f};
    }
    cb->Update(&l, sizeof(l), 0);
}
} // namespace

IRhiBuffer* CPostProcess::AcquirePostCb() noexcept {
    if (m_PostCbCursor >= kPostCbRing) {
        if (m_PostCbCursor == kPostCbRing) {
            ACS_LOG_WARN("CPostProcess: per-frame pass CB limit (%u) exceeded; "
                         "remaining post passes are skipped", kPostCbRing);
            ++m_PostCbCursor;
        }
        return nullptr;
    }
    return m_CbPost[m_PostCbCursor++].Get();
}

bool CPostProcess::Pass_Extract(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept {
    auto* dst = m_BloomMips[0].Get();
    if (!dst || !m_HdrRt || !m_PipeExtract) return false;

    // SceneInput selects the fully resolved/exposed HDR image. Bloom therefore
    // sees temporally stable highlights without bypassing auto exposure.
    IRhiTexture* src = SceneInput(p);
    if (!src) return false;
    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    UpdatePostCB(post_cb, p, 1.0f / src->Width(), 1.0f / src->Height());

    cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExtract);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
    return true;
}

bool CPostProcess::Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept {
    if (from_mip >= kBloomMips - 1u || !m_PipeDownsample) return false;
    auto* src = m_BloomMips[from_mip].Get();
    auto* dst = m_BloomMips[from_mip + 1].Get();
    if (!src || !dst) return false;
    FPostProcessParams p{};   // params 不要だが texel size のみ更新
    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    UpdatePostCB(post_cb, p, 1.0f / src->Width(), 1.0f / src->Height());

    cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeDownsample);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
    return true;
}

bool CPostProcess::Pass_Upsample(IRhiCommandList& cmd, u32 to_mip, f32 radius,
                                 f32 scatter) noexcept {
    if (to_mip >= kBloomMips - 1u || !m_PipeUpsample) return false;
    auto* src = m_BloomMips[to_mip + 1].Get();
    auto* base = m_BloomMips[to_mip].Get();
    auto* dst = m_BloomTmp[to_mip].Get();
    if (!src || !base || !dst) return false;
    FPostProcessParams p{};
    p.bloom_radius = radius;
    p.bloom_scatter = scatter;
    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    UpdatePostCB(post_cb, p, 1.0f / src->Width(), 1.0f / src->Height());

    // 同じ texture を read/write しないよう ping-pong。opaque 出力後に所有ポインタを交換する。
    cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeUpsample);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetTexture(0, *src);
    cmd.SetTexture(1, *base);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
    Swap(m_BloomMips[to_mip], m_BloomTmp[to_mip]);
    return true;
}

bool CPostProcess::Pass_GaussianBlur(IRhiCommandList& cmd, u32 mip, bool horizontal, f32 amount) noexcept {
    if (mip >= kBloomMips || !m_PipeGaussian) return false;
    auto* tex = m_BloomMips[mip].Get();
    auto* tmp = m_BloomTmp[mip].Get();
    if (!tex || !tmp) return false;
    IRhiTexture* src = horizontal ? tex : tmp;   // H: mip→tmp、V: tmp→mip
    IRhiTexture* dst = horizontal ? tmp : tex;
    FPostProcessParams p{};
    p.bloom_radius = amount;                     // → params0.z (step スケール)
    // 方向: H=(texel_w,0)、V=(0,texel_h) を params1.yz へ
    const f32 tw = 1.0f / static_cast<f32>(tex->Width());
    const f32 th = 1.0f / static_cast<f32>(tex->Height());
    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    UpdatePostCB(post_cb, p, horizontal ? tw : 0.0f, horizontal ? 0.0f : th);

    cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeGaussian);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
    return true;
}

bool CPostProcess::Pass_TaaResolve(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept {
    auto* cur_rt = m_Taa[m_TaaFrame % 2].Get();         // 今フレームの書き先
    auto* hist_rt = m_Taa[(m_TaaFrame + 1) % 2].Get();   // 前フレームの resolved
    if (!cur_rt || !hist_rt || !m_HdrRt || !m_PipeTaaResolve ||
        !m_CbTaaReproj) {
        return false;
    }

    // Cold-start (frame 0): history RT が未書き込み = Diligent の未定義メモリを
    // 読む可能性がある。current_hdr を history slot にも bind することで
    // output = lerp(current, current, a) = current となり、garbage を完全排除。
    // 翌フレームからは history RT に実 resolved が入っているので通常 path。
    // Temporal history is scene-linear and exposure-independent. Applying eye
    // adaptation before this pass makes old and current samples use different
    // exposure scales and produces brightness ghosting.
    IRhiTexture* scene = m_HdrRt.Get();
    if (!scene) return false;
    const auto temporal = ResolveTemporalHistoryFrame(
        m_TaaFrame,
        p.taa_view_proj_no_jitter,
        p.taa_prev_view_proj_no_jitter,
        p.taa_blend_factor,
        p.taa_motion_texture != nullptr);
    const bool first_frame = (m_TaaFrame == 0);
    IRhiTexture* hist_input = first_frame ? scene : hist_rt;

    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    FPostProcessParams temporal_params = p;
    temporal_params.taa_blend_factor =
        temporal.current_frame_weight;
    temporal_params.taa_motion_texture =
        temporal.motion_vectors_enabled ? p.taa_motion_texture : nullptr;
    UpdatePostCB(
        post_cb, temporal_params,
        1.0f / cur_rt->Width(), 1.0f / cur_rt->Height());

    // TaaReproj CB を埋める。`taa_view_proj_no_jitter` が単位行列の
    // ままなら inv は単位、prev も単位で motion=0 になる (= 静的 reprojection 動作)。
    FTaaReprojCBLayout r{};
    r.inv_view_proj  = Inverse(p.taa_view_proj_no_jitter);
    r.prev_view_proj = temporal.previous_view_projection;
    r.camera_position = FVec4{
        p.taa_camera_position.x, p.taa_camera_position.y,
        p.taa_camera_position.z, 1.0f};
    if (m_CbTaaReproj) m_CbTaaReproj->Update(&r, sizeof(r));

    // t2 slot: motion texture が指定されていればそれを bind、
    // なければ depth (指定があれば実 depth、なければ 1x1 全 255 で sky 扱い)。
    // shader 側は taa_params.z で解釈を切り替えるので PSO の slot 数は不変。
    IRhiTexture* slot2_tex = temporal.motion_vectors_enabled
                           ? p.taa_motion_texture
                           : (p.taa_depth_texture ? p.taa_depth_texture
                                                  : m_TaaDepthFb.Get());
    IRhiTexture* reactive_tex = p.taa_reactive_texture
                              ? p.taa_reactive_texture
                              : m_TaaDepthFb.Get();
    IRhiTexture* reactive_depth = p.taa_depth_texture
                                ? p.taa_depth_texture
                                : m_TaaDepthFb.Get();
    // The TAA PSO declares all five SRVs on every mode. Strict backends do not
    // permit an unbound descriptor merely because the shader branch is
    // disabled, so validate the fallback set before opening the pass and bind
    // every declared slot unconditionally.
    if (!slot2_tex || !reactive_tex || !reactive_depth)
        return false;

    cmd.BeginRenderToTexture(*cur_rt, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeTaaResolve);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetConstantBuffer(1, *m_CbTaaReproj);
    cmd.SetTexture(0, *scene);                     // current scene-linear HDR
    cmd.SetTexture(1, *hist_input);                // history (or current on frame 0)
    cmd.SetTexture(2, *slot2_tex);                 // depth または motion vector
    cmd.SetTexture(3, *reactive_tex);
    cmd.SetTexture(4, *reactive_depth);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur_rt);
    return true;
}

bool CPostProcess::Pass_Tonemap(IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx, const FPostProcessParams& p, IRhiTexture* ldr_target, IRhiTexture* selection_mask, FVec3 selection_color, f32 selection_intensity, f32 selection_thickness_pixels) noexcept {
    if (!m_PipeTonemap || sc.Width() == 0 || sc.Height() == 0) return false;
    if (ldr_target != nullptr
        && (ldr_target->Width() != sc.Width()
            || ldr_target->Height() != sc.Height())) {
        return false;
    }
    const bool scene_auto_exposed =
        p.auto_exposure_enabled &&
        m_ExposureOutputValid &&
        m_ExposedRt;
    const bool expose_ssr =
        p.ssr_texture != nullptr &&
        scene_auto_exposed &&
        m_Exposure[m_AutoFrame % 2];
    FPostProcessParams tonemap_params = p;
    // Inside the tonemap CB this bit describes only whether the separately
    // supplied scene-linear SSR needs the completed adapted exposure.
    tonemap_params.auto_exposure_enabled = expose_ssr;
    IRhiBuffer* post_cb = AcquirePostCb();
    if (!post_cb) return false;
    UpdatePostCB(post_cb, tonemap_params, 1.0f / sc.Width(), 1.0f / sc.Height(), selection_color, selection_intensity, selection_thickness_pixels);

    IRhiTexture* tonemap_src = SceneInput(p);
    if (!tonemap_src) return false;
    IRhiTexture* bloom_input =
        m_BloomOutputValid && m_BloomMips[0]
            ? m_BloomMips[0].Get() : m_BlackFb.Get();
    IRhiTexture* ssr_input =
        p.ssr_texture ? p.ssr_texture : m_BlackFb.Get();
    IRhiTexture* adapted_exposure_input =
        expose_ssr
            ? m_Exposure[m_AutoFrame % 2].Get()
            : m_BlackFb.Get();
    IRhiTexture* selection_input =
        selection_mask != nullptr ? selection_mask : m_BlackFb.Get();
    // Every declared SRV is mandatory on strict backends. A successfully
    // initialized post stack owns m_BlackFb, but fail before beginning the
    // swapchain pass if that invariant is ever broken.
    if (!bloom_input || !ssr_input || !adapted_exposure_input || !selection_input)
        return false;

    if (ldr_target != nullptr) {
        cmd.BeginRenderToTexture(*ldr_target, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    } else {
        cmd.BeginRenderToSwapchain(sc, buf_idx, FClearColor{0, 0, 0, 1}, nullptr, 1.0f);
    }
    cmd.SetPipeline(*m_PipeTonemap);
    cmd.SetConstantBuffer(0, *post_cb);
    cmd.SetTexture(0, *tonemap_src);
    cmd.SetTexture(1, *bloom_input);
    // SSR texture 未指定時は黒 texture を bind する。
    cmd.SetTexture(2, *ssr_input);
    // The mode bit keeps the fallback neutral; no t3 sample is issued on the
    // disabled shader branch even though the strict binding is present.
    cmd.SetTexture(3, *adapted_exposure_input);
    // 無効時もstrict backend向けに黒textureをbindし、shader側flagでsampleを抑止する。
    cmd.SetTexture(4, *selection_input);
    cmd.Draw(3, 0);
    if (ldr_target != nullptr) {
        cmd.EndRenderToTexture(*ldr_target);
    } else {
        cmd.EndRenderToSwapchain(sc, buf_idx);
    }
    return true;
}

bool CPostProcess::EnsureFxaaResources() noexcept {
    if (!m_Device || !m_Fxaa || !m_Fxaa->IsReady()
        || m_Width == 0 || m_Height == 0) {
        return false;
    }
    if (m_FxaaInput && m_FxaaInput->Width() == m_Width
        && m_FxaaInput->Height() == m_Height) {
        return true;
    }

    FTextureDesc description{};
    description.width = m_Width;
    description.height = m_Height;
    description.format = m_ColorFormat;
    description.is_render_target = true;
    auto result = CreateRhiTexture(*m_Device, description);
    if (result.IsErr()) return false;
    m_FxaaInput = Move(result.Value());
    return true;
}

// ==== Auto-exposure passes ====

IRhiTexture* CPostProcess::SceneInput(const FPostProcessParams& p) const noexcept {
    if (p.auto_exposure_enabled && m_ExposureOutputValid && m_ExposedRt) {
        return m_ExposedRt.Get();
    }
    if (p.taa_enabled && m_TaaOutputValid && m_Taa[m_TaaFrame % 2]) {
        return m_Taa[m_TaaFrame % 2].Get();
    }
    return m_HdrRt.Get();
}

bool CPostProcess::Pass_LumaReduce(IRhiCommandList& cmd) noexcept {
    if (!m_HdrRt || m_LumaMipCount == 0 ||
        m_LumaMipCount > kMaxLumaMips ||
        !m_PipeLumaExtract || !m_PipeLumaDown) {
        return false;
    }

    // mip 0: m_HdrRt → m_LumaMips[0]、各 texel で log2 輝度を 4-tap 平均。
    // UpdatePostCB に渡す texel size は「読み元」の 1 texel ぶん。
    {
        auto* dst = m_LumaMips[0].Get();
        if (!dst) return false;
        FPostProcessParams tmp{};
        IRhiBuffer* post_cb = AcquirePostCb();
        if (!post_cb) return false;
        UpdatePostCB(post_cb, tmp, 1.0f / m_HdrRt->Width(), 1.0f / m_HdrRt->Height());
        cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*m_PipeLumaExtract);
        cmd.SetConstantBuffer(0, *post_cb);
        cmd.SetTexture(0, *m_HdrRt);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
    // mip 1..N-1: log2 輝度を box average で 1x1 まで縮約
    for (u32 i = 1; i < m_LumaMipCount; ++i) {
        auto* src = m_LumaMips[i - 1].Get();
        auto* dst = m_LumaMips[i].Get();
        if (!src || !dst) return false;
        FPostProcessParams tmp{};
        IRhiBuffer* post_cb = AcquirePostCb();
        if (!post_cb) return false;
        UpdatePostCB(post_cb, tmp, 1.0f / src->Width(), 1.0f / src->Height());
        cmd.BeginRenderToTexture(*dst, FClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*m_PipeLumaDown);
        cmd.SetConstantBuffer(0, *post_cb);
        cmd.SetTexture(0, *src);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
    return true;
}

bool CPostProcess::Pass_ExposureAdapt(IRhiCommandList& cmd, const FPostProcessParams& p) noexcept {
    if (m_LumaMipCount == 0 || m_LumaMipCount > kMaxLumaMips ||
        !m_PipeExposure || !m_CbAuto) {
        return false;
    }
    auto* avg  = m_LumaMips[m_LumaMipCount - 1].Get();   // 1x1 平均 log2 輝度
    auto* cur  = m_Exposure[m_AutoFrame % 2].Get();        // 今フレームの書き先
    auto* prev = m_Exposure[(m_AutoFrame + 1) % 2].Get();  // 前フレームの露出
    if (!avg || !cur || !prev) return false;

    FAutoExposureCBLayout l{};
    l.a0 = FVec4{ p.auto_exposure_key, p.auto_exposure_min,
                 p.auto_exposure_max, p.auto_exposure_speed };
    // frame 0 は prev が未初期化 (Diligent の未定義メモリ) なので warm=0 で
    // 目標露出を直接採用し、garbage からの補間を回避する (TAA cold-start と同じ考え方)。
    l.a1 = FVec4{ p.delta_time, m_AutoFrame == 0 ? 0.0f : 1.0f, 0.0f, 0.0f };
    m_CbAuto->Update(&l, sizeof(l));

    cmd.BeginRenderToTexture(*cur, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExposure);
    cmd.SetConstantBuffer(0, *m_CbAuto);
    cmd.SetTexture(0, *avg);
    cmd.SetTexture(1, *prev);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur);
    return true;
}

bool CPostProcess::Pass_ExposureApply(IRhiCommandList& cmd,
                                      IRhiTexture& source) noexcept {
    if (!m_ExposedRt || !m_PipeExposeApply) return false;
    auto* exp_tex = m_Exposure[m_AutoFrame % 2].Get();     // Pass_ExposureAdapt が書いた露出
    if (!exp_tex) return false;
    cmd.BeginRenderToTexture(*m_ExposedRt, FClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExposeApply);
    cmd.SetTexture(0, source);
    cmd.SetTexture(1, *exp_tex);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*m_ExposedRt);
    return true;
}

} // namespace acs
