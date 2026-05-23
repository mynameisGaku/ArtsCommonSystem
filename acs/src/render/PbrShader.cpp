// SPDX-License-Identifier: Apache-2.0
// PbrShader 実装 — Cook-Torrance BRDF (GGX + Smith + Schlick)
#include "render/PbrShader.h"
#include "asset/MeshAsset.h"   // MeshVertex
#include "foundation/Move.h"   // Move
#include "foundation/Log.h"

#include <cstring>

namespace acs {

namespace {

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

// HLSL: PBR Cook-Torrance。StandardShader と同じ vertex 入力 (pos / nrm / uv)
// + 同じ vs 出力 (world_p / world_n / uv)。
const char* kPbrHLSL = R"(
#pragma pack_matrix(row_major)

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

#define ACS_MAX_AREA_LIGHTS 2

cbuffer Frame : register(b0) {
    float4x4 view_proj;
    float4   camera_pos;                              // xyz=eye, w=pad
    float4   ambient;                                 // xyz=ambient color, w=dir_count
    float4   point_count_pad;                         // x=point_count, y=area_count
    float4   light_dir  [ACS_MAX_DIR_LIGHTS];
    float4   light_color[ACS_MAX_DIR_LIGHTS];
    float4   point_pos_range [ACS_MAX_POINT_LIGHTS];
    float4   point_color     [ACS_MAX_POINT_LIGHTS];
    float4   ibl_params;                              // x=ibl_enabled, y=prefilter_mip_count, z=use_sh9
    float4   sh9[9];                                  // SH 9 coefficients (RGB)、z モードで使用
    // 矩形 area light: center (world), axis_x*half_width, axis_y*half_height, color
    float4   area_center [ACS_MAX_AREA_LIGHTS];
    float4   area_axis_x [ACS_MAX_AREA_LIGHTS];
    float4   area_axis_y [ACS_MAX_AREA_LIGHTS];
    float4   area_color  [ACS_MAX_AREA_LIGHTS];

    // 静的光プローブグリッド (Phase 33d)
    float4   probe_params;            // x=probe_count (0..4)
    float4   probe_pos [4];           // 各 probe world pos (xyz)
    float4   probe_sh9 [4 * 9];       // 各 probe の SH 9 係数 (xyz=RGB)

    // Volumetric fog (Phase 33e)
    float4   fog_color_density;       // xyz=fog color, w=density (0=off)
    float4   fog_height_params;       // x=height_falloff, y=fog_height_base, zw=pad

    // Shadow map (Phase 34b + CSM = Cascaded Shadow Map)
    // shadow_view_proj[c]: 各 cascade の light VP (最大 4 cascade)
    // shadow_params       : x=bias, y=enabled (0/1), z=texel_size (cascade-local), w=filter_radius
    // cascade_splits      : xyzw = 各 cascade の view-space z far (cascade 選択の閾値)
    //                       single mode は全成分が inf で常に cascade 0
    // cascade_uv_scale    : x=atlas X 方向のスケール (single mode=1、N cascade=1/N)
    //                       y=1.0 (常)、zw=pad
    float4x4 shadow_view_proj[4];
    float4   shadow_params;
    float4   cascade_splits;
    float4   cascade_uv_scale;

    // SSAO (Phase 34j-2): screen-space AO テクスチャを ambient/indirect 項に乗算。
    //   x = enabled (0/1)
    //   y = intensity (0=neutral, 1=通常、>1=強調)
    //   z = 1 / viewport_width
    //   w = 1 / viewport_height
    float4   ssao_params;

    // SSGI (Phase 33c): screen-space indirect light を ambient に加算。
    //   x = enabled (0/1)、y = intensity (typical 0.5..2.0)、zw = pad
    float4   ssgi_params;

    // Lightmap (Phase 33f): baked static GI を mesh の uv で sample して
    // ambient/indirect 項に加算。SSGI と排他ではない (両方有効化可)。
    //   x = enabled (0/1)、y = intensity、zw = pad
    float4   lightmap_params;

    // SSR (Phase 34e-2fix): screen-space reflection を IBL specular に blend。
    //   x = enabled (0/1)、y = intensity、zw = pad。screen UV は ssao_params.zw を流用。
    float4   ssr_params;
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   base_color;     // xyz=color, w=alpha
    float4   pbr_params;     // x=metallic, y=roughness, z=ao, w=pad
    float4   ext_params;     // x=clearcoat (0..1)、y=clearcoat_roughness (0..1)
                             // z=anisotropy (-1..1)、w=enable_flags (bit0=clearcoat, bit1=aniso)
    float4   aniso_tangent;  // xyz=anisotropic tangent direction (world)、w=pad
    float4   emissive;       // Phase 34l: xyz=自己発光色 * strength、w=pad
    float4   sheen_params;   // Phase 35-1a: xyz=sheen color, w=sheen weight (0=OFF)
    float4   sheen_rough;    // Phase 35-1a: x=sheen roughness, yzw=pad
    float4   irid_params;    // Phase 35-1b: x=weight, y=thickness(nm), z=film IOR, w=pad
    float4   sss_params;     // Phase 35-2: xyz=subsurface color, w=weight (0=OFF)
};

Texture2D    albedo           : register(t0);
TextureCube  irradiance        : register(t1);
TextureCube  prefilter         : register(t2);
Texture2D    brdf_lut          : register(t3);
Texture2D    normal_map        : register(t4);
Texture2D    shadow_map        : register(t5);
Texture2D    ssao_map          : register(t6);
Texture2D    ssgi_color        : register(t7);
Texture2D    lightmap          : register(t8);
Texture2D    ssr_color         : register(t9);
SamplerState albedo_sampler     : register(s0);
SamplerState irradiance_sampler : register(s1);
SamplerState prefilter_sampler  : register(s2);
SamplerState brdf_lut_sampler   : register(s3);
SamplerState normal_map_sampler : register(s4);
SamplerState shadow_map_sampler : register(s5);
SamplerState ssao_map_sampler   : register(s6);
SamplerState ssgi_color_sampler : register(s7);
SamplerState lightmap_sampler   : register(s8);
SamplerState ssr_color_sampler  : register(s9);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut {
    float4 pos     : SV_POSITION;
    float3 world_p : POSITION;
    float3 world_n : NORMAL;
    float2 uv      : TEXCOORD0;
    float  view_z  : TEXCOORD1;     // CSM の cascade 選択に使う view-space depth
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    o.uv      = v.uv;
    // LH perspective では clip.w == view-space z。CSM cascade 選択に使う。
    o.view_z  = o.pos.w;
    return o;
}

static const float PI = 3.14159265358979323846;

// GGX (Trowbridge-Reitz) normal distribution
float D_GGX(float NdotH, float a2) {
    float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
    return a2 / max(PI * d * d, 1e-7);
}

// Smith joint geometry (Schlick-GGX). k = (roughness+1)^2 / 8 for direct light.
float G_Smith(float NdotV, float NdotL, float roughness) {
    float k = (roughness + 1.0);
    k = k * k * 0.125;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    return Gv * Gl;
}

// Schlick Fresnel approximation
float3 F_Schlick(float VdotH, float3 F0) {
    float f = pow(saturate(1.0 - VdotH), 5.0);
    return F0 + (1.0 - F0) * f;
}

// PCSS (Percentage-Closer Soft Shadow、Fernando 2005)。
// 1) blocker search: receiver の周囲で実際の occluder の avg depth を求める
// 2) penumbra width = (receiver - blocker_avg) / blocker_avg * light_size_uv
// 3) penumbra に応じた radius で PCF (4x4)、receiver が遠いほど影が柔らかく
// 完全遮蔽 / 完全照明の早期 return で blocker が無い場合の負荷を抑える。
//
// 単純な 4-tap PCF にフォールバックしたいときは shadow_params.w に 0 を渡す
// (filter_radius、デフォルト 1.0 = PCSS、0 = hard 4-tap)
//
// 単一 cascade ぶんの PCSS シャドウ係数 (atlas UV へ展開、kernel leak 防止)。
// 戻り値 1.0 = 完全照明 / 0.0 = 完全遮蔽 / NDC out も 1.0 (cascade 範囲外)。
float SamplePcssCascade(int cascade, float3 world_p) {
    float4 lp = mul(float4(world_p, 1.0), shadow_view_proj[cascade]);
    float3 ndc = lp.xyz / lp.w;
    if (ndc.x < -1.0 || ndc.x > 1.0 ||
        ndc.y < -1.0 || ndc.y > 1.0 ||
        ndc.z <  0.0 || ndc.z > 1.0) {
        return 1.0;
    }

    // cascade-local UV → atlas UV
    float2 base_uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
    float  scale_x = cascade_uv_scale.x;            // single=1, N-cascade=1/N
    float  ofs_x   = float(cascade) * scale_x;      // atlas X 開始位置

    float my_d   = ndc.z;
    float bias   = shadow_params.x;
    float ts     = shadow_params.z;                 // cascade-local texel size
    float kFilt  = shadow_params.w;                 // 0=hard PCF、>0=PCSS

    // atlas_uv.x は cascade 領域 [ofs_x, ofs_x+scale_x] にクランプして
    // 隣接 cascade に kernel が漏れて偽の影/光が混入するのを防ぐ (atlas-boundary PCF leak)。
    // 半 texel の inset で隣接 cascade 0 と完全に分離する。
    const float kHalfTexelInset = ts * 0.5 * scale_x;

    // ---- Blocker search (PCSS Fernando 2005、16 tap、半径 = 4 * texel) ----
    float blocker_sum = 0;
    int   blocker_cnt = 0;
    const int kBlockerN = 4;
    float search_r = ts * 4.0;
    [unroll]
    for (int by = -kBlockerN/2; by < kBlockerN/2; ++by) {
        [unroll]
        for (int bx = -kBlockerN/2; bx < kBlockerN/2; ++bx) {
            float2 off       = float2(bx, by) * (search_r * 0.5);
            float2 atlas_uv  = (base_uv + off) * float2(scale_x, 1.0) + float2(ofs_x, 0);
            atlas_uv.x = clamp(atlas_uv.x, ofs_x + kHalfTexelInset,
                                            ofs_x + scale_x - kHalfTexelInset);
            float sd = shadow_map.SampleLevel(shadow_map_sampler, atlas_uv, 0).r;
            if (sd + bias < my_d) {
                blocker_sum += sd;
                blocker_cnt += 1;
            }
        }
    }

    if (blocker_cnt == 0) return 1.0;
    if (blocker_cnt == kBlockerN * kBlockerN) return 0.0;

    float blocker_avg = blocker_sum / float(blocker_cnt);
    // penumbra ≒ (receiver - blocker) * light_size / blocker (UV 上の太陽径は固定)
    float penumbra = max((my_d - blocker_avg) / max(blocker_avg, 1e-3), 0.0);
    float filter_r = max(penumbra * 0.01 * kFilt, ts);

    // ---- 4x4 PCF (stratified) で penumbra-sized blur ----
    float lit = 0;
    const int kPcfN = 4;
    [unroll]
    for (int py = 0; py < kPcfN; ++py) {
        [unroll]
        for (int px = 0; px < kPcfN; ++px) {
            float2 jitter = float2((float)px / (kPcfN-1) - 0.5,
                                   (float)py / (kPcfN-1) - 0.5);
            float2 off      = jitter * filter_r * 2.0;
            float2 atlas_uv = (base_uv + off) * float2(scale_x, 1.0) + float2(ofs_x, 0);
            atlas_uv.x = clamp(atlas_uv.x, ofs_x + kHalfTexelInset,
                                            ofs_x + scale_x - kHalfTexelInset);
            float sd = shadow_map.SampleLevel(shadow_map_sampler, atlas_uv, 0).r;
            lit += (sd + bias >= my_d) ? 1.0 : 0.0;
        }
    }
    return lit / float(kPcfN * kPcfN);
}

// CSM 全体 (cascade 選択 + boundary blending)。
// Phase 36-2 cascade blending: 各 cascade の末尾 kBlendRatio (=15%) は
// 次 cascade と線形ブレンドして「カスケード seam」(解像度切替の影段差) を除去。
// blend 領域では PCSS を 2 回呼ぶので約 2x コスト、それ以外は単発。
float ComputeShadow(float3 world_p, float view_z) {
    if (shadow_params.y < 0.5) return 1.0;

    // ---- Cascade selection by view-space z (Phase 34b part 3 CSM) ----
    // single mode では cascade_splits.xyzw = inf なので常に cascade 0、
    // ブレンドも next_split=inf で発動しない (= 後方互換)。
    int cascade = 0;
    if (view_z > cascade_splits.x) cascade = 1;
    if (view_z > cascade_splits.y) cascade = 2;
    if (view_z > cascade_splits.z) cascade = 3;

    float shadow_c = SamplePcssCascade(cascade, world_p);

    // ---- Cascade boundary blending (Phase 36-2) ----
    // 末尾 cascade (=3 or cascade_splits[c+1]>=1e29) は次が無いので blend しない。
    float prev_split = (cascade == 0) ? 0.0 :
                       (cascade == 1) ? cascade_splits.x :
                       (cascade == 2) ? cascade_splits.y :
                                        cascade_splits.z;
    float next_split = (cascade == 0) ? cascade_splits.x :
                       (cascade == 1) ? cascade_splits.y :
                       (cascade == 2) ? cascade_splits.z :
                                        1e30;

    if (next_split < 1e29) {
        const float kBlendRatio = 0.15;
        float cascade_range = max(next_split - prev_split, 1e-3);
        float blend_width   = cascade_range * kBlendRatio;
        float blend_start   = next_split - blend_width;
        if (view_z > blend_start) {
            float t = saturate((view_z - blend_start) / blend_width);
            float shadow_next = SamplePcssCascade(cascade + 1, world_p);
            shadow_c = lerp(shadow_c, shadow_next, t);
        }
    }

    return shadow_c;
}

// ddx/ddy 由来の screen-space TBN を使って tangent-space normal を world-space に変換。
// vertex tangent が無くても normal map を使える Schueler 法。
float3 PerturbNormal(float3 worldP, float3 worldN, float2 uv, float3 tangent_n) {
    float3 dp_dx = ddx(worldP);
    float3 dp_dy = ddy(worldP);
    float2 duv_dx = ddx(uv);
    float2 duv_dy = ddy(uv);
    // determinant 不変な TBN 構築
    float3 dp_dy_perp = cross(dp_dy, worldN);
    float3 dp_dx_perp = cross(worldN, dp_dx);
    float3 t = dp_dy_perp * duv_dx.x + dp_dx_perp * duv_dy.x;
    float3 b = dp_dy_perp * duv_dx.y + dp_dx_perp * duv_dy.y;
    float invmax = rsqrt(max(dot(t, t), dot(b, b)));
    float3 N_perturbed = normalize(t * (tangent_n.x * invmax)
                                  + b * (tangent_n.y * invmax)
                                  + worldN * tangent_n.z);
    return N_perturbed;
}

// Karis "Real Shading in UE4" 流 Fresnel with roughness。
// 低 NoV (grazing) で粗い表面は full Fresnel しないという経験的補正。
float3 FresnelSchlickRoughness(float cosTheta, float3 F0, float roughness) {
    float3 r = (float3)(1.0 - roughness);
    return F0 + (max(r, F0) - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Anisotropic GGX (Walter / Heitz)。
// αx, αy は tangent / bitangent 方向別の roughness² (アスペクト比は anisotropy で制御)。
float D_GGX_Aniso(float NoH, float ToH, float BoH, float ax, float ay) {
    float t1 = ToH / max(ax, 1e-4);
    float t2 = BoH / max(ay, 1e-4);
    float t3 = NoH;
    float a2 = t1 * t1 + t2 * t2 + t3 * t3;
    return 1.0 / max(PI * ax * ay * a2 * a2, 1e-7);
}

// Anisotropic Smith G (Heitz 2014 separable form)
float G_Smith_Aniso(float NoV, float ToV, float BoV,
                   float NoL, float ToL, float BoL,
                   float ax, float ay) {
    float lambdaV = NoL * sqrt(ax * ax * ToV * ToV + ay * ay * BoV * BoV + NoV * NoV);
    float lambdaL = NoV * sqrt(ax * ax * ToL * ToL + ay * ay * BoL * BoL + NoL * NoL);
    return 0.5 / max(lambdaV + lambdaL, 1e-7);
}

// Clear-coat layer (top-coat lacquer, dielectric IOR ≈ 1.5 → F0 = 0.04)。
// 戻り値: (1) coat の specular [* NoL]、(2) 入射光が base 層に届く割合 (= 1 - F_coat)
struct ClearcoatTerm { float3 spec_times_nol; float attenuation; };
ClearcoatTerm EvalClearcoat(float3 N, float3 V, float3 L,
                            float clearcoat, float coat_roughness) {
    ClearcoatTerm o;
    o.spec_times_nol = float3(0, 0, 0);
    o.attenuation    = 1.0;
    if (clearcoat <= 0.0) return o;

    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return o;
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a  = max(coat_roughness * coat_roughness, 1e-3);
    float a2 = a * a;
    float Dc = a2 / max(PI * pow(NdotH * NdotH * (a2 - 1.0) + 1.0, 2.0), 1e-7);
    // 簡易 Smith G (isotropic、coat 層は metallic 不可なので一般 GGX で十分)
    float k = (coat_roughness + 1.0); k = k * k * 0.125;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    float Gc = Gv * Gl;
    float Fc = 0.04 + (1.0 - 0.04) * pow(saturate(1.0 - VdotH), 5.0);

    float spec = (Dc * Gc * Fc) / max(4.0 * NdotV * NdotL, 1e-7);
    o.spec_times_nol = float3(spec, spec, spec) * NdotL * clearcoat;
    // base 層への透過: 1 - Fc * clearcoat (energy conservation)
    o.attenuation = 1.0 - Fc * clearcoat;
    return o;
}

// プローブ grid (Phase 33d): 各 probe ごとに SH9 を保持し、world_p から距離重み付き
// (IDW、power=4 で局所性強め) で blend する。1 probe ならその係数をそのまま使う。
float3 ProbeGridIrradiance(float3 world_p, float3 N);

// SH 9 reconstruction (Ramamoorthi-Hanrahan 2001、Stupid SH Tricks p.6)
//   irradiance(N) ≈ c4*L[0] - c5*L[6] + c3*L[6]*z² + c1*L[8]*(x²-y²)
//                + 2 c1 (L[4]*xy + L[7]*xz + L[5]*yz) + 2 c2 (L[3]*x + L[1]*y + L[2]*z)
float3 Sh9Irradiance(float3 N, float4 L[9]) {
    const float c1 = 0.429043;
    const float c2 = 0.511664;
    const float c3 = 0.743125;
    const float c4 = 0.886227;
    const float c5 = 0.247708;
    float x = N.x, y = N.y, z = N.z;
    float3 e = c4 * L[0].rgb
             + 2.0 * c2 * (L[3].rgb * x + L[1].rgb * y + L[2].rgb * z)
             + c5 * (-L[6].rgb)
             + 2.0 * c1 * (L[4].rgb * x * y + L[7].rgb * x * z + L[5].rgb * y * z)
             + c3 * L[6].rgb * z * z
             + c1 * L[8].rgb * (x * x - y * y);
    return max(e, float3(0, 0, 0));    // clamp 負値 (アンダーシュート対策)
}
)" R"(
float3 ProbeGridIrradiance(float3 world_p, float3 N) {
    int n = (int)probe_params.x;
    if (n <= 0) return float3(0, 0, 0);
    // IDW: w[i] = 1 / (dist²)²
    float total_w = 0.0;
    float ws[4] = {0, 0, 0, 0};
    [unroll]
    for (int i = 0; i < 4; ++i) {
        if (i >= n) { ws[i] = 0; continue; }
        float3 d = world_p - probe_pos[i].xyz;
        float dist2 = max(dot(d, d), 1e-4);
        ws[i] = 1.0 / (dist2 * dist2);
        total_w += ws[i];
    }
    float inv = 1.0 / max(total_w, 1e-6);
    float4 blended[9];
    [unroll]
    for (int k = 0; k < 9; ++k) blended[k] = float4(0, 0, 0, 0);
    [unroll]
    for (int j = 0; j < 4; ++j) {
        if (j >= n) break;
        float w = ws[j] * inv;
        [unroll]
        for (int k = 0; k < 9; ++k) {
            blended[k] += probe_sh9[j * 9 + k] * w;
        }
    }
    return max(Sh9Irradiance(N, blended), float3(0, 0, 0));
}

// IBL ambient (split-sum approximation):
//   diffuse_ibl  = (1 - F) * (1 - metallic) * base * irradiance.Sample(N)
//   specular_ibl = prefilter.SampleLevel(R, roughness * (mips-1)) * (F0 * lut.r + lut.g)
float3 ComputeIblAmbient(float3 N, float3 V, float3 world_p, float3 base,
                        float metallic, float roughness, float ao,
                        float3 ssr_radiance, float ssr_weight,
                        float3 sheenColor, float sheenWeight,
                        float3 iridFresnel, float iridWeight)
{
    float NoV = saturate(dot(N, V));
    float3 R  = reflect(-V, N);

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float3 F  = FresnelSchlickRoughness(NoV, F0, roughness);

    float3 kd = (1.0 - F) * (1.0 - metallic);
    // diffuse: probe grid (Phase 33d) > SH9 single (Phase 32c) > cubemap (Phase 31) の優先順
    float3 irr;
    if (probe_params.x >= 1.0) {
        irr = ProbeGridIrradiance(world_p, N);
    } else if (ibl_params.z >= 0.5) {
        irr = Sh9Irradiance(N, sh9);
    } else {
        irr = irradiance.SampleLevel(irradiance_sampler, N, 0).rgb;
    }
    float3 diffuse_ibl = kd * base * irr;
    // Sheen ambient (Phase 35-1a): irradiance を sheen 色で着色した簡易 ambient + base 減衰
    if (sheenWeight > 0.0) {
        float maxC = max(sheenColor.r, max(sheenColor.g, sheenColor.b));
        diffuse_ibl = diffuse_ibl * saturate(1.0 - sheenWeight * maxC * 0.5)
                    + irr * sheenColor * sheenWeight * 0.25;
    }

    // prefilter は mip = roughness * (mip_count - 1)。Sample (with HW mip
    // selection) ではなく SampleLevel で明示することで filtering を確実にする。
    float mip_lvl = roughness * max(ibl_params.y - 1.0, 0.0);
    float3 prefilt = prefilter.SampleLevel(prefilter_sampler, R, mip_lvl).rgb;
    float2 lut_xy = brdf_lut.SampleLevel(brdf_lut_sampler, float2(NoV, roughness), 0).rg;
    // Phase 34e-2fix: 反射元の radiance を環境 prefilter (off-screen) から SSR
    // (on-screen の実ジオメトリ) へ blend。BRDF 応答 (split-sum scale+bias) は共通。
    float3 reflected = lerp(prefilt, ssr_radiance, saturate(ssr_weight));
    // Iridescence (Phase 35-1b): split-sum の F0 を薄膜変調した値へ差し替える
    float3 specF0 = lerp(F0, iridFresnel, iridWeight);
    float3 specular_ibl = reflected * (specF0 * lut_xy.x + lut_xy.y);

    return (diffuse_ibl + specular_ibl) * ao;
}

// IBL specular for clear-coat layer (split-sum、F0=0.04 固定の dielectric)。
// 戻り値: (1) coat の IBL specular、(2) base 層への透過率 (= 1 - F_coat)。
struct IblCoatTerm { float3 spec; float attenuation; };
IblCoatTerm ComputeIblClearcoat(float3 N, float3 V, float clearcoat, float coat_roughness) {
    IblCoatTerm o;
    o.spec = float3(0, 0, 0);
    o.attenuation = 1.0;
    if (clearcoat <= 0.0) return o;
    float NoV = saturate(dot(N, V));
    float3 R  = reflect(-V, N);
    float3 F0c = float3(0.04, 0.04, 0.04);
    float3 Fc  = FresnelSchlickRoughness(NoV, F0c, coat_roughness);
    float mip_lvl = coat_roughness * max(ibl_params.y - 1.0, 0.0);
    float3 prefilt = prefilter.SampleLevel(prefilter_sampler, R, mip_lvl).rgb;
    float2 lut_xy = brdf_lut.SampleLevel(brdf_lut_sampler, float2(NoV, coat_roughness), 0).rg;
    o.spec = prefilt * (F0c * lut_xy.x + lut_xy.y) * clearcoat;
    // base 透過: Fresnel * clearcoat 強度ぶんを引く
    o.attenuation = 1.0 - max(Fc.r, max(Fc.g, Fc.b)) * clearcoat;
    return o;
}
)" R"(
// ===== Iridescence: thin-film interference (Phase 35-1b) =====
// Belcour & Barla 2017 の解析モデル (glTF KHR_materials_iridescence 準拠)。
// 薄膜の多重反射の干渉を spectral に評価し RGB へ射影する。LUT 不要。

// IOR → 垂直入射 Fresnel reflectance (F0)
float IorToFresnel0(float transmitted, float incident) {
    float r = (transmitted - incident) / (transmitted + incident);
    return r * r;
}
float3 IorToFresnel0(float3 transmitted, float incident) {
    float3 r = (transmitted - incident) / (transmitted + incident);
    return r * r;
}
// F0 → IOR (下地マテリアルの IOR を base F0 から復元)
float3 Fresnel0ToIor(float3 f0) {
    float3 s = sqrt(clamp(f0, 0.0, 0.9999));
    return (1.0 + s) / (1.0 - s);
}
// Schlick Fresnel (薄膜の界面反射用、scalar / RGB の overload)
float IridFresnel(float cosT, float f0) {
    float t = saturate(1.0 - cosT); float t2 = t * t;
    return f0 + (1.0 - f0) * (t2 * t2 * t);
}
float3 IridFresnel(float cosT, float3 f0) {
    float t = saturate(1.0 - cosT); float t2 = t * t;
    return f0 + (1.0 - f0) * (t2 * t2 * t);
}

// 光路差 OPD (nm) を CIE color matching の Gaussian fit で RGB 応答へ (Belcour-Barla)。
float3 EvalSensitivity(float opd, float3 shift) {
    float  phase = 2.0 * PI * opd * 1.0e-9;
    float3 val = float3(5.4856e-13, 4.4201e-13, 5.2481e-13);
    float3 pos = float3(1.6810e+06, 1.7953e+06, 2.2084e+06);
    float3 var = float3(4.3278e+09, 9.3046e+09, 6.6121e+09);
    float3 xyz = val * sqrt(2.0 * PI * var) * cos(pos * phase + shift)
               * exp(-(phase * phase) * var);
    xyz.x += 9.7470e-14 * sqrt(2.0 * PI * 4.5282e+09)
           * cos(2.2399e+06 * phase + shift.x) * exp(-4.5282e+09 * (phase * phase));
    xyz /= 1.0685e-7;
    // XYZ → linear sRGB (Rec.709 / D65)
    float3x3 XYZ_TO_RGB = float3x3( 3.2404542, -1.5371385, -0.4985314,
                                   -0.9692660,  1.8760108,  0.0415560,
                                    0.0556434, -0.2040259,  1.0572252);
    return mul(XYZ_TO_RGB, xyz);
}

// 薄膜干渉で base の Fresnel F0 を変調した RGB reflectance を返す (Belcour-Barla 2017)。
//   outsideIor : 媒質 IOR (空気 = 1.0)、filmIor: 薄膜 IOR、cosTheta1: 入射角 cos、
//   thicknessNm: 膜厚 (nm)、baseF0: 下地マテリアルの F0 (RGB)。
float3 EvalIridescence(float outsideIor, float filmIor, float cosTheta1,
                       float thicknessNm, float3 baseF0) {
    // 膜厚 0 付近で filmIor を媒質側へ寄せ、効果を滑らかに消す
    float iridIor = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thicknessNm));
    // 膜内の屈折角 (Snell の法則)
    float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                      * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq < 0.0) return float3(1.0, 1.0, 1.0);   // 全反射
    float cosTheta2 = sqrt(cosTheta2Sq);

