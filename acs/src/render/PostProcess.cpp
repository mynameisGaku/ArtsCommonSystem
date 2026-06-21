// SPDX-License-Identifier: Apache-2.0
// FPostProcess (Bloom + ACES Tonemap) 実装
#include "render/PostProcess.h"
#include "foundation/Move.h"
#include "foundation/Log.h"
#include "math/Vec.h"

#include <cstring>

namespace acs {

namespace {

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

float4 PSMain(VSOut v) : SV_TARGET {
    float3 c = src.Sample(src_sampler, v.uv).rgb;
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

// Downsample: 13-tap Jimenez (Call of Duty: Advanced Warfare、SIGGRAPH 2014)。
// 5 つの partial-box average を Karis-style weighted blend する。
//
// FSample layout (t = 1 source texel):
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

// Upsample: tent filter で広域ぼかしを上の段に加算 (additive blend する想定)
const char* kUpsamplePS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // z=radius
    float4 params1;   // y=texel_w, z=texel_h
};
Texture2D    src : register(t0);
SamplerState src_sampler : register(s0);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float4 PSMain(VSOut v) : SV_TARGET {
    float r = params0.z;
    float2 t = float2(params1.y, params1.z) * r;
    // 9-tap tent filter
    float3 sum = float3(0, 0, 0);
    sum += src.Sample(src_sampler, v.uv + float2(-1,-1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0,-1) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1,-1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2(-1, 0) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0, 0) * t).rgb * (4.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1, 0) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2(-1, 1) * t).rgb * (1.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 0, 1) * t).rgb * (2.0/16.0);
    sum += src.Sample(src_sampler, v.uv + float2( 1, 1) * t).rgb * (1.0/16.0);
    return float4(sum, 1.0);
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
    float4 taa_params;    // x=blend_factor、y=reproject_enabled、z=motion_texture_mode
};
cbuffer TaaReproj : register(b1) {
    float4x4 taa_inv_view_proj;
    float4x4 taa_prev_view_proj;
};
Texture2D    current_hdr : register(t0);
Texture2D    history_hdr : register(t1);
Texture2D    scene_depth : register(t2);
SamplerState current_hdr_sampler : register(s0);
SamplerState history_hdr_sampler : register(s1);
SamplerState scene_depth_sampler : register(s2);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float2 ComputeMotionUv(float2 uv) {
    // 現フレームの depth から world pos を復元、前フレームの VP で clip pos を計算、
    // ndc → uv に戻して motion vec を作る。camera 動きのみ反映 (object 動きは見えない)。
    float depth = scene_depth.SampleLevel(scene_depth_sampler, uv, 0).r;
    if (depth >= 0.9999) return uv;            // sky は motion 0 (history そのまま)
    float4 clip = float4(uv * 2.0 - 1.0, depth, 1.0);
    clip.y = -clip.y;
    float4 wp = mul(clip, taa_inv_view_proj);
    wp.xyz /= max(wp.w, 1e-6);
    float4 prev_clip = mul(float4(wp.xyz, 1.0), taa_prev_view_proj);
    if (prev_clip.w < 1e-4) return uv;
    float2 prev_ndc = prev_clip.xy / prev_clip.w;
    float2 prev_uv = float2(prev_ndc.x * 0.5 + 0.5, -prev_ndc.y * 0.5 + 0.5);
    return prev_uv;
}

float4 PSMain(VSOut v) : SV_TARGET {
    float3 cur = current_hdr.SampleLevel(current_hdr_sampler, v.uv, 0).rgb;

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
    // 画面外に飛んだ場合は clamp (border の history が出ないように)
    if (any(hist_uv < 0.0) || any(hist_uv > 1.0)) {
        hist_uv = v.uv;            // fallback: 静的 reprojection
    }
    float3 hist = history_hdr.SampleLevel(history_hdr_sampler, hist_uv, 0).rgb;

    // Neighborhood AABB clamp: 3x3 neighborhood の current min/max を取り、
    // history がその範囲外なら clamp で抑える。motion 時の古い色を排除する。
    float2 tx = float2(params1.y, params1.z);
    float3 nmin = cur, nmax = cur;
    [unroll]
    for (int dy = -1; dy <= 1; ++dy) {
        [unroll]
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            float3 c = current_hdr.SampleLevel(current_hdr_sampler, v.uv + float2(dx, dy) * tx, 0).rgb;
            nmin = min(nmin, c);
            nmax = max(nmax, c);
        }
    }
    hist = clamp(hist, nmin, nmax);

    float a = saturate(taa_params.x);
    if (a < 1e-4) a = 0.1;          // ガード (CB 0 で全 history になるのを避ける)
    return float4(lerp(hist, cur, a), 1.0);
}
)";

