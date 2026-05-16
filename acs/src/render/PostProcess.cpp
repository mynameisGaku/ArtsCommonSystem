// PostProcess (Bloom + ACES Tonemap) 実装
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
    // Karis weighted average で firefly 抑制
    float l = max(max(c.r, c.g), c.b);
    float t = params0.x;
    float knee = max(l - t, 0.0);
    float w = knee / max(l, 0.0001);
    return float4(c * w, 1.0);
}
)";

// Downsample: 13-tap (Jimenez SIGGRAPH 2014 の partial)
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
    // 13-tap Jimenez (簡易版: 6-tap 中心重み付け)
    float3 a = src.Sample(src_sampler, v.uv + float2(-1, -1) * t).rgb;
    float3 b = src.Sample(src_sampler, v.uv + float2( 1, -1) * t).rgb;
    float3 c = src.Sample(src_sampler, v.uv + float2(-1,  1) * t).rgb;
    float3 d = src.Sample(src_sampler, v.uv + float2( 1,  1) * t).rgb;
    float3 e = src.Sample(src_sampler, v.uv).rgb;
    float3 sum = (a + b + c + d) * 0.25 * 0.5 + e * 0.5;
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

// TAA resolve (Phase 34f / 34f-2): current HDR + history HDR + depth →
// reprojected & neighborhood-clamped blend。
//
// Phase 34f-2: 別 CB (TaaReproj at b1) に view_proj + inv_view_proj +
// prev_view_proj を入れて、camera motion 由来の motion vec を計算。
// reproject_enabled = 0 のときは motion=0 で静的 reprojection (frame 34f-1 互換)。
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
    float4 taa_params;    // x=blend_factor (current weight)、y=reproject_enabled
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

    // History を sample する位置: reproject 有効なら motion vec で offset
    float2 hist_uv = v.uv;
    if (taa_params.y >= 0.5) {
        hist_uv = ComputeMotionUv(v.uv);
        // 画面外に飛んだ場合は clamp (border の history が出ないように)
        if (any(hist_uv < 0.0) || any(hist_uv > 1.0)) {
            hist_uv = v.uv;        // fallback: 静的 reprojection
        }
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
    float4 cas_params;// x=cas_strength (0=disable、Phase 34i)
    float4 taa_params;// x=blend_factor (Phase 34f、tonemap は読まないが CB レイアウト整合のため)
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

// Color grading (Phase 34h): tonemap 後 (LDR) に適用する ASC-CDL 風補正。
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

// procedural noise (Inigo Quilez 風) — film grain 用、低コスト
float HashGrain(float2 p, float t) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32 + t);
    return frac(p.x * p.y);
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) Chromatic aberration: 中心から放射方向に R/G/B を分けてサンプル
    float2 center = float2(0.5, 0.5);
    float2 dir = v.uv - center;
    float dist_radial = length(dir);
    float ca = params2.z;
    float3 hdr_col;
    if (ca > 1e-5) {
        float2 ofs = dir * ca;
        hdr_col.r = hdr.Sample(hdr_sampler, v.uv + ofs).r;
        hdr_col.g = hdr.Sample(hdr_sampler, v.uv      ).g;
        hdr_col.b = hdr.Sample(hdr_sampler, v.uv - ofs).b;
    } else {
        hdr_col = hdr.Sample(hdr_sampler, v.uv).rgb;
    }
    hdr_col *= params0.w;       // exposure

    // CAS sharpening (Phase 34i、AMD FSR 簡略版、HDR-aware):
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

    // 2.5) Color grading (Phase 34h、tonemap 後 LDR で適用)
    mapped = ColorGrade(mapped);

    // 3) Vignette (radial darkening)
    float vig_r = max(params2.y, 1e-4);
    float vig_i = params2.x;
    float vig = smoothstep(1.0, vig_r, dist_radial * 1.414);     // 1.414 ~= sqrt(2)
    mapped *= lerp(1.0 - vig_i, 1.0, vig);

    // 4) Film grain (HDR 後、Gamma 前)
    float g_i = params2.w;
    if (g_i > 1e-5) {
        float n = HashGrain(v.uv * 1024.0, params3.x);
        mapped += (n - 0.5) * g_i;
    }

    // 5) Gamma
    mapped = pow(max(mapped, 0.0), 1.0 / max(params1.x, 0.0001));
    return float4(mapped, 1.0);
}
)";