    // 第 1 界面 (媒質 → 膜): 反射率と位相シフト
    float R12   = IridFresnel(cosTheta1, IorToFresnel0(iridIor, outsideIor));
    float T121  = 1.0 - R12;
    float phi12 = (iridIor < outsideIor) ? PI : 0.0;
    float phi21 = PI - phi12;

    // 第 2 界面 (膜 → 下地): 反射率と位相シフト (RGB 別)
    float3 baseIor = Fresnel0ToIor(baseF0);
    float3 R23   = IridFresnel(cosTheta2, IorToFresnel0(baseIor, iridIor));
    float3 phi23 = float3((baseIor.x < iridIor) ? PI : 0.0,
                          (baseIor.y < iridIor) ? PI : 0.0,
                          (baseIor.z < iridIor) ? PI : 0.0);

    // 光路差と総位相
    float  opd = 2.0 * iridIor * thicknessNm * cosTheta2;
    float3 phi = float3(phi21, phi21, phi21) + phi23;

    // 多重反射の等比級数 (Airy 総和)
    float3 R123 = clamp(R12 * R23, 1e-5, 0.9999);
    float3 r123 = sqrt(R123);
    float3 Rs   = (T121 * T121) * R23 / (float3(1.0, 1.0, 1.0) - R123);