// Tonemap + cinematic post-FX: chromatic aberration → HDR mix → tonemap → vignette → grain → gamma
const char* kTonemapPS = R"(
cbuffer Post : register(b0) {
    float4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    float4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind (0=ACES, 1=AgX, 2=Reinhard ext)
    float4 params2;   // x=vignette_intensity, y=vignette_radius, z=chromatic_aberration, w=grain_intensity
    float4 params3;   // x=grain_time, y=ssr_intensity, zw=pad
    float4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    float4 cg_lift;   // xyz=lift (shadow offset)
    float4 cg_gain;   // xyz=gain (highlight multiplier)
    float4 cas_params;// x=cas_strength (0=disable)
    float4 taa_params;// x=blend_factor (tonemap は読まないが CB レイアウト整合のため)
};
Texture2D    hdr   : register(t0);
Texture2D    bloom : register(t1);
Texture2D    ssr   : register(t2);
SamplerState hdr_sampler   : register(s0);
SamplerState bloom_sampler : register(s1);
SamplerState ssr_sampler   : register(s2);

struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };

float3 ACESFilm(float3 x) {
    // Narkowicz 2016 の近似
    float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return saturate((x * (a * x + b)) / (x * (c * x + d) + e));
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
    // AgxLook 末尾の定数項 -0.00232 と grain 加算で x=0 付近が負値に落ちる
    // ケースがあるので、Filament 公式実装と同じく結果を saturate でガード。
    return saturate(AgxLook(AgxLog(x)));
}

// Reinhard 拡張 (Lottes/Hable 風)
float3 ReinhardExt(float3 x, float white2) {
    return (x * (1.0 + x / white2)) / (1.0 + x);
}

float3 Tonemap(float3 c, int kind) {
    if (kind == 1) return AgxTonemap(c);
    if (kind == 2) return ReinhardExt(c, 16.0);
    return ACESFilm(c);
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
        hdr_col.r = (hdr.Sample(hdr_sampler, v.uv + ofs).r +
                     hdr.Sample(hdr_sampler, v.uv + ofs * 0.5).r) * 0.5;
        hdr_col.g =  hdr.Sample(hdr_sampler, v.uv).g;
        hdr_col.b = (hdr.Sample(hdr_sampler, v.uv - ofs).b +
                     hdr.Sample(hdr_sampler, v.uv - ofs * 0.5).b) * 0.5;
    } else {
        hdr_col = hdr.Sample(hdr_sampler, v.uv).rgb;
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
        float3 nN = hdr.SampleLevel(hdr_sampler, v.uv + float2(0,    -px.y), 0).rgb * params0.w;
        float3 nS = hdr.SampleLevel(hdr_sampler, v.uv + float2(0,     px.y), 0).rgb * params0.w;
        float3 nE = hdr.SampleLevel(hdr_sampler, v.uv + float2( px.x, 0   ), 0).rgb * params0.w;
        float3 nW = hdr.SampleLevel(hdr_sampler, v.uv + float2(-px.x, 0   ), 0).rgb * params0.w;
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

    float3 bloom_col = bloom.Sample(bloom_sampler, v.uv).rgb * params0.y;
    float3 ssr_col   = ssr.Sample(ssr_sampler, v.uv).rgb * params3.y;
    float3 mixed = hdr_col + bloom_col + ssr_col;

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

    // 5) Gamma
    mapped = pow(max(mapped, 0.0), 1.0 / max(params1.x, 0.0001));

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
// tonemap PSO の texture slot 数を変えない設計: 各パスの texture slot は最大 2、
// tonemap は 3 slot のまま。

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
    return float4(c * e, 1.0);
}
)";

// 各パスで使う共通の動的 CB レイアウト
struct PostCBLayout {
    FVec4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    FVec4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind
    FVec4 params2;   // x=vignette_intensity, y=vignette_radius, z=ca, w=grain
    FVec4 params3;   // x=grain_time, y=ssr_intensity
    FVec4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    FVec4 cg_lift;   // xyz=lift
    FVec4 cg_gain;   // xyz=gain
    FVec4 cas_params;// x=cas_strength
    FVec4 taa_params;// x=blend_factor (TAA)、y=reproject_enabled
};