// ==== Auto-exposure (Phase 34k-2) ====
// シーンの平均輝度を GPU で測定し、露出を自動算出する。Phase 34k の CPU eye-
// adaptation を実測輝度ベースへ置き換える。3 種のパスで構成:
//   1) Luma extract/downsample: _hdr_rt → log2 輝度 → mip chain で 1x1 まで縮約
//   2) Exposure adapt: 1x1 平均輝度から目標露出を出し、前フレーム露出へ指数補間
//   3) Exposure apply: _hdr_rt に露出を掛けて _exposed_rt へ
// tonemap PSO の texture slot 数を変えない設計 (project_diligent_slot3_issue 回避):
// 各パスの texture slot は最大 2、tonemap は従来どおり 3 slot のまま。

// Luma extract: HDR → log2 輝度。出力 texel が覆う 2x2 source 領域を 4-tap 平均。
// 幾何平均 (log 空間平均) にすることで少数の高輝度ピクセルに過敏にならない。
const char* kLumaExtractPS = R"(
cbuffer Post : register(b0) {
    float4 params0;
    float4 params1;   // y=texel_w, z=texel_h (source = _hdr_rt)
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

// Exposure apply: _hdr_rt に順応済み露出 (1x1) を掛けて _exposed_rt へ。
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
    Vec4 params0;   // x=threshold, y=intensity, z=radius, w=exposure
    Vec4 params1;   // x=gamma, y=texel_w, z=texel_h, w=tonemap_kind
    Vec4 params2;   // x=vignette_intensity, y=vignette_radius, z=ca, w=grain
    Vec4 params3;   // x=grain_time, y=ssr_intensity
    Vec4 cg0;       // x=saturation, y=contrast, z=temperature, w=tint
    Vec4 cg_lift;   // xyz=lift
    Vec4 cg_gain;   // xyz=gain
    Vec4 cas_params;// x=cas_strength
    Vec4 taa_params;// x=blend_factor (Phase 34f TAA)、y=reproject_enabled (Phase 34f-2)
};

// Phase 34f-2: TAA reprojection 用の別 CB (b1 で bind)。
struct TaaReprojCBLayout {
    Mat4 inv_view_proj;
    Mat4 prev_view_proj;
};

// Phase 34k-2: auto-exposure 用 CB (Exposure adapt パスで b0 に bind)。
struct AutoExposureCBLayout {
    Vec4 a0;   // x=key, y=min_exp, z=max_exp, w=speed
    Vec4 a1;   // x=dt, y=warm (0=cold start)
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

// 全画面三角形 (頂点バッファ無し) のパイプライン共通設定
void FillFullscreenLayout(PipelineDesc& pd) noexcept {
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.vertex_stride = 0;
    pd.layout_count  = 0;
    pd.cull_mode     = CullMode::None;
    pd.depth_test    = false;
    pd.depth_write   = false;
    pd.depth_format  = Format::Unknown;
}

} // namespace

PostProcess::~PostProcess() noexcept {
    Shutdown();
}

Result<void> PostProcess::Init(IRhiDevice& device, u32 width, u32 height,
                                Format color_format) noexcept {
    _device = &device;
    _color_format = color_format;
    _width = width;
    _height = height;

    if (auto r = CreateRenderTargets(device, width, height); r.IsErr()) return r;
    if (auto r = CreatePipelines(device);                   r.IsErr()) return r;

    BufferDesc cbd{};
    cbd.size         = CBSize<PostCBLayout>();
    cbd.usage        = BufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cbr = CreateRhiBuffer(device, cbd);
    if (cbr.IsErr()) return Err<void>(cbr.Error());
    _cb_post = Move(cbr.Value());

    // Phase 34f-2: TaaReproj CB (b1)
    BufferDesc rcbd{};
    rcbd.size = CBSize<TaaReprojCBLayout>();
    rcbd.usage = BufferUsage::Uniform;
    rcbd.cpu_writable = true;
    auto rcbr = CreateRhiBuffer(device, rcbd);
    if (rcbr.IsErr()) return Err<void>(rcbr.Error());
    _cb_taa_reproj = Move(rcbr.Value());

    // Phase 34k-2: auto-exposure 用 CB
    BufferDesc acbd{};
    acbd.size = CBSize<AutoExposureCBLayout>();
    acbd.usage = BufferUsage::Uniform;
    acbd.cpu_writable = true;
    auto acbr = CreateRhiBuffer(device, acbd);
    if (acbr.IsErr()) return Err<void>(acbr.Error());
    _cb_auto = Move(acbr.Value());

    // depth が未指定だった時のための 1x1 fallback (depth>=0.9999 になるよう 255 で fill)
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = Format::R8G8B8A8_UNorm;
    td.initial_data = far_depth; td.initial_data_size = 4;
    auto dfb = CreateRhiTexture(device, td);
    if (dfb.IsErr()) return Err<void>(dfb.Error());
    _taa_depth_fb = Move(dfb.Value());

    return Ok();
}