    // m=0 (DC) 項
    float3 I = float3(R12, R12, R12) + Rs;
    // m>0 (干渉) 項: dirac ペアを 2 次まで
    float3 Cm = Rs - float3(T121, T121, T121);
    [unroll]
    for (int m = 1; m <= 2; ++m) {
        Cm *= r123;
        float3 Sm = 2.0 * EvalSensitivity(float(m) * opd, float(m) * phi);
        I += Cm * Sm;
    }
    return max(I, float3(0.0, 0.0, 0.0));
}

// Cook-Torrance BRDF * NdotL (light vector L is from surface to light source).
// anisotropy ≠ 0 のとき D / G を anisotropic 版に切替える。tangent (T) はオブジェクト
// から与えられる主軸方向、B (bitangent) は cross(N, T) で生成。
float3 BrdfCookTorrance(float3 N, float3 V, float3 L,
                       float3 base, float metallic, float roughness,
                       float anisotropy, float3 T_world,
                       float3 iridFresnel, float iridWeight)
{
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    if (NdotL <= 0.0) return float3(0, 0, 0);
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = lerp(float3(0.04, 0.04, 0.04), base, metallic);
    float  a  = roughness * roughness;
    float  D, G;
    float3 F = F_Schlick(VdotH, F0);
    // Iridescence (Phase 35-1b): 薄膜干渉で base Fresnel を変調 (NoV 評価済の値へ blend)。
    F = lerp(F, iridFresnel, iridWeight);

    if (abs(anisotropy) > 1e-3) {
        // anisotropy=-1 で完全に tangent 方向に伸びる、+1 で bitangent 方向に。
        // ax / ay は a を中心に anisotropy で偏らせる (Filament 流)。
        float aniso = clamp(anisotropy, -0.99, 0.99);
        float ax = max(a * (1.0 + aniso), 1e-3);
        float ay = max(a * (1.0 - aniso), 1e-3);
        float3 T_unit = normalize(T_world - N * dot(T_world, N));     // Gram-Schmidt
        float3 B_unit = cross(N, T_unit);
        float ToH = dot(T_unit, H), BoH = dot(B_unit, H);
        float ToV = dot(T_unit, V), BoV = dot(B_unit, V);
        float ToL = dot(T_unit, L), BoL = dot(B_unit, L);
        D = D_GGX_Aniso(NdotH, ToH, BoH, ax, ay);
        G = G_Smith_Aniso(NdotV, ToV, BoV, NdotL, ToL, BoL, ax, ay) * 4.0 * NdotV * NdotL;
        // 注: G_Smith_Aniso は (G / (4 NoV NoL)) 形式 (Heitz 2014) なので、
        //   spec = D F G_native / (4 NoV NoL) に揃えるため逆スケール
    } else {
        D = D_GGX(NdotH, a * a);
        G = G_Smith(NdotV, NdotL, roughness);
    }

    float3 specular = (D * G * F) / max(4.0 * NdotV * NdotL, 1e-7);
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * base / PI;
    return (diffuse + specular) * NdotL;
}

// Charlie sheen distribution (Estevez & Kulla 2017)。布/ベルベットの retro-reflective ローブ。
float D_Charlie(float NoH, float sheenRoughness) {
    float a   = max(sheenRoughness, 1e-3);
    float inv = 1.0 / a;
    float s2  = saturate(1.0 - NoH * NoH);   // sin^2(theta)
    return (2.0 + inv) * pow(s2, inv * 0.5) / (2.0 * PI);
}

// Neubelt visibility (Filament cloth)。柔らかい減衰、解析式で LUT 不要。
float V_Neubelt(float NoV, float NoL) {
    return saturate(1.0 / (4.0 * (NoL + NoV - NoL * NoV)));
}

// per-pixel マテリアル (1 光源ぶんの BRDF 評価に要るパラメータ束)。
// Phase 35: lobe パラメータ (sheen / 今後 iridescence / SSS) はここに足していく。
struct SurfaceMaterial {
    float3 albedo;
    float  metallic;
    float  roughness;
    float  clearcoat;
    float  coat_roughness;
    float  anisotropy;
    float3 aniso_tangent;
    float3 sheen_color;       // Phase 35-1a: 布の毛羽色
    float  sheen_weight;      // 0 = sheen OFF
    float  sheen_roughness;
    float3 irid_fresnel;      // Phase 35-1b: 薄膜変調済 Fresnel (NoV で評価済)
    float  irid_weight;       // 0 = iridescence OFF
    float3 sss_color;         // Phase 35-2: subsurface (内部散乱) の色
    float  sss_weight;        // 0 = SSS OFF
};

// 1 光源ぶんの layered BRDF を評価 (base Cook-Torrance + clearcoat 層 + sheen 層)。
// L = surface→light、戻り値は放射輝度係数 (光色・距離減衰・shadow は呼び側で乗算)。
// dir/area/point の 3 ループはこの 1 関数を呼ぶだけにし、lobe 追加を 1 箇所に集約する。
float3 EvalSurfaceForLight(float3 N, float3 V, float3 L, SurfaceMaterial m) {
    float3 base_brdf = BrdfCookTorrance(N, V, L, m.albedo, m.metallic, m.roughness,
                                        m.anisotropy, m.aniso_tangent,
                                        m.irid_fresnel, m.irid_weight);
    ClearcoatTerm cc = EvalClearcoat(N, V, L, m.clearcoat, m.coat_roughness);
    float3 lit = base_brdf * cc.attenuation + cc.spec_times_nol;

    // Sheen (Phase 35-1a): 布/ベルベットの fuzz 層を base に加算。エネルギー保存の
    // ため base を簡易減衰 (厳密な sheen directional albedo は将来 LUT 化を検討)。
    if (m.sheen_weight > 0.0) {
        float  NoL = saturate(dot(N, L));
        float  NoV = saturate(dot(N, V)) + 1e-5;
        float3 H   = normalize(V + L);
        float  NoH = saturate(dot(N, H));
        float3 sheen = m.sheen_color
                     * (D_Charlie(NoH, m.sheen_roughness) * V_Neubelt(NoV, NoL))
                     * NoL * m.sheen_weight;
        float  maxC = max(m.sheen_color.r, max(m.sheen_color.g, m.sheen_color.b));
        // saturate: SetSheen が範囲クランプ済だが、CB を直接書く経路でも負にしない防御。
        lit = lit * saturate(1.0 - m.sheen_weight * maxC * 0.5) + sheen;
    }

    // Subsurface scattering (Phase 35-2): 肌/ロウ/玉 の内部散乱を解析近似で加算。
    // (1) wrapped diffuse の Lambert 超過分 = terminator の柔らかい回り込み。
    // (2) 裏面 translucency = 薄い部分を光が透ける逆光グロー (Barre-Brisebois 2011)。
    // LUT も追加 pass も不要。直接光専用 (IBL は terminator が無いので非適用)。
    if (m.sss_weight > 0.0) {
        const float kWrap = 0.5, kDistortion = 0.2, kPower = 3.0, kTransScale = 0.6;
        float  noL    = dot(N, L);
        float  noL_w  = saturate((noL + kWrap) / (1.0 + kWrap));
        float  wrapEx = max(noL_w - saturate(noL), 0.0);          // (1)
        float3 Lt     = normalize(-L + N * kDistortion);
        float  trans  = pow(saturate(dot(V, Lt)), kPower);        // (2)
        lit += m.sss_color * m.albedo
             * (wrapEx / PI + trans * kTransScale) * m.sss_weight;
    }
    return lit;
}
)" R"(
float4 PSMain(VSOut v) : SV_TARGET {
    float3 N = normalize(v.world_n);
    // Normal map perturbation (Phase 34g)。fallback 1x1 (0.5,0.5,1.0) なら無変化。
    float3 nm = normal_map.Sample(normal_map_sampler, v.uv).rgb * 2.0 - 1.0;
    // 単位長に正規化 (sampler の linear interp で長さズレが起きるため)
    nm = normalize(nm + float3(0, 0, 1e-6));
    N = PerturbNormal(v.world_p, N, v.uv, nm);
    float3 V = normalize(camera_pos.xyz - v.world_p);

    float3 albedo_rgb = albedo.Sample(albedo_sampler, v.uv).rgb * base_color.xyz;
    float  metallic   = pbr_params.x;
    float  roughness  = max(pbr_params.y, 0.04);     // 0 は数値不安定
    float  ao         = pbr_params.z;
    // 拡張 material
    float  clearcoat       = saturate(ext_params.x);
    float  coat_roughness  = max(ext_params.y, 0.04);
    float  anisotropy      = ext_params.z;
    float3 aniso_T_world   = aniso_tangent.xyz;

    // 光源ループ 3 種 (dir/area/point) が共有する per-pixel マテリアル
    SurfaceMaterial mat;
    mat.albedo         = albedo_rgb;
    mat.metallic       = metallic;
    mat.roughness      = roughness;
    mat.clearcoat      = clearcoat;
    mat.coat_roughness = coat_roughness;
    mat.anisotropy     = anisotropy;
    mat.aniso_tangent  = aniso_T_world;
    mat.sheen_color     = sheen_params.xyz;
    mat.sheen_weight    = sheen_params.w;
    mat.sheen_roughness = sheen_rough.x;
    // Iridescence (Phase 35-1b): 薄膜 Fresnel を per-pixel に 1 度だけ NoV で評価。
    mat.irid_weight  = irid_params.x;
    mat.irid_fresnel = float3(0.0, 0.0, 0.0);
    if (irid_params.x > 0.0) {
        float3 F0_base = lerp(float3(0.04, 0.04, 0.04), albedo_rgb, metallic);
        mat.irid_fresnel = EvalIridescence(1.0, irid_params.z, saturate(dot(N, V)),
                                           irid_params.y, F0_base);
    }
    mat.sss_color  = sss_params.xyz;          // Phase 35-2
    mat.sss_weight = sss_params.w;

    // SSAO modulation factor (Phase 34j-2): screen-space AO テクスチャから visibility を読み、
    // ambient/indirect 項に掛ける。direct light には影響しない (物理的に AO は indirect 専用)。
    // ssao_params.x=0 で無効 (factor=1)。ssao_params.zw = 1/viewport_size。
    // SSAO map の .r = AO visibility、.g = contact shadow (Phase 34q)。
    float ssao_factor    = 1.0;
    float contact_shadow = 1.0;
    if (ssao_params.x >= 0.5) {
        float2 screen_uv = v.pos.xy * float2(ssao_params.z, ssao_params.w);
        float2 ssao_rg = ssao_map.SampleLevel(ssao_map_sampler, screen_uv, 0).rg;
        ssao_factor    = saturate(1.0 - (1.0 - ssao_rg.x) * ssao_params.y);
        contact_shadow = ssao_rg.y;
    }

    // SSR (Phase 34e-2fix): screen-space reflection を roughness 依存の blur で
    // sample する。rough 面ほど反射をぼかし weight も下げる → smooth 面は SSR を、
    // rough 面は環境 prefilter を反射元に使う物理的に正しい挙動になる。
    // screen UV / viewport は SSAO の値 (ssao_params.zw) を流用する。
    float3 ssr_rgb    = float3(0, 0, 0);
    float  ssr_weight = 0.0;
    if (ssr_params.x >= 0.5) {
        float2 ssr_uv = v.pos.xy * float2(ssao_params.z, ssao_params.w);
        float  blur   = roughness * 0.015;
        float3 sum    = float3(0, 0, 0);
        float  amask  = 0.0;
        [unroll]
        for (int sy = -1; sy <= 1; ++sy) {
            [unroll]
            for (int sx = -1; sx <= 1; ++sx) {
                float4 s = ssr_color.SampleLevel(ssr_color_sampler,
                              ssr_uv + float2(sx, sy) * blur, 0);
                sum   += s.rgb;
                amask += s.a;
            }
        }
        ssr_rgb    = sum * (1.0 / 9.0);
        // hit mask * intensity * (1-roughness) で rough 面ほど SSR を弱める
        ssr_weight = (amask / 9.0) * ssr_params.y * (1.0 - roughness);
    }

    // 環境光: ibl_params.x が 1 なら IBL ambient、0 なら flat ambient。
    // uniform branching なので片方の TextureCube サンプルは PSO の dead code
    // として削除される (FXC の判断による)。
    float3 col;
    if (ibl_params.x >= 0.5) {
        float3 base_ibl = ComputeIblAmbient(N, V, v.world_p, albedo_rgb, metallic, roughness, ao,
                                            ssr_rgb, ssr_weight, sheen_params.xyz, sheen_params.w,
                                            mat.irid_fresnel, mat.irid_weight);
        IblCoatTerm cc_ibl = ComputeIblClearcoat(N, V, clearcoat, coat_roughness);
        col = (base_ibl * cc_ibl.attenuation + cc_ibl.spec * ao) * ssao_factor;
    } else {
        col = ambient.xyz * albedo_rgb * ao * ssao_factor;
    }

    // SSGI (Phase 33c): screen-space 1 bounce indirect light を ambient に加算。
    // 非金属の albedo に modulate (Lambertian-like)、AO も乗せる (SSGI ray 自身は
    // hemisphere sampling だが、SSAO の幾何遮蔽でさらに抑える)。
    if (ssgi_params.x >= 0.5) {
        float2 screen_uv = v.pos.xy * float2(ssao_params.z, ssao_params.w);
        float3 gi = ssgi_color.SampleLevel(ssgi_color_sampler, screen_uv, 0).rgb;
        col += gi * albedo_rgb * (1.0 - metallic) * ssgi_params.y * ssao_factor;
    }

    // Lightmap (Phase 33f): mesh の uv で baked static GI を sample して
    // ambient に加算。非金属の diffuse のみ (metallic surface には baked light は乗らない)。
    if (lightmap_params.x >= 0.5) {
        float3 lm = lightmap.SampleLevel(lightmap_sampler, v.uv, 0).rgb;
        col += lm * albedo_rgb * (1.0 - metallic) * lightmap_params.y;
    }

    // 有向光源 (i==0 のみ shadow_map で遮蔽)
    float shadow = ComputeShadow(v.world_p, v.view_z);
    int dir_count = (int)ambient.w;
    [unroll]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        // i==0 (= sun) は shadow map (PCSS) と contact shadow (Phase 34q) の両方で遮蔽
        float k = (i == 0) ? (shadow * contact_shadow) : 1.0;
        col += light_color[i].xyz * EvalSurfaceForLight(N, V, L, mat) * k;
    }

    // 矩形 area light: 4x4 stratified sample で Monte Carlo 積分。
    // それぞれのサンプル点を point light として扱い、area 全面積で正規化する。
    // (面積光の特徴: 軟らかい highlight elongation、近距離での明るい照り)
    int area_count = (int)point_count_pad.y;
    [unroll]
    for (int a = 0; a < ACS_MAX_AREA_LIGHTS; ++a) {
        if (a >= area_count) break;
        float3 area_c = area_center[a].xyz;
        float3 axisX  = area_axis_x[a].xyz;
        float3 axisY  = area_axis_y[a].xyz;
        float3 col_a  = area_color[a].xyz;
        // 面の normal は cross(axisX, axisY) 正規化方向
        float3 area_n = normalize(cross(axisX, axisY));
        // light 後ろ側にあるピクセルは寄与なし (両面 emit でないとして)
        float facing = dot(area_n, v.world_p - area_c);
        if (facing > 0.0) {
            float3 area_sum = float3(0, 0, 0);
            const int kSamples = 4;
            [unroll]
            for (int sy = 0; sy < kSamples; ++sy) {
                float vy = ((float)sy + 0.5) / (float)kSamples * 2.0 - 1.0;
                [unroll]
                for (int sx = 0; sx < kSamples; ++sx) {
                    float vx = ((float)sx + 0.5) / (float)kSamples * 2.0 - 1.0;
                    float3 sample_pos = area_c + axisX * vx + axisY * vy;
                    float3 to_l = sample_pos - v.world_p;
                    float  dist = length(to_l);
                    float3 L = to_l / max(dist, 1e-4);
                    // inverse square + 面要素の cos(法線方向)
                    float cos_area = saturate(-dot(area_n, L));
                    float att = cos_area / max(dist * dist, 1e-4);
                    area_sum += EvalSurfaceForLight(N, V, L, mat) * att;
                }
            }
            // (axisX × axisY の長さ) = 面の半面積 ×4。N_sample で割って full area で乗算。
            float area = 4.0 * length(cross(axisX, axisY));
            col += col_a * area_sum * (area / (float)(kSamples * kSamples));
        }
    }

    // 点光源 (距離減衰)
    int pt_count = (int)point_count_pad.x;
    [unroll]
    for (int j = 0; j < ACS_MAX_POINT_LIGHTS; ++j) {
        if (j >= pt_count) break;
        float3 to_light = point_pos_range[j].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng  = max(point_pos_range[j].w, 1e-4);
        if (dist >= rng) continue;
        float3 L = to_light / max(dist, 1e-4);
        float  att = 1.0 - dist / rng;
        att = att * att;
        col += point_color[j].xyz * EvalSurfaceForLight(N, V, L, mat) * att;
    }

    // Emissive (Phase 34l): 自己発光。lighting と無関係に加算する。fog より前に
    // 足すので、発光体も距離フォグで減衰する (遠くの発光は霞む)。
    col += emissive.rgb;

    // Volumetric fog (Phase 33e): exponential height fog
    //   density = fog_density * exp(-height_falloff * (world_y - fog_base))
    //   transmittance = exp(-density * dist)
    if (fog_color_density.w > 0.0) {
        float3 to_cam = v.world_p - camera_pos.xyz;
        float dist = length(to_cam);
        // height attenuation (h を上にいくと density 減衰、地面近くで濃い)
        float h = v.world_p.y - fog_height_params.y;
        float density = fog_color_density.w * exp(-fog_height_params.x * max(h, 0.0));
        float transmittance = exp(-density * dist);
        col = col * transmittance + fog_color_density.xyz * (1.0 - transmittance);
    }

    return float4(col, base_color.w);
}
)";