// TAA reprojection 用の別 CB (b1 で bind)。
struct TaaReprojCBLayout {
    FMat4 inv_view_proj;
    FMat4 prev_view_proj;
};

// auto-exposure 用 CB (Exposure adapt パスで b0 に bind)。
struct AutoExposureCBLayout {
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

} // namespace

FPostProcess::~FPostProcess() noexcept {
    Shutdown();
}

TResult<void> FPostProcess::Init(IRhiDevice& device, u32 width, u32 height,
                                EFormat color_format) noexcept {
    m_Device = &device;
    m_ColorFormat = color_format;
    m_Width = width;
    m_Height = height;

    if (auto r = CreateRenderTargets(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipelines(device);                   r.IsErr()) return r;

    FBufferDesc cbd{};
    cbd.size         = CBSize<PostCBLayout>();
    cbd.usage        = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cbr = CreateRhiBuffer(device, cbd);
    if (cbr.IsErr()) return Err<void>(cbr.Error());
    m_CbPost = Move(cbr.Value());

    // TaaReproj CB (b1)
    FBufferDesc rcbd{};
    rcbd.size = CBSize<TaaReprojCBLayout>();
    rcbd.usage = EBufferUsage::Uniform;
    rcbd.cpu_writable = true;
    auto rcbr = CreateRhiBuffer(device, rcbd);
    if (rcbr.IsErr()) return Err<void>(rcbr.Error());
    m_CbTaaReproj = Move(rcbr.Value());

    // auto-exposure 用 CB
    FBufferDesc acbd{};
    acbd.size = CBSize<AutoExposureCBLayout>();
    acbd.usage = EBufferUsage::Uniform;
    acbd.cpu_writable = true;
    auto acbr = CreateRhiBuffer(device, acbd);
    if (acbr.IsErr()) return Err<void>(acbr.Error());
    m_CbAuto = Move(acbr.Value());

    // depth が未指定だった時のための 1x1 fallback (depth>=0.9999 になるよう 255 で fill)
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = far_depth; td.initial_data_size = 4;
    auto dfb = CreateRhiTexture(device, td);
    if (dfb.IsErr()) return Err<void>(dfb.Error());
    m_TaaDepthFb = Move(dfb.Value());

    return Ok();
}

void FPostProcess::Shutdown() noexcept {
    m_TaaDepthFb.Reset();
    m_CbAuto.Reset();
    m_CbTaaReproj.Reset();
    m_CbPost.Reset();
    m_PipeExposeApply.Reset();
    m_PipeExposure.Reset();
    m_PipeLumaDown.Reset();
    m_PipeLumaExtract.Reset();
    m_PipeTonemap.Reset();
    m_PipeTaaResolve.Reset();
    m_PipeUpsample.Reset();
    m_PipeDownsample.Reset();
    m_PipeExtract.Reset();
    m_PsExposeApply.Reset();
    m_PsExposure.Reset();
    m_PsLumaDown.Reset();
    m_PsLumaExtract.Reset();
    m_PsTonemap.Reset();
    m_PsTaaResolve.Reset();
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
    m_HdrRt.Reset();
    m_TaaFrame  = 0;
    m_AutoFrame = 0;
    m_Device = nullptr;
}

TResult<void> FPostProcess::Resize(u32 width, u32 height) noexcept {
    if (!m_Device) return ACS_ERR(Render, 300, "FPostProcess::Resize before Init");
    if (width == m_Width && height == m_Height) return Ok();
    m_HdrRt.Reset();
    for (auto& m : m_BloomMips) m.Reset();
    for (auto& t : m_Taa) t.Reset();
    for (auto& m : m_LumaMips) m.Reset();
    for (auto& e : m_Exposure)  e.Reset();
    m_ExposedRt.Reset();
    m_LumaMipCount = 0;
    m_Width  = width;
    m_Height = height;
    m_TaaFrame  = 0;     // reset TAA state on resize (history は size 違いで使えない)
    m_AutoFrame = 0;     // reset auto-exposure state on resize
    return CreateRenderTargets(*m_Device, width, height);
}

TResult<void> FPostProcess::CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
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
    return Ok();
}