void PostProcess::Shutdown() noexcept {
    _taa_depth_fb.Reset();
    _cb_auto.Reset();
    _cb_taa_reproj.Reset();
    _cb_post.Reset();
    _pipe_expose_apply.Reset();
    _pipe_exposure.Reset();
    _pipe_luma_down.Reset();
    _pipe_luma_extract.Reset();
    _pipe_tonemap.Reset();
    _pipe_taa_resolve.Reset();
    _pipe_upsample.Reset();
    _pipe_downsample.Reset();
    _pipe_extract.Reset();
    _ps_expose_apply.Reset();
    _ps_exposure.Reset();
    _ps_luma_down.Reset();
    _ps_luma_extract.Reset();
    _ps_tonemap.Reset();
    _ps_taa_resolve.Reset();
    _ps_upsample.Reset();
    _ps_downsample.Reset();
    _ps_extract.Reset();
    _vs_fullscreen.Reset();
    _exposed_rt.Reset();
    for (auto& e : _exposure)   e.Reset();
    for (auto& m : _luma_mips)  m.Reset();
    _luma_mip_count = 0;
    for (auto& t : _taa) t.Reset();
    for (auto& m : _bloom_mips) m.Reset();
    _hdr_rt.Reset();
    _taa_frame  = 0;
    _auto_frame = 0;
    _device = nullptr;
}

Result<void> PostProcess::Resize(u32 width, u32 height) noexcept {
    if (!_device) return ACS_ERR(Render, 300, "PostProcess::Resize before Init");
    if (width == _width && height == _height) return Ok();
    _hdr_rt.Reset();
    for (auto& m : _bloom_mips) m.Reset();
    for (auto& t : _taa) t.Reset();
    for (auto& m : _luma_mips) m.Reset();
    for (auto& e : _exposure)  e.Reset();
    _exposed_rt.Reset();
    _luma_mip_count = 0;
    _width  = width;
    _height = height;
    _taa_frame  = 0;     // reset TAA state on resize (history は size 違いで使えない)
    _auto_frame = 0;     // reset auto-exposure state on resize
    return CreateRenderTargets(*_device, width, height);
}