constexpr u32 kMaxDirLights   = 4;
constexpr u32 kMaxPointLights = 4;
constexpr u32 kMaxAreaLights  = 2;
struct FrameCBLayout {
    Mat4 view_proj;
    Vec4 camera_pos;
    Vec4 ambient;
    Vec4 point_count_pad;       // x=point_count, y=area_count
    Vec4 light_dir   [kMaxDirLights];
    Vec4 light_color [kMaxDirLights];
    Vec4 point_pos_range[kMaxPointLights];
    Vec4 point_color    [kMaxPointLights];
    Vec4 ibl_params;
    Vec4 sh9[9];
    Vec4 area_center [kMaxAreaLights];
    Vec4 area_axis_x [kMaxAreaLights];
    Vec4 area_axis_y [kMaxAreaLights];
    Vec4 area_color  [kMaxAreaLights];
    Vec4 probe_params;
    Vec4 probe_pos[4];
    Vec4 probe_sh9[4 * 9];
    Vec4 fog_color_density;
    Vec4 fog_height_params;
    // CSM 対応 (Phase 34b part 3): single mode は shadow_view_proj[0] のみ使用、
    // 残りは backward compat の inf split で常にスキップ。
    Mat4 shadow_view_proj[4];   // 各 cascade の light VP
    Vec4 shadow_params;         // x=bias, y=enabled, z=texel_size, w=filter_radius
    Vec4 cascade_splits;        // xyzw = cascade 0/1/2/3 の view-space z far (inf=未使用)
    Vec4 cascade_uv_scale;      // x=atlas X scale (single=1, N-cascade=1/N)、y=1、zw=pad
    Vec4 ssao_params;       // Phase 34j-2: x=enabled, y=intensity, zw=inv_viewport
    Vec4 ssgi_params;       // Phase 33c: x=enabled, y=intensity, zw=pad
    Vec4 lightmap_params;   // Phase 33f: x=enabled, y=intensity, zw=pad
    Vec4 ssr_params;        // Phase 34e-2fix: x=enabled, y=intensity, zw=pad
};