TResult<void> FPostProcess::CreatePipelines(IRhiDevice& device) noexcept {
    // ---- 共通 VS ----
    {
        FShaderDesc sd{};
        sd.stage = EShaderStage::Vertex;
        sd.hlsl_source = kFullscreenVS;
        sd.entry_point = "VSMain";
        sd.debug_name  = "Fullscreen.VS";
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_VsFullscreen = Move(r.Value());
    }

    // ---- 各 PS ----
    auto compile_ps = [&](const char* src, const char* name,
                          TUniquePtr<IRhiShader>& out) -> TResult<void> {
        FShaderDesc sd{};
        sd.stage = EShaderStage::Pixel;
        sd.hlsl_source = src;
        sd.entry_point = "PSMain";
        sd.debug_name  = name;
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        out = Move(r.Value());
        return Ok();
    };
    if (auto r = compile_ps(kExtractPS,     "Bloom.Extract",    m_PsExtract);     r.IsErr()) return r;
    if (auto r = compile_ps(kDownsamplePS,  "Bloom.Downsample", m_PsDownsample);  r.IsErr()) return r;
    if (auto r = compile_ps(kUpsamplePS,    "Bloom.Upsample",   m_PsUpsample);    r.IsErr()) return r;
    if (auto r = compile_ps(kTaaResolvePS,  "Taa.Resolve",      m_PsTaaResolve); r.IsErr()) return r;
    if (auto r = compile_ps(kTonemapPS,     "Tonemap",          m_PsTonemap);     r.IsErr()) return r;
    if (auto r = compile_ps(kLumaExtractPS,    "Luma.Extract",   m_PsLumaExtract); r.IsErr()) return r;
    if (auto r = compile_ps(kLumaDownsamplePS, "Luma.Downsample",m_PsLumaDown);    r.IsErr()) return r;
    if (auto r = compile_ps(kExposurePS,       "Exposure.Adapt", m_PsExposure);     r.IsErr()) return r;
    if (auto r = compile_ps(kExposeApplyPS,    "Exposure.Apply", m_PsExposeApply); r.IsErr()) return r;

    // ---- Pipelines ----
    // Extract: HDR → bloom_mips[0]、Opaque blend
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsExtract.Get();
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
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsDownsample.Get();
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
    // Upsample: bloom_mips[i+1] → bloom_mips[i]、Additive blend
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsUpsample.Get();
        pd.rt_format = m_HdrFormat;
        pd.blend_mode = EBlendMode::Additive;
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
        m_PipeUpsample = Move(r.Value());
    }
    // TAA Resolve: current HDR + history HDR + scene_depth → resolved HDR (新 RT)、Opaque
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsTaaResolve.Get();
        pd.rt_format = m_HdrFormat;
        pd.cbuffer_slots = 2;       // b0=Post, b1=TaaReproj
        pd.texture_slots = 3;
        pd.cbuffer_names[0] = "Post";
        pd.cbuffer_names[1] = "TaaReproj";
        pd.texture_names[0] = "current_hdr";
        pd.texture_names[1] = "history_hdr";
        pd.texture_names[2] = "scene_depth";
        pd.static_sampler_count = 3;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        pd.static_samplers[2].filter    = ESamplerFilter::Point;     // depth は離散値
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_PipeTaaResolve = Move(r.Value());
    }

    // Tonemap: HDR + bloom + ssr → backbuffer、Opaque
    {
        FPipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsTonemap.Get();
        pd.rt_format = m_ColorFormat;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 3;       // t0=hdr, t1=bloom, t2=ssr
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "bloom";
        pd.texture_names[2] = "ssr";
        pd.static_sampler_count = 3;
        for (u32 i = 0; i < 3; ++i) {
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
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsLumaExtract.Get();
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
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsLumaDown.Get();
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
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsExposure.Get();
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
        pd.vs = m_VsFullscreen.Get();
        pd.ps = m_PsExposeApply.Get();
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

    return Ok();
}

void FPostProcess::Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                          const PostProcessParams& params) noexcept {
    if (!m_HdrRt || !m_PipeExtract) return;

    // Auto-exposure: シーン輝度測定 → 露出順応 → 露出適用。
    // TAA / Bloom / Tonemap より前に実行し、下流パスは SceneInput() 経由で
    // 露出適用後の m_ExposedRt を読む。条件は有効フラグ + pipeline/RT の存在。
    const bool auto_exp = params.auto_exposure_enabled
                          && m_PipeLumaExtract && m_ExposedRt;
    if (auto_exp) {
        Pass_LumaReduce(cmd);
        Pass_ExposureAdapt(cmd, params);
        Pass_ExposureApply(cmd);
    }

    // TAA Resolve: current HDR + previous resolved (history) → new resolved。
    // 後段で Pass_Tonemap が resolved を読むよう振る舞う (Pass_Tonemap 側で taa_enabled
    // を見て参照を差し替える)。
    if (params.taa_enabled) {
        Pass_TaaResolve(cmd, params);
    }

    if (params.bloom_enabled) {
        // 1) Extract: HDR (もしくは TAA resolved) → mip[0]
        Pass_Extract(cmd, params);

        // 2) Downsample: mip[i] → mip[i+1]
        for (u32 i = 0; i + 1 < kBloomMips; ++i) {
            Pass_Downsample(cmd, i);
        }

        // 3) Upsample (additive): mip[i+1] → mip[i] に上書き加算。
        //    progressive radius: 深い mip ほど tent 半径を広げ、段間を滑らかに接続して
        //    «広く柔らかい» UE5 風 bloom にする (固定半径だと深い段の広がりが不足しブロッキー)。
        for (u32 i = kBloomMips - 1; i > 0; --i) {
            const f32 r = params.bloom_radius * (1.0f + static_cast<f32>(i - 1) * 0.55f);
            Pass_Upsample(cmd, i - 1, r);
        }
    }

    // 4) Tonemap: HDR (or TAA resolved) + mip[0] → backbuffer
    Pass_Tonemap(cmd, swapchain, buffer_index, params);

    if (params.taa_enabled) {
        m_TaaFrame++;
    }
    if (auto_exp) {
        m_AutoFrame++;
    }
}