Result<void> PostProcess::CreateRenderTargets(IRhiDevice& device, u32 w, u32 h) noexcept {
    // メイン HDR RT
    TextureDesc td{};
    td.width  = w;
    td.height = h;
    td.format = _hdr_format;
    td.is_render_target = true;
    auto hr = CreateRhiTexture(device, td);
    if (hr.IsErr()) return Err<void>(hr.Error());
    _hdr_rt = Move(hr.Value());

    // Bloom mip chain (1/2, 1/4, 1/8, 1/16, 1/32)
    u32 mw = w, mh = h;
    for (u32 i = 0; i < kBloomMips; ++i) {
        mw = mw > 1 ? mw / 2 : 1;
        mh = mh > 1 ? mh / 2 : 1;
        TextureDesc bd{};
        bd.width  = mw;
        bd.height = mh;
        bd.format = _hdr_format;
        bd.is_render_target = true;
        auto br = CreateRhiTexture(device, bd);
        if (br.IsErr()) return Err<void>(br.Error());
        _bloom_mips[i] = Move(br.Value());
    }

    // TAA history ping-pong RT (Phase 34f): HDR と同サイズ + 同フォーマット
    for (u32 i = 0; i < 2; ++i) {
        TextureDesc tt{};
        tt.width  = w;
        tt.height = h;
        tt.format = _hdr_format;
        tt.is_render_target = true;
        auto tr = CreateRhiTexture(device, tt);
        if (tr.IsErr()) return Err<void>(tr.Error());
        _taa[i] = Move(tr.Value());
    }

    // ---- Auto-exposure (Phase 34k-2) ----
    // Luma mip chain: _hdr_rt の 1/2 から 1x1 まで縮約する。最深段 (1x1) に
    // シーン平均 log2 輝度が入る。フォーマットは 1ch あれば足りるが、RT として
    // 実績のある R16G16_Float (BRDF LUT と同形式) を使う。
    {
        u32 lw = w, lh = h;
        _luma_mip_count = 0;
        for (u32 i = 0; i < kMaxLumaMips; ++i) {
            lw = lw > 1 ? lw / 2 : 1;
            lh = lh > 1 ? lh / 2 : 1;
            TextureDesc ld{};
            ld.width  = lw;
            ld.height = lh;
            ld.format = _luma_format;
            ld.is_render_target = true;
            auto lr = CreateRhiTexture(device, ld);
            if (lr.IsErr()) return Err<void>(lr.Error());
            _luma_mips[i] = Move(lr.Value());
            ++_luma_mip_count;
            if (lw == 1 && lh == 1) break;
        }
    }
    // 順応済み露出 (1x1 ping-pong)
    for (u32 i = 0; i < 2; ++i) {
        TextureDesc ed{};
        ed.width  = 1;
        ed.height = 1;
        ed.format = _luma_format;
        ed.is_render_target = true;
        auto er = CreateRhiTexture(device, ed);
        if (er.IsErr()) return Err<void>(er.Error());
        _exposure[i] = Move(er.Value());
    }
    // 露出適用後の HDR (下流パスが読む)
    {
        TextureDesc xd{};
        xd.width  = w;
        xd.height = h;
        xd.format = _hdr_format;
        xd.is_render_target = true;
        auto xr = CreateRhiTexture(device, xd);
        if (xr.IsErr()) return Err<void>(xr.Error());
        _exposed_rt = Move(xr.Value());
    }
    return Ok();
}