struct ObjectCBLayout {
    Mat4 model;
    Vec4 base_color;
    Vec4 pbr_params;        // x=metallic, y=roughness, z=ao, w=pad
    Vec4 ext_params;        // x=clearcoat, y=coat_roughness, z=anisotropy, w=flags
    Vec4 aniso_tangent;     // xyz=tangent world, w=pad
    Vec4 emissive;          // Phase 34l: xyz=emissive color * strength, w=pad
    Vec4 sheen_params;      // Phase 35-1a: xyz=sheen color, w=sheen weight
    Vec4 sheen_rough;       // Phase 35-1a: x=sheen roughness, yzw=pad
    Vec4 irid_params;       // Phase 35-1b: x=weight, y=thickness(nm), z=film IOR, w=pad
    Vec4 sss_params;        // Phase 35-2: xyz=subsurface color, w=weight
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> PbrShader::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Pbr.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Pbr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else _ps = Move(r.Value());

    BufferDesc fb{};
    fb.size = CBSize<FrameCBLayout>();
    fb.usage = EBufferUsage::Uniform;
    fb.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, fb); r.IsErr())
        return Err<void>(r.Error());
    else _frame_cb = Move(r.Value());

    BufferDesc ob{};
    ob.size = CBSize<ObjectCBLayout>();
    ob.usage = EBufferUsage::Uniform;
    ob.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, ob); r.IsErr())
        return Err<void>(r.Error());
    else _object_cb = Move(r.Value());

    // 1x1 白テクスチャ (albedo fallback)
    const u8 white[4] = {255, 255, 255, 255};
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white; td.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, td); r.IsErr())
        return Err<void>(r.Error());
    else _white = Move(r.Value());

    // Shadow map fallback: 1x1 RGBA8 全 255 (.r=1.0 = far、shadow_params.y=0 で
    // shader 側が早期 return するので実際には sample されない。SRB の有効 binding 要件用)。
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    TextureDesc sd{};
    sd.width = 1; sd.height = 1;
    sd.format = EFormat::R8G8B8A8_UNorm;
    sd.initial_data = far_depth; sd.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, sd); r.IsErr())
        return Err<void>(r.Error());
    else _shadow_fb = Move(r.Value());

    // Normal map fallback: 1x1 RGBA8 (128,128,255,0) = tangent (0,0,1) → 無変化
    const u8 flat_nrm[4] = { 128, 128, 255, 0 };
    TextureDesc nt{};
    nt.width = 1; nt.height = 1;
    nt.format = EFormat::R8G8B8A8_UNorm;
    nt.initial_data = flat_nrm; nt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, nt); r.IsErr())
        return Err<void>(r.Error());
    else _normal_map_fb = Move(r.Value());

    // SSAO fallback: 1x1 全 255 = visibility 1.0 (AO 無し)。ssao_params.x=0 でも
    // SRB に valid texture を bind する要件のため作成しておく。
    const u8 full_vis[4] = { 255, 255, 255, 255 };
    TextureDesc st{};
    st.width = 1; st.height = 1;
    st.format = EFormat::R8G8B8A8_UNorm;
    st.initial_data = full_vis; st.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, st); r.IsErr())
        return Err<void>(r.Error());
    else _ssao_fb = Move(r.Value());

    // SSGI fallback: 1x1 R11G11B10F、初期値 0 (indirect light なし)。SRB binding 用。
    // initial_data 経路は R11G11B10F でサポート無いので、blank RT として作る。
    // ssgi_params.x=0 で shader が早期 return するので内容は未定義のままで OK。
    TextureDesc gt{};
    gt.width = 1; gt.height = 1;
    gt.format = EFormat::R11G11B10_Float;
    gt.is_render_target = true;     // RT 兼用にすると SRV が自動で付く
    if (auto r = CreateRhiTexture(device, gt); r.IsErr())
        return Err<void>(r.Error());
    else _ssgi_fb = Move(r.Value());

    // Lightmap fallback: 1x1 RGBA8 全 0 (baked light なし)。
    // lightmap_params.x=0 で shader が早期 return するので unused。
    const u8 zero_rgba[4] = { 0, 0, 0, 0 };
    TextureDesc lt{};
    lt.width = 1; lt.height = 1;
    lt.format = EFormat::R8G8B8A8_UNorm;
    lt.initial_data = zero_rgba; lt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, lt); r.IsErr())
        return Err<void>(r.Error());
    else _lightmap_fb = Move(r.Value());

    // SSR fallback: 1x1 RGBA8 全 0 (.a=0 → hit mask 0 = 反射なし)。
    // ssr_params.x=0 で shader が早期 return するので unused だが SRB binding 用。
    TextureDesc rt{};
    rt.width = 1; rt.height = 1;
    rt.format = EFormat::R8G8B8A8_UNorm;
    rt.initial_data = zero_rgba; rt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, rt); r.IsErr())
        return Err<void>(r.Error());
    else _ssr_fb = Move(r.Value());

    // IBL fallback: 1x1x6 R11G11B10F cubemap + 1x1 RG16F 2D。
    // shader が ibl_enabled=0 で uniform branch して sample しない想定だが、
    // SRB に valid な texture を bind する必要があるので作っておく。内容は
    // undefined (driver は通常 0 化する)。
    TextureDesc ic{};
    ic.width = 1; ic.height = 1;
    ic.format = EFormat::R11G11B10_Float;
    ic.array_size = 6;
    ic.is_cubemap = true;
    if (auto r = CreateRhiTexture(device, ic); r.IsErr())
        return Err<void>(r.Error());
    else _ibl_irradiance_fb = Move(r.Value());
    if (auto r = CreateRhiTexture(device, ic); r.IsErr())
        return Err<void>(r.Error());
    else _ibl_prefilter_fb = Move(r.Value());

    TextureDesc bt{};
    bt.width = 1; bt.height = 1;
    bt.format = EFormat::R16G16_Float;
    if (auto r = CreateRhiTexture(device, bt); r.IsErr())
        return Err<void>(r.Error());
    else _ibl_brdf_fb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = ECullMode::Back;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 10;    // t0=albedo .. t8=lightmap, t9=ssr_color (Phase 34e-2fix)
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "albedo";
    pd.texture_names[1] = "irradiance";
    pd.texture_names[2] = "prefilter";
    pd.texture_names[3] = "brdf_lut";
    pd.texture_names[4] = "normal_map";
    pd.texture_names[5] = "shadow_map";
    pd.texture_names[6] = "ssao_map";
    pd.texture_names[7] = "ssgi_color";
    pd.texture_names[8] = "lightmap";
    pd.texture_names[9] = "ssr_color";
    pd.static_sampler_count = 10;
    pd.static_samplers[0].filter    = ESamplerFilter::Linear;
    pd.static_samplers[0].address_u = ESamplerAddress::Wrap;
    pd.static_samplers[0].address_v = ESamplerAddress::Wrap;
    pd.static_samplers[1].filter    = ESamplerFilter::Linear;
    pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
    pd.static_samplers[2].filter    = ESamplerFilter::Linear;
    pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[2].address_w = ESamplerAddress::Clamp;
    pd.static_samplers[3].filter    = ESamplerFilter::Linear;
    pd.static_samplers[3].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[3].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[3].address_w = ESamplerAddress::Clamp;     // 2D LUT で w 軸は未使用だが一貫性のため
    pd.static_samplers[4].filter    = ESamplerFilter::Linear;
    pd.static_samplers[4].address_u = ESamplerAddress::Wrap;       // normal map は wrap (tileable)
    pd.static_samplers[4].address_v = ESamplerAddress::Wrap;
    pd.static_samplers[5].filter    = ESamplerFilter::Linear;
    pd.static_samplers[5].address_u = ESamplerAddress::Clamp;       // shadow map は clamp
    pd.static_samplers[5].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[6].filter    = ESamplerFilter::Linear;       // SSAO は linear で smooth
    pd.static_samplers[6].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[6].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[7].filter    = ESamplerFilter::Linear;       // SSGI も linear で blur 効果
    pd.static_samplers[7].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[7].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[8].filter    = ESamplerFilter::Linear;       // lightmap は linear で texel boundary を smooth
    pd.static_samplers[8].address_u = ESamplerAddress::Clamp;       // 端を伸ばす (タイリングしない baked light)
    pd.static_samplers[8].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[9].filter    = ESamplerFilter::Linear;       // SSR も linear、画面外参照は clamp
    pd.static_samplers[9].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[9].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(MeshVertex);
    // MeshVertex の Vec3 は alignas(16) で 16 バイト境界。
    // → position@0, normal@16, uv@32 (Standard と一致)。
    // 12/24 にしてしまうと normal が position パディング + normal の途中を読んで
    // ジオメトリが破壊され、PBR が「黒っぽくべったり影」状態に見える。
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else _pipeline = Move(r.Value());

    return Ok();
}