namespace {
void UpdatePostCB(IRhiBuffer* cb, const PostProcessParams& p,
                  f32 texel_w, f32 texel_h) noexcept {
    if (!cb) return;
    PostCBLayout l{};
    l.params0 = FVec4{ p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.exposure };
    l.params1 = FVec4{ p.gamma, texel_w, texel_h, static_cast<f32>(p.tonemap_kind) };
    l.params2 = FVec4{ p.vignette_intensity, p.vignette_radius,
                      p.chromatic_aberration, p.grain_intensity };
    l.params3 = FVec4{ p.grain_time, p.ssr_intensity, 0, 0 };
    l.cg0     = FVec4{ p.cg_saturation, p.cg_contrast, p.cg_temperature, p.cg_tint };
    l.cg_lift = FVec4{ p.cg_lift.x, p.cg_lift.y, p.cg_lift.z, 0 };
    l.cg_gain = FVec4{ p.cg_gain.x, p.cg_gain.y, p.cg_gain.z, 0 };
    l.cas_params = FVec4{ p.cas_strength < 0 ? 0.0f : p.cas_strength, 0, 0, 0 };
    // reproject_enabled は taa_depth_texture が指定されてるかで判定。
    const f32 reproject_enabled = (p.taa_enabled && p.taa_depth_texture) ? 1.0f : 0.0f;
    // motion texture があれば motion mode (depth reprojection より優先)。
    const f32 motion_mode = (p.taa_enabled && p.taa_motion_texture) ? 1.0f : 0.0f;
    l.taa_params = FVec4{ p.taa_blend_factor < 0 ? 0.0f : p.taa_blend_factor,
                         reproject_enabled, motion_mode, 0 };
    cb->Update(&l, sizeof(l), 0);
}
} // namespace