Result<void> PostProcess::CreatePipelines(IRhiDevice& device) noexcept {
    // ---- 共通 VS ----
    {
        ShaderDesc sd{};
        sd.stage = ShaderStage::Vertex;
        sd.hlsl_source = kFullscreenVS;
        sd.entry_point = "VSMain";
        sd.debug_name  = "Fullscreen.VS";
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        _vs_fullscreen = Move(r.Value());
    }

    // ---- 各 PS ----
    auto compile_ps = [&](const char* src, const char* name,
                          UniquePtr<IRhiShader>& out) -> Result<void> {
        ShaderDesc sd{};
        sd.stage = ShaderStage::Pixel;
        sd.hlsl_source = src;
        sd.entry_point = "PSMain";
        sd.debug_name  = name;
        auto r = CreateRhiShader(device, sd);
        if (r.IsErr()) return Err<void>(r.Error());
        out = Move(r.Value());
        return Ok();
    };
    if (auto r = compile_ps(kExtractPS,     "Bloom.Extract",    _ps_extract);     r.IsErr()) return r;
    if (auto r = compile_ps(kDownsamplePS,  "Bloom.Downsample", _ps_downsample);  r.IsErr()) return r;
    if (auto r = compile_ps(kUpsamplePS,    "Bloom.Upsample",   _ps_upsample);    r.IsErr()) return r;
    if (auto r = compile_ps(kTaaResolvePS,  "Taa.Resolve",      _ps_taa_resolve); r.IsErr()) return r;
    if (auto r = compile_ps(kTonemapPS,     "Tonemap",          _ps_tonemap);     r.IsErr()) return r;
    if (auto r = compile_ps(kLumaExtractPS,    "Luma.Extract",   _ps_luma_extract); r.IsErr()) return r;
    if (auto r = compile_ps(kLumaDownsamplePS, "Luma.Downsample",_ps_luma_down);    r.IsErr()) return r;
    if (auto r = compile_ps(kExposurePS,       "Exposure.Adapt", _ps_exposure);     r.IsErr()) return r;
    if (auto r = compile_ps(kExposeApplyPS,    "Exposure.Apply", _ps_expose_apply); r.IsErr()) return r;

    // ---- Pipelines ----
    // Extract: HDR → bloom_mips[0]、Opaque blend
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_extract.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_extract = Move(r.Value());
    }
    // Downsample: bloom_mips[i] → bloom_mips[i+1]、Opaque
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_downsample.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_downsample = Move(r.Value());
    }
    // Upsample: bloom_mips[i+1] → bloom_mips[i]、Additive blend
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_upsample.Get();
        pd.rt_format = _hdr_format;
        pd.blend_mode = BlendMode::Additive;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_upsample = Move(r.Value());
    }
    // TAA Resolve: current HDR + history HDR + scene_depth → resolved HDR (新 RT)、Opaque
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_taa_resolve.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 2;       // b0=Post, b1=TaaReproj (Phase 34f-2)
        pd.texture_slots = 3;
        pd.cbuffer_names[0] = "Post";
        pd.cbuffer_names[1] = "TaaReproj";
        pd.texture_names[0] = "current_hdr";
        pd.texture_names[1] = "history_hdr";
        pd.texture_names[2] = "scene_depth";
        pd.static_sampler_count = 3;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = SamplerFilter::Linear;
            pd.static_samplers[i].address_u = SamplerAddress::Clamp;
            pd.static_samplers[i].address_v = SamplerAddress::Clamp;
        }
        pd.static_samplers[2].filter    = SamplerFilter::Point;     // depth は離散値
        pd.static_samplers[2].address_u = SamplerAddress::Clamp;
        pd.static_samplers[2].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_taa_resolve = Move(r.Value());
    }

    // Tonemap: HDR + bloom + ssr → backbuffer、Opaque
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_tonemap.Get();
        pd.rt_format = _color_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 3;       // t0=hdr, t1=bloom, t2=ssr (Phase 34e)
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "bloom";
        pd.texture_names[2] = "ssr";
        pd.static_sampler_count = 3;
        for (u32 i = 0; i < 3; ++i) {
            pd.static_samplers[i].filter    = SamplerFilter::Linear;
            pd.static_samplers[i].address_u = SamplerAddress::Clamp;
            pd.static_samplers[i].address_v = SamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_tonemap = Move(r.Value());
    }

    // ---- Auto-exposure pipelines (Phase 34k-2) ----
    // Luma Extract: _hdr_rt → _luma_mips[0]、log2 輝度
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_luma_extract.Get();
        pd.rt_format = _luma_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_luma_extract = Move(r.Value());
    }
    // Luma Downsample: _luma_mips[i] → _luma_mips[i+1]
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_luma_down.Get();
        pd.rt_format = _luma_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 1;
        pd.cbuffer_names[0] = "Post";
        pd.texture_names[0] = "src";
        pd.static_sampler_count = 1;
        pd.static_samplers[0].filter    = SamplerFilter::Linear;
        pd.static_samplers[0].address_u = SamplerAddress::Clamp;
        pd.static_samplers[0].address_v = SamplerAddress::Clamp;
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_luma_down = Move(r.Value());
    }
    // Exposure Adapt: avg luma (1x1) + prev exposure (1x1) → 順応済み露出 (1x1)
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_exposure.Get();
        pd.rt_format = _luma_format;
        pd.cbuffer_slots = 1;
        pd.texture_slots = 2;
        pd.cbuffer_names[0] = "AutoExp";
        pd.texture_names[0] = "avg_luma";
        pd.texture_names[1] = "prev_exp";
        pd.static_sampler_count = 2;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = SamplerFilter::Linear;
            pd.static_samplers[i].address_u = SamplerAddress::Clamp;
            pd.static_samplers[i].address_v = SamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_exposure = Move(r.Value());
    }
    // Exposure Apply: _hdr_rt + 露出 (1x1) → _exposed_rt
    {
        PipelineDesc pd{};
        FillFullscreenLayout(pd);
        pd.vs = _vs_fullscreen.Get();
        pd.ps = _ps_expose_apply.Get();
        pd.rt_format = _hdr_format;
        pd.cbuffer_slots = 0;
        pd.texture_slots = 2;
        pd.texture_names[0] = "hdr";
        pd.texture_names[1] = "exposure";
        pd.static_sampler_count = 2;
        for (u32 i = 0; i < 2; ++i) {
            pd.static_samplers[i].filter    = SamplerFilter::Linear;
            pd.static_samplers[i].address_u = SamplerAddress::Clamp;
            pd.static_samplers[i].address_v = SamplerAddress::Clamp;
        }
        auto r = CreateRhiPipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        _pipe_expose_apply = Move(r.Value());
    }

    return Ok();
}