void PbrShader::Shutdown() noexcept {
    _pipeline.Reset();
    _object_cb.Reset();
    _frame_cb.Reset();
    _shadow_fb.Reset();
    _shadow_depth = nullptr;
    _normal_map_fb.Reset();
    _normal_map = nullptr;
    _ssao_fb.Reset();
    _ssao_tex = nullptr;
    _ssgi_fb.Reset();
    _ssgi_tex = nullptr;
    _lightmap_fb.Reset();
    _lightmap_tex = nullptr;
    _ssr_fb.Reset();
    _ssr_tex = nullptr;
    _ibl_brdf_fb.Reset();
    _ibl_prefilter_fb.Reset();
    _ibl_irradiance_fb.Reset();
    _white.Reset();
    _ps.Reset();
    _vs.Reset();
    _ibl_irradiance = nullptr;
    _ibl_prefilter  = nullptr;
    _ibl_brdf       = nullptr;
    _ibl_mips       = 0;
    _ibl_enabled    = false;
}

void PbrShader::SetIbl(IRhiTexture* irradiance,
                       IRhiTexture* prefilter,
                       IRhiTexture* brdf_lut,
                       u32 prefilter_mips) noexcept {
    _ibl_irradiance = irradiance;
    _ibl_prefilter  = prefilter;
    _ibl_brdf       = brdf_lut;
    _ibl_mips       = prefilter_mips;
    _ibl_enabled    = (irradiance != nullptr) && (prefilter != nullptr)
                       && (brdf_lut != nullptr) && (prefilter_mips > 0);
    FlushFrameCB();
}