void FPostProcess::Pass_Extract(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    auto* dst = m_BloomMips[0].Get();
    if (!dst || !m_HdrRt) return;
    UpdatePostCB(m_CbPost.Get(), p, 1.0f / dst->Width(), 1.0f / dst->Height());

    // TAA 有効時は resolved (m_Taa[cur]) を読む。そうでなければ SceneInput
    // (auto-exposure 適用後の m_ExposedRt、または raw m_HdrRt)。
    // resolved を読むことで bloom の firefly が temporal stable になり、明滅が消える。
    IRhiTexture* src = SceneInput(p);
    if (p.taa_enabled && m_Taa[m_TaaFrame % 2]) {
        src = m_Taa[m_TaaFrame % 2].Get();
    }

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExtract);
    cmd.SetConstantBuffer(0, *m_CbPost);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void FPostProcess::Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept {
    auto* src = m_BloomMips[from_mip].Get();
    auto* dst = m_BloomMips[from_mip + 1].Get();
    if (!src || !dst) return;
    PostProcessParams p{};   // params 不要だが texel size のみ更新
    UpdatePostCB(m_CbPost.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeDownsample);
    cmd.SetConstantBuffer(0, *m_CbPost);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void FPostProcess::Pass_Upsample(IRhiCommandList& cmd, u32 to_mip, f32 radius) noexcept {
    auto* src = m_BloomMips[to_mip + 1].Get();
    auto* dst = m_BloomMips[to_mip].Get();
    if (!src || !dst) return;
    PostProcessParams p{};
    p.bloom_radius = radius;
    UpdatePostCB(m_CbPost.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    // additive blend、clear はせず既存内容に加算
    cmd.SetPipeline(*m_PipeUpsample);
    cmd.SetConstantBuffer(0, *m_CbPost);
    cmd.SetTexture(0, *src);
    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void FPostProcess::Pass_TaaResolve(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    auto* cur_rt = m_Taa[m_TaaFrame % 2].Get();         // 今フレームの書き先
    auto* hist_rt = m_Taa[(m_TaaFrame + 1) % 2].Get();   // 前フレームの resolved
    if (!cur_rt || !hist_rt || !m_HdrRt) return;

    // Cold-start (frame 0): history RT が未書き込み = Diligent の未定義メモリを
    // 読む可能性がある。current_hdr を history slot にも bind することで
    // output = lerp(current, current, a) = current となり、garbage を完全排除。
    // 翌フレームからは history RT に実 resolved が入っているので通常 path。
    // current は SceneInput (auto-exposure 後の m_ExposedRt、または raw m_HdrRt)。
    IRhiTexture* scene = SceneInput(p);
    const bool first_frame = (m_TaaFrame == 0);
    IRhiTexture* hist_input = first_frame ? scene : hist_rt;

    UpdatePostCB(m_CbPost.Get(), p, 1.0f / cur_rt->Width(), 1.0f / cur_rt->Height());

    // TaaReproj CB を埋める。`taa_view_proj_no_jitter` が単位行列の
    // ままなら inv は単位、prev も単位で motion=0 になる (= 静的 reprojection 動作)。
    TaaReprojCBLayout r{};
    r.inv_view_proj  = Inverse(p.taa_view_proj_no_jitter);
    r.prev_view_proj = p.taa_prev_view_proj_no_jitter;
    if (m_CbTaaReproj) m_CbTaaReproj->Update(&r, sizeof(r));

    // t2 slot: motion texture が指定されていればそれを bind、
    // なければ depth (指定があれば実 depth、なければ 1x1 全 255 で sky 扱い)。
    // shader 側は taa_params.z で解釈を切り替えるので PSO の slot 数は不変。
    IRhiTexture* slot2_tex = p.taa_motion_texture
                           ? p.taa_motion_texture
                           : (p.taa_depth_texture ? p.taa_depth_texture
                                                  : m_TaaDepthFb.Get());

    cmd.BeginRenderToTexture(*cur_rt, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeTaaResolve);
    cmd.SetConstantBuffer(0, *m_CbPost);
    if (m_CbTaaReproj) cmd.SetConstantBuffer(1, *m_CbTaaReproj);
    cmd.SetTexture(0, *scene);                     // current HDR (露出適用後 or raw)
    cmd.SetTexture(1, *hist_input);                // history (or current on frame 0)
    if (slot2_tex) cmd.SetTexture(2, *slot2_tex);  // depth または motion vector
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur_rt);
}

void FPostProcess::Pass_Tonemap(IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                                const PostProcessParams& p) noexcept {
    UpdatePostCB(m_CbPost.Get(), p, 1.0f / sc.Width(), 1.0f / sc.Height());

    // TAA 有効時は m_Taa[現フレーム index]、そうでなければ SceneInput
    // (auto-exposure 後の m_ExposedRt、または raw m_HdrRt) を tonemap input にする。
    IRhiTexture* tonemap_src = SceneInput(p);
    if (p.taa_enabled && m_Taa[m_TaaFrame % 2]) {
        tonemap_src = m_Taa[m_TaaFrame % 2].Get();
    }

    cmd.BeginRenderToSwapchain(sc, buf_idx, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeTonemap);
    cmd.SetConstantBuffer(0, *m_CbPost);
    if (tonemap_src) cmd.SetTexture(0, *tonemap_src);
    if (m_BloomMips[0]) cmd.SetTexture(1, *m_BloomMips[0]);
    // SSR slot: ユーザー指定があれば本物、なければ 0 寄与の bloom mip[最深] を fallback として使う
    // (SSR shader が `* ssr_intensity` で 0 にして無害化)
    if (p.ssr_texture) {
        cmd.SetTexture(2, *p.ssr_texture);
    } else if (m_BloomMips[kBloomMips - 1]) {
        cmd.SetTexture(2, *m_BloomMips[kBloomMips - 1]);  // 1/32 mip、SSR 指定なしなら ssr_intensity=0 で寄与なし
    }
    cmd.Draw(3, 0);
    cmd.EndRenderToSwapchain(sc, buf_idx);
}

// ==== Auto-exposure passes ====

IRhiTexture* FPostProcess::SceneInput(const PostProcessParams& p) const noexcept {
    if (p.auto_exposure_enabled && m_ExposedRt) return m_ExposedRt.Get();
    return m_HdrRt.Get();
}

void FPostProcess::Pass_LumaReduce(IRhiCommandList& cmd) noexcept {
    if (!m_HdrRt || m_LumaMipCount == 0 || !m_PipeLumaExtract || !m_PipeLumaDown) return;

    // mip 0: m_HdrRt → m_LumaMips[0]、各 texel で log2 輝度を 4-tap 平均。
    // UpdatePostCB に渡す texel size は「読み元」の 1 texel ぶん。
    {
        auto* dst = m_LumaMips[0].Get();
        if (!dst) return;
        PostProcessParams tmp{};
        UpdatePostCB(m_CbPost.Get(), tmp, 1.0f / m_HdrRt->Width(), 1.0f / m_HdrRt->Height());
        cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*m_PipeLumaExtract);
        cmd.SetConstantBuffer(0, *m_CbPost);
        cmd.SetTexture(0, *m_HdrRt);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
    // mip 1..N-1: log2 輝度を box average で 1x1 まで縮約
    for (u32 i = 1; i < m_LumaMipCount; ++i) {
        auto* src = m_LumaMips[i - 1].Get();
        auto* dst = m_LumaMips[i].Get();
        if (!src || !dst) continue;
        PostProcessParams tmp{};
        UpdatePostCB(m_CbPost.Get(), tmp, 1.0f / src->Width(), 1.0f / src->Height());
        cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*m_PipeLumaDown);
        cmd.SetConstantBuffer(0, *m_CbPost);
        cmd.SetTexture(0, *src);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
}

void FPostProcess::Pass_ExposureAdapt(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    if (m_LumaMipCount == 0 || !m_PipeExposure || !m_CbAuto) return;
    auto* avg  = m_LumaMips[m_LumaMipCount - 1].Get();   // 1x1 平均 log2 輝度
    auto* cur  = m_Exposure[m_AutoFrame % 2].Get();        // 今フレームの書き先
    auto* prev = m_Exposure[(m_AutoFrame + 1) % 2].Get();  // 前フレームの露出
    if (!avg || !cur || !prev) return;

    AutoExposureCBLayout l{};
    l.a0 = FVec4{ p.auto_exposure_key, p.auto_exposure_min,
                 p.auto_exposure_max, p.auto_exposure_speed };
    // frame 0 は prev が未初期化 (Diligent の未定義メモリ) なので warm=0 で
    // 目標露出を直接採用し、garbage からの補間を回避する (TAA cold-start と同じ考え方)。
    l.a1 = FVec4{ p.delta_time, m_AutoFrame == 0 ? 0.0f : 1.0f, 0.0f, 0.0f };
    m_CbAuto->Update(&l, sizeof(l));

    cmd.BeginRenderToTexture(*cur, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExposure);
    cmd.SetConstantBuffer(0, *m_CbAuto);
    cmd.SetTexture(0, *avg);
    cmd.SetTexture(1, *prev);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur);
}

void FPostProcess::Pass_ExposureApply(IRhiCommandList& cmd) noexcept {
    if (!m_HdrRt || !m_ExposedRt || !m_PipeExposeApply) return;
    auto* exp_tex = m_Exposure[m_AutoFrame % 2].Get();     // Pass_ExposureAdapt が書いた露出
    if (!exp_tex) return;
    cmd.BeginRenderToTexture(*m_ExposedRt, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*m_PipeExposeApply);
    cmd.SetTexture(0, *m_HdrRt);
    cmd.SetTexture(1, *exp_tex);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*m_ExposedRt);
}

} // namespace acs