void PostProcess::Render(IRhiCommandList& cmd, IRhiSwapchain& swapchain, u32 buffer_index,
                          const PostProcessParams& params) noexcept {
    if (!_hdr_rt || !_pipe_extract) return;

    // Auto-exposure (Phase 34k-2): シーン輝度測定 → 露出順応 → 露出適用。
    // TAA / Bloom / Tonemap より前に実行し、下流パスは SceneInput() 経由で
    // 露出適用後の _exposed_rt を読む。条件は有効フラグ + pipeline/RT の存在。
    const bool auto_exp = params.auto_exposure_enabled
                          && _pipe_luma_extract && _exposed_rt;
    if (auto_exp) {
        Pass_LumaReduce(cmd);
        Pass_ExposureAdapt(cmd, params);
        Pass_ExposureApply(cmd);
    }

    // TAA Resolve (Phase 34f): current HDR + previous resolved (history) → new resolved。
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

        // 3) Upsample (additive): mip[i+1] → mip[i] に上書き加算
        for (u32 i = kBloomMips - 1; i > 0; --i) {
            Pass_Upsample(cmd, i - 1, params.bloom_radius);
        }
    }

    // 4) Tonemap: HDR (or TAA resolved) + mip[0] → backbuffer
    Pass_Tonemap(cmd, swapchain, buffer_index, params);

    if (params.taa_enabled) {
        _taa_frame++;
    }
    if (auto_exp) {
        _auto_frame++;
    }
}

namespace {
void UpdatePostCB(IRhiBuffer* cb, const PostProcessParams& p,
                  f32 texel_w, f32 texel_h) noexcept {
    if (!cb) return;
    PostCBLayout l{};
    l.params0 = Vec4{ p.bloom_threshold, p.bloom_intensity, p.bloom_radius, p.exposure };
    l.params1 = Vec4{ p.gamma, texel_w, texel_h, static_cast<f32>(p.tonemap_kind) };
    l.params2 = Vec4{ p.vignette_intensity, p.vignette_radius,
                      p.chromatic_aberration, p.grain_intensity };
    l.params3 = Vec4{ p.grain_time, p.ssr_intensity, 0, 0 };
    l.cg0     = Vec4{ p.cg_saturation, p.cg_contrast, p.cg_temperature, p.cg_tint };
    l.cg_lift = Vec4{ p.cg_lift.x, p.cg_lift.y, p.cg_lift.z, 0 };
    l.cg_gain = Vec4{ p.cg_gain.x, p.cg_gain.y, p.cg_gain.z, 0 };
    l.cas_params = Vec4{ p.cas_strength < 0 ? 0.0f : p.cas_strength, 0, 0, 0 };
    // Phase 34f-2: reproject_enabled は taa_depth_texture が指定されてるかで判定。
    const f32 reproject_enabled = (p.taa_enabled && p.taa_depth_texture) ? 1.0f : 0.0f;
    l.taa_params = Vec4{ p.taa_blend_factor < 0 ? 0.0f : p.taa_blend_factor,
                         reproject_enabled, 0, 0 };
    cb->Update(&l, sizeof(l), 0);
}
} // namespace

void PostProcess::Pass_Extract(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    auto* dst = _bloom_mips[0].Get();
    if (!dst || !_hdr_rt) return;
    UpdatePostCB(_cb_post.Get(), p, 1.0f / dst->Width(), 1.0f / dst->Height());

    // TAA 有効時は resolved (_taa[cur]) を読む。そうでなければ SceneInput
    // (auto-exposure 適用後の _exposed_rt、または raw _hdr_rt)。
    // resolved を読むことで bloom の firefly が temporal stable になり、明滅が消える。
    IRhiTexture* src = SceneInput(p);
    if (p.taa_enabled && _taa[_taa_frame % 2]) {
        src = _taa[_taa_frame % 2].Get();
    }

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_extract);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_Downsample(IRhiCommandList& cmd, u32 from_mip) noexcept {
    auto* src = _bloom_mips[from_mip].Get();
    auto* dst = _bloom_mips[from_mip + 1].Get();
    if (!src || !dst) return;
    PostProcessParams p{};   // params 不要だが texel size のみ更新
    UpdatePostCB(_cb_post.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_downsample);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_Upsample(IRhiCommandList& cmd, u32 to_mip, f32 radius) noexcept {
    auto* src = _bloom_mips[to_mip + 1].Get();
    auto* dst = _bloom_mips[to_mip].Get();
    if (!src || !dst) return;
    PostProcessParams p{};
    p.bloom_radius = radius;
    UpdatePostCB(_cb_post.Get(), p, 1.0f / src->Width(), 1.0f / src->Height());

    // additive blend、clear はせず既存内容に加算
    cmd.SetPipeline(*_pipe_upsample);
    cmd.SetConstantBuffer(0, *_cb_post);
    cmd.SetTexture(0, *src);
    cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*dst);
}