void PbrShader::SetFog(Vec3 color, f32 density, f32 height_falloff, f32 height_base) noexcept {
    _fog_color_density = Vec4{color.x, color.y, color.z, density};
    _fog_height_params = Vec4{height_falloff, height_base, 0, 0};
    FlushFrameCB();
}

void PbrShader::SetProbeGrid(const LightProbe* probes, u32 count) noexcept {
    if (count > 4) count = 4;
    _probe_count = count;
    for (u32 i = 0; i < count; ++i) {
        _probe_pos[i] = Vec4{probes[i].position.x, probes[i].position.y, probes[i].position.z, 0};
        for (u32 k = 0; k < 9; ++k) {
            _probe_sh9[i * 9 + k] = probes[i].sh9[k];
        }
    }
    for (u32 i = count; i < 4; ++i) {
        _probe_pos[i] = Vec4{0, 0, 0, 0};
        for (u32 k = 0; k < 9; ++k) _probe_sh9[i * 9 + k] = Vec4{0, 0, 0, 0};
    }
    FlushFrameCB();
}

void PbrShader::SetSh9(const Vec4* sh9_or_null) noexcept {
    if (sh9_or_null) {
        for (u32 i = 0; i < 9; ++i) _sh9[i] = sh9_or_null[i];
        _sh9_enabled = true;
    } else {
        for (u32 i = 0; i < 9; ++i) _sh9[i] = Vec4{0, 0, 0, 0};
        _sh9_enabled = false;
    }
    FlushFrameCB();
}

void PbrShader::BindIblTextures(IRhiCommandList& cmd) noexcept {
    if (_ibl_enabled) {
        cmd.SetTexture(1, *_ibl_irradiance);
        cmd.SetTexture(2, *_ibl_prefilter);
        cmd.SetTexture(3, *_ibl_brdf);
    } else {
        if (_ibl_irradiance_fb) cmd.SetTexture(1, *_ibl_irradiance_fb);
        if (_ibl_prefilter_fb)  cmd.SetTexture(2, *_ibl_prefilter_fb);
        if (_ibl_brdf_fb)       cmd.SetTexture(3, *_ibl_brdf_fb);
    }
    // Normal map (Phase 34g): 必ず slot 4 を bind
    if (_normal_map) {
        cmd.SetTexture(4, *_normal_map);
    } else if (_normal_map_fb) {
        cmd.SetTexture(4, *_normal_map_fb);
    }
    // Shadow map (Phase 34b): slot 5
    if (_shadow_depth) {
        cmd.SetTexture(5, *_shadow_depth);
    } else if (_shadow_fb) {
        cmd.SetTexture(5, *_shadow_fb);
    }
    // SSAO map (Phase 34j-2): slot 6
    if (_ssao_tex) {
        cmd.SetTexture(6, *_ssao_tex);
    } else if (_ssao_fb) {
        cmd.SetTexture(6, *_ssao_fb);
    }
    // SSGI color (Phase 33c): slot 7
    if (_ssgi_tex) {
        cmd.SetTexture(7, *_ssgi_tex);
    } else if (_ssgi_fb) {
        cmd.SetTexture(7, *_ssgi_fb);
    }
    // Lightmap (Phase 33f): slot 8
    if (_lightmap_tex) {
        cmd.SetTexture(8, *_lightmap_tex);
    } else if (_lightmap_fb) {
        cmd.SetTexture(8, *_lightmap_fb);
    }
    // SSR (Phase 34e-2fix): slot 9
    if (_ssr_tex) {
        cmd.SetTexture(9, *_ssr_tex);
    } else if (_ssr_fb) {
        cmd.SetTexture(9, *_ssr_fb);
    }
}

void PbrShader::SetNormalMap(IRhiTexture* tex) noexcept {
    _normal_map = tex;
}

void PbrShader::SetSsao(IRhiTexture* ssao_tex, f32 intensity,
                        u32 viewport_w, u32 viewport_h) noexcept {
    _ssao_tex       = ssao_tex;
    _ssao_intensity = intensity < 0 ? 0.0f : intensity;
    _ssao_inv_w     = (viewport_w > 0) ? (1.0f / static_cast<f32>(viewport_w)) : 0.0f;
    _ssao_inv_h     = (viewport_h > 0) ? (1.0f / static_cast<f32>(viewport_h)) : 0.0f;
    FlushFrameCB();
}