void PostProcess::Pass_TaaResolve(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    auto* cur_rt = _taa[_taa_frame % 2].Get();         // 今フレームの書き先
    auto* hist_rt = _taa[(_taa_frame + 1) % 2].Get();   // 前フレームの resolved
    if (!cur_rt || !hist_rt || !_hdr_rt) return;

    // Cold-start (frame 0): history RT が未書き込み = Diligent の未定義メモリを
    // 読む可能性がある。current_hdr を history slot にも bind することで
    // output = lerp(current, current, a) = current となり、garbage を完全排除。
    // 翌フレームからは history RT に実 resolved が入っているので通常 path。
    // current は SceneInput (auto-exposure 後の _exposed_rt、または raw _hdr_rt)。
    IRhiTexture* scene = SceneInput(p);
    const bool first_frame = (_taa_frame == 0);
    IRhiTexture* hist_input = first_frame ? scene : hist_rt;

    UpdatePostCB(_cb_post.Get(), p, 1.0f / cur_rt->Width(), 1.0f / cur_rt->Height());

    // Phase 34f-2: TaaReproj CB を埋める。`taa_view_proj_no_jitter` が単位行列の
    // ままなら inv は単位、prev も単位で motion=0 になる (= 静的 reprojection 動作)。
    TaaReprojCBLayout r{};
    r.inv_view_proj  = Inverse(p.taa_view_proj_no_jitter);
    r.prev_view_proj = p.taa_prev_view_proj_no_jitter;
    if (_cb_taa_reproj) _cb_taa_reproj->Update(&r, sizeof(r));

    // depth fallback: 指定があれば実 depth、なければ 1x1 全 255 (depth>=0.9999 で sky 扱い)
    IRhiTexture* depth_src = p.taa_depth_texture ? p.taa_depth_texture : _taa_depth_fb.Get();

    cmd.BeginRenderToTexture(*cur_rt, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_taa_resolve);
    cmd.SetConstantBuffer(0, *_cb_post);
    if (_cb_taa_reproj) cmd.SetConstantBuffer(1, *_cb_taa_reproj);
    cmd.SetTexture(0, *scene);                     // current HDR (露出適用後 or raw)
    cmd.SetTexture(1, *hist_input);                // history (or current on frame 0)
    if (depth_src) cmd.SetTexture(2, *depth_src);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur_rt);
}

void PostProcess::Pass_Tonemap(IRhiCommandList& cmd, IRhiSwapchain& sc, u32 buf_idx,
                                const PostProcessParams& p) noexcept {
    UpdatePostCB(_cb_post.Get(), p, 1.0f / sc.Width(), 1.0f / sc.Height());

    // TAA 有効時は _taa[現フレーム index]、そうでなければ SceneInput
    // (auto-exposure 後の _exposed_rt、または raw _hdr_rt) を tonemap input にする。
    IRhiTexture* tonemap_src = SceneInput(p);
    if (p.taa_enabled && _taa[_taa_frame % 2]) {
        tonemap_src = _taa[_taa_frame % 2].Get();
    }

    cmd.BeginRenderToSwapchain(sc, buf_idx, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_tonemap);
    cmd.SetConstantBuffer(0, *_cb_post);
    if (tonemap_src) cmd.SetTexture(0, *tonemap_src);
    if (_bloom_mips[0]) cmd.SetTexture(1, *_bloom_mips[0]);
    // SSR slot: ユーザー指定があれば本物、なければ 0 寄与の bloom mip[最深] を fallback として使う
    // (SSR shader が `* ssr_intensity` で 0 にして無害化)
    if (p.ssr_texture) {
        cmd.SetTexture(2, *p.ssr_texture);
    } else if (_bloom_mips[kBloomMips - 1]) {
        cmd.SetTexture(2, *_bloom_mips[kBloomMips - 1]);  // 1/32 mip、SSR 指定なしなら ssr_intensity=0 で寄与なし
    }
    cmd.Draw(3, 0);
    cmd.EndRenderToSwapchain(sc, buf_idx);
}