void PbrShader::SetSsgi(IRhiTexture* ssgi_tex, f32 intensity) noexcept {
    _ssgi_tex       = ssgi_tex;
    _ssgi_intensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

void PbrShader::SetSsr(IRhiTexture* ssr_tex, f32 intensity) noexcept {
    _ssr_tex       = ssr_tex;
    _ssr_intensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

void PbrShader::SetLightmap(IRhiTexture* lightmap_tex, f32 intensity) noexcept {
    _lightmap_tex       = lightmap_tex;
    _lightmap_intensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

void PbrShader::SetShadowMap(IRhiTexture* depth, const Mat4& light_vp,
                              f32 bias, f32 texel_size, f32 filter_radius) noexcept {
    _shadow_depth     = depth;
    // 後方互換: 全 cascade スロットに同じ VP を書き、splits を inf にして
    // HLSL の cascade 選択が常に cascade 0 を選ぶようにする。
    // uv_scale = {1, 1, 0, 0} で atlas 変換をパススルー (single texture モード)。
    for (u32 c = 0; c < kMaxShadowCascades; ++c) _shadow_view_proj[c] = light_vp;
    _shadow_params     = Vec4{bias, depth ? 1.0f : 0.0f, texel_size, filter_radius};
    _cascade_splits    = Vec4{1e30f, 1e30f, 1e30f, 1e30f};
    _cascade_uv_scale  = Vec4{1.0f, 1.0f, 0, 0};
    FlushFrameCB();
}

void PbrShader::SetShadowMapCascades(IRhiTexture* depth,
                                      const Mat4* light_vp,
                                      const f32*  cascade_splits,
                                      u32 cascade_count,
                                      f32 bias, f32 texel_size,
                                      f32 filter_radius) noexcept {
    _shadow_depth = depth;
    if (cascade_count == 0) cascade_count = 1;
    if (cascade_count > kMaxShadowCascades) cascade_count = kMaxShadowCascades;
    for (u32 c = 0; c < cascade_count; ++c) _shadow_view_proj[c] = light_vp[c];
    // 未使用 slot は cascade_count-1 を再利用 (cascade 選択結果が不正値になっても無害)
    for (u32 c = cascade_count; c < kMaxShadowCascades; ++c)
        _shadow_view_proj[c] = light_vp[cascade_count - 1];

    // cascade_splits xyzw に 4 cascade の z far を詰め、未使用は inf
    f32 splits[kMaxShadowCascades] = {1e30f, 1e30f, 1e30f, 1e30f};
    for (u32 c = 0; c < cascade_count; ++c) splits[c] = cascade_splits[c];
    _cascade_splits = Vec4{splits[0], splits[1], splits[2], splits[3]};

    _shadow_params    = Vec4{bias, depth ? 1.0f : 0.0f, texel_size, filter_radius};
    // atlas X scale = 1 / cascade_count (single mode は cascade_count=1 で 1)
    const f32 scale_x = 1.0f / static_cast<f32>(cascade_count);
    _cascade_uv_scale = Vec4{scale_x, 1.0f, 0, 0};
    FlushFrameCB();
}

void PbrShader::SetLights(const Mat4& vp, Vec3 eye,
                          const DirLight* lights, u32 count,
                          Vec3 ambient) noexcept {
    _vp = vp;
    _eye = eye;
    _ambient = ambient;
    if (count > kMaxDirLights) count = kMaxDirLights;
    _dir_count = count;
    for (u32 i = 0; i < count; ++i) _dir_lights[i] = lights[i];
    FlushFrameCB();
}

void PbrShader::SetPointLights(const PointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    _point_count = count;
    for (u32 i = 0; i < count; ++i) _point_lights[i] = lights[i];
    FlushFrameCB();
}

void PbrShader::SetAreaLights(const AreaLight* lights, u32 count) noexcept {
    if (count > kMaxAreaLights) count = kMaxAreaLights;
    _area_count = count;
    for (u32 i = 0; i < count; ++i) _area_lights[i] = lights[i];
    FlushFrameCB();
}

void PbrShader::FlushFrameCB() noexcept {
    if (!_frame_cb) return;
    FrameCBLayout cb{};
    cb.view_proj  = _vp;
    cb.camera_pos = Vec4{_eye.x, _eye.y, _eye.z, 1.0f};
    cb.ambient    = Vec4{_ambient.x, _ambient.y, _ambient.z, static_cast<f32>(_dir_count)};
    cb.point_count_pad = Vec4{static_cast<f32>(_point_count), static_cast<f32>(_area_count), 0, 0};
    for (u32 i = 0; i < _dir_count; ++i) {
        const Vec3& d = _dir_lights[i].direction;
        const Vec3& c = _dir_lights[i].color;
        cb.light_dir[i]   = Vec4{d.x, d.y, d.z, 0};
        cb.light_color[i] = Vec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < _point_count; ++i) {
        const Vec3& p = _point_lights[i].position;
        const Vec3& c = _point_lights[i].color;
        cb.point_pos_range[i] = Vec4{p.x, p.y, p.z, _point_lights[i].range};
        cb.point_color[i]     = Vec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < _area_count; ++i) {
        const AreaLight& a = _area_lights[i];
        cb.area_center[i] = Vec4{a.center.x, a.center.y, a.center.z, 0};
        cb.area_axis_x[i] = Vec4{a.axis_x.x, a.axis_x.y, a.axis_x.z, 0};
        cb.area_axis_y[i] = Vec4{a.axis_y.x, a.axis_y.y, a.axis_y.z, 0};
        cb.area_color[i]  = Vec4{a.color.x,  a.color.y,  a.color.z,  0};
    }
    cb.probe_params = Vec4{static_cast<f32>(_probe_count), 0, 0, 0};
    for (u32 i = 0; i < 4; ++i) cb.probe_pos[i] = _probe_pos[i];
    for (u32 i = 0; i < 4 * 9; ++i) cb.probe_sh9[i] = _probe_sh9[i];
    cb.fog_color_density = _fog_color_density;
    cb.fog_height_params = _fog_height_params;
    for (u32 c = 0; c < kMaxShadowCascades; ++c)
        cb.shadow_view_proj[c] = _shadow_view_proj[c];
    cb.shadow_params     = _shadow_params;
    cb.cascade_splits    = _cascade_splits;
    cb.cascade_uv_scale  = _cascade_uv_scale;
    cb.ibl_params = Vec4{
        _ibl_enabled ? 1.0f : 0.0f,
        static_cast<f32>(_ibl_mips),
        _sh9_enabled ? 1.0f : 0.0f,
        0
    };
    cb.ssao_params = Vec4{
        (_ssao_tex && _ssao_inv_w > 0 && _ssao_inv_h > 0) ? 1.0f : 0.0f,
        _ssao_intensity,
        _ssao_inv_w,
        _ssao_inv_h
    };
    cb.ssgi_params = Vec4{
        _ssgi_tex ? 1.0f : 0.0f,
        _ssgi_intensity,
        0, 0
    };
    cb.lightmap_params = Vec4{
        _lightmap_tex ? 1.0f : 0.0f,
        _lightmap_intensity,
        0, 0
    };
    cb.ssr_params = Vec4{
        _ssr_tex ? 1.0f : 0.0f,
        _ssr_intensity,
        0, 0
    };
    for (u32 i = 0; i < 9; ++i) cb.sh9[i] = _sh9[i];
    _frame_cb->Update(&cb, sizeof(cb));
}

void PbrShader::SetObject(const Mat4& model, Vec3 base_color,
                          f32 metallic, f32 roughness, f32 ao) noexcept {
    if (!_object_cb) return;
    ObjectCBLayout cb{};
    cb.model = model;
    cb.base_color = Vec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.pbr_params = Vec4{metallic, roughness, ao, 0};
    cb.ext_params    = _ext_params;
    cb.aniso_tangent = _aniso_tangent;
    cb.emissive      = _emissive;
    cb.sheen_params  = _sheen_params;
    cb.sheen_rough   = _sheen_rough;
    cb.irid_params   = _irid_params;
    cb.sss_params    = _sss_params;
    _object_cb->Update(&cb, sizeof(cb));
}

void PbrShader::SetExtParams(f32 clearcoat, f32 clearcoat_roughness,
                             f32 anisotropy, Vec3 tangent) noexcept {
    _ext_params    = Vec4{clearcoat, clearcoat_roughness, anisotropy, 0};
    _aniso_tangent = Vec4{tangent.x, tangent.y, tangent.z, 0};
    // 注: SetObject が CB を flush するので、SetExtParams 単独では反映されない。
    // SetObject 直後に呼んでも次の SetObject で 上書き されない (member に格納)。
    // 描画前に SetObject() が再度呼ばれて反映される設計。
}

void PbrShader::SetEmissive(Vec3 color, f32 strength) noexcept {
    const f32 s = strength < 0.0f ? 0.0f : strength;
    _emissive = Vec4{color.x * s, color.y * s, color.z * s, 0.0f};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

void PbrShader::SetSheen(Vec3 sheen_color, f32 weight, f32 roughness) noexcept {
    // weight は [0,1] の blend 係数、sheen_color は反射率なので各 ch を [0,1] に収める。
    // これでシェーダの energy 減衰係数 (1 - weight*maxC*0.5) が負へ振れない。
    auto sat01 = [](f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const f32 w = sat01(weight);
    const f32 r = roughness < 0.04f ? 0.04f : roughness;
    _sheen_params = Vec4{sat01(sheen_color.x), sat01(sheen_color.y), sat01(sheen_color.z), w};
    _sheen_rough  = Vec4{r, 0, 0, 0};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

void PbrShader::SetIridescence(f32 weight, f32 thickness_nm, f32 film_ior) noexcept {
    // weight は [0,1] の blend 係数。thickness は非負、film_ior は物理的に >= 1。
    const f32 w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    const f32 t = thickness_nm < 0.0f ? 0.0f : thickness_nm;
    const f32 i = film_ior < 1.0f ? 1.0f : film_ior;
    _irid_params = Vec4{w, t, i, 0};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

void PbrShader::SetSubsurface(Vec3 sss_color, f32 weight) noexcept {
    // weight は [0,1] の blend 係数、sss_color は内部散乱の色 (各 ch [0,1])。
    auto sat01 = [](f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    _sss_params = Vec4{sat01(sss_color.x), sat01(sss_color.y), sat01(sss_color.z),
                       sat01(weight)};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

void PbrShader::DrawMesh(IRhiCommandList& cmd, const GpuMesh& mesh, const Mat4& model,
                        Vec3 base_color, f32 metallic, f32 roughness, f32 ao,
                        IRhiTexture* albedo) noexcept {
    if (!_pipeline || !_frame_cb || !_object_cb) return;
    SetObject(model, base_color, metallic, roughness, ao);
    cmd.SetPipeline(*_pipeline);
    cmd.SetConstantBuffer(0, *_frame_cb);
    cmd.SetConstantBuffer(1, *_object_cb);
    cmd.SetTexture(0, *(albedo ? albedo : _white.Get()));
    BindIblTextures(cmd);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
}

} // namespace acs