// ==== Auto-exposure passes (Phase 34k-2) ====

IRhiTexture* PostProcess::SceneInput(const PostProcessParams& p) const noexcept {
    if (p.auto_exposure_enabled && _exposed_rt) return _exposed_rt.Get();
    return _hdr_rt.Get();
}

void PostProcess::Pass_LumaReduce(IRhiCommandList& cmd) noexcept {
    if (!_hdr_rt || _luma_mip_count == 0 || !_pipe_luma_extract || !_pipe_luma_down) return;

    // mip 0: _hdr_rt → _luma_mips[0]、各 texel で log2 輝度を 4-tap 平均。
    // UpdatePostCB に渡す texel size は「読み元」の 1 texel ぶん。
    {
        auto* dst = _luma_mips[0].Get();
        if (!dst) return;
        PostProcessParams tmp{};
        UpdatePostCB(_cb_post.Get(), tmp, 1.0f / _hdr_rt->Width(), 1.0f / _hdr_rt->Height());
        cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*_pipe_luma_extract);
        cmd.SetConstantBuffer(0, *_cb_post);
        cmd.SetTexture(0, *_hdr_rt);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
    // mip 1..N-1: log2 輝度を box average で 1x1 まで縮約
    for (u32 i = 1; i < _luma_mip_count; ++i) {
        auto* src = _luma_mips[i - 1].Get();
        auto* dst = _luma_mips[i].Get();
        if (!src || !dst) continue;
        PostProcessParams tmp{};
        UpdatePostCB(_cb_post.Get(), tmp, 1.0f / src->Width(), 1.0f / src->Height());
        cmd.BeginRenderToTexture(*dst, ClearColor{0,0,0,1}, nullptr, 1.0f);
        cmd.SetPipeline(*_pipe_luma_down);
        cmd.SetConstantBuffer(0, *_cb_post);
        cmd.SetTexture(0, *src);
        cmd.Draw(3, 0);
        cmd.EndRenderToTexture(*dst);
    }
}

void PostProcess::Pass_ExposureAdapt(IRhiCommandList& cmd, const PostProcessParams& p) noexcept {
    if (_luma_mip_count == 0 || !_pipe_exposure || !_cb_auto) return;
    auto* avg  = _luma_mips[_luma_mip_count - 1].Get();   // 1x1 平均 log2 輝度
    auto* cur  = _exposure[_auto_frame % 2].Get();        // 今フレームの書き先
    auto* prev = _exposure[(_auto_frame + 1) % 2].Get();  // 前フレームの露出
    if (!avg || !cur || !prev) return;

    AutoExposureCBLayout l{};
    l.a0 = Vec4{ p.auto_exposure_key, p.auto_exposure_min,
                 p.auto_exposure_max, p.auto_exposure_speed };
    // frame 0 は prev が未初期化 (Diligent の未定義メモリ) なので warm=0 で
    // 目標露出を直接採用し、garbage からの補間を回避する (TAA cold-start と同じ考え方)。
    l.a1 = Vec4{ p.delta_time, _auto_frame == 0 ? 0.0f : 1.0f, 0.0f, 0.0f };
    _cb_auto->Update(&l, sizeof(l));

    cmd.BeginRenderToTexture(*cur, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_exposure);
    cmd.SetConstantBuffer(0, *_cb_auto);
    cmd.SetTexture(0, *avg);
    cmd.SetTexture(1, *prev);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*cur);
}

void PostProcess::Pass_ExposureApply(IRhiCommandList& cmd) noexcept {
    if (!_hdr_rt || !_exposed_rt || !_pipe_expose_apply) return;
    auto* exp_tex = _exposure[_auto_frame % 2].Get();     // Pass_ExposureAdapt が書いた露出
    if (!exp_tex) return;
    cmd.BeginRenderToTexture(*_exposed_rt, ClearColor{0,0,0,1}, nullptr, 1.0f);
    cmd.SetPipeline(*_pipe_expose_apply);
    cmd.SetTexture(0, *_hdr_rt);
    cmd.SetTexture(1, *exp_tex);
    cmd.Draw(3, 0);
    cmd.EndRenderToTexture(*_exposed_rt);
}

} // namespace acs
