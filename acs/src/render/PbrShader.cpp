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

    // Shadow map (Phase 34b、第 0 番目の dir light のみ対応)
    float4x4 shadow_view_proj;
    float4   shadow_params;           // x=bias, y=enabled (0/1), z=texel_size, w=filter_radius
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   base_color;     // xyz=color, w=alpha
    float4   pbr_params;     // x=metallic, y=roughness, z=ao, w=pad
    float4   ext_params;     // x=clearcoat (0..1)、y=clearcoat_roughness (0..1)
                             // z=anisotropy (-1..1)、w=enable_flags (bit0=clearcoat, bit1=aniso)
    float4   aniso_tangent;  // xyz=anisotropic tangent direction (world)、w=pad
};

Texture2D    albedo           : register(t0);
TextureCube  irradiance        : register(t1);
TextureCube  prefilter         : register(t2);
Texture2D    brdf_lut          : register(t3);
Texture2D    normal_map        : register(t4);
Texture2D    shadow_map        : register(t5);
SamplerState albedo_sampler     : register(s0);
SamplerState irradiance_sampler : register(s1);
SamplerState prefilter_sampler  : register(s2);
SamplerState brdf_lut_sampler   : register(s3);
SamplerState normal_map_sampler : register(s4);
SamplerState shadow_map_sampler : register(s5);

struct VSIn  { float3 pos : POSITION; float3 nrm : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut {
    float4 pos     : SV_POSITION;
    float3 world_p : POSITION;
    float3 world_n : NORMAL;
    float2 uv      : TEXCOORD0;
};

VSOut VSMain(VSIn v) {
    VSOut o;
    float4 wp = mul(float4(v.pos, 1.0), model);
    o.world_p = wp.xyz;
    o.pos     = mul(wp, view_proj);
    o.world_n = mul(float4(v.nrm, 0.0), model).xyz;
    o.uv      = v.uv;
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

// シャドウマップ係数 (1=完全照明、0=完全遮蔽)。4-tap PCF + bias。
// StandardShader.cpp の ComputeShadow と同等。single-return で X4000 回避。
float ComputeShadow(float3 world_p) {
    float result = 1.0;
    if (shadow_params.y >= 0.5) {
        float4 lp = mul(float4(world_p, 1.0), shadow_view_proj);
        float3 ndc = lp.xyz / lp.w;
        if (ndc.x >= -1.0 && ndc.x <= 1.0 &&
            ndc.y >= -1.0 && ndc.y <= 1.0 &&
            ndc.z >=  0.0 && ndc.z <= 1.0) {
            float2 uv = float2(ndc.x * 0.5 + 0.5, -ndc.y * 0.5 + 0.5);
            float my_d = ndc.z;
            float bias = shadow_params.x;
            float ts = shadow_params.z;
            // 4-tap PCF (bilinear sample で滑らかなエッジ)
            float lit = 0;
            lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2(-ts, -ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
            lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2( ts, -ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
            lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2(-ts,  ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
            lit += (shadow_map.SampleLevel(shadow_map_sampler, uv + float2( ts,  ts), 0).r + bias >= my_d) ? 1.0 : 0.0;
            result = lit * 0.25;
        }
    }
    return result;
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
                        float metallic, float roughness, float ao)
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

    // prefilter は mip = roughness * (mip_count - 1)。Sample (with HW mip
    // selection) ではなく SampleLevel で明示することで filtering を確実にする。
    float mip_lvl = roughness * max(ibl_params.y - 1.0, 0.0);
    float3 prefilt = prefilter.SampleLevel(prefilter_sampler, R, mip_lvl).rgb;
    float2 lut_xy = brdf_lut.SampleLevel(brdf_lut_sampler, float2(NoV, roughness), 0).rg;
    float3 specular_ibl = prefilt * (F0 * lut_xy.x + lut_xy.y);

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

// Cook-Torrance BRDF * NdotL (light vector L is from surface to light source).
// anisotropy ≠ 0 のとき D / G を anisotropic 版に切替える。tangent (T) はオブジェクト
// から与えられる主軸方向、B (bitangent) は cross(N, T) で生成。
float3 BrdfCookTorrance(float3 N, float3 V, float3 L,
                       float3 base, float metallic, float roughness,
                       float anisotropy, float3 T_world)
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

    // 環境光: ibl_params.x が 1 なら IBL ambient、0 なら flat ambient。
    // uniform branching なので片方の TextureCube サンプルは PSO の dead code
    // として削除される (FXC の判断による)。
    float3 col;
    if (ibl_params.x >= 0.5) {
        float3 base_ibl = ComputeIblAmbient(N, V, v.world_p, albedo_rgb, metallic, roughness, ao);
        IblCoatTerm cc_ibl = ComputeIblClearcoat(N, V, clearcoat, coat_roughness);
        col = base_ibl * cc_ibl.attenuation + cc_ibl.spec * ao;
    } else {
        col = ambient.xyz * albedo_rgb * ao;
    }

    // 有向光源 (i==0 のみ shadow_map で遮蔽)
    float shadow = ComputeShadow(v.world_p);
    int dir_count = (int)ambient.w;
    [unroll]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        float3 base_brdf = BrdfCookTorrance(N, V, L, albedo_rgb, metallic, roughness,
                                            anisotropy, aniso_T_world);
        ClearcoatTerm cc = EvalClearcoat(N, V, L, clearcoat, coat_roughness);
        float k = (i == 0) ? shadow : 1.0;
        col += light_color[i].xyz * (base_brdf * cc.attenuation + cc.spec_times_nol) * k;
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
                    float3 base_brdf = BrdfCookTorrance(N, V, L, albedo_rgb, metallic, roughness,
                                                        anisotropy, aniso_T_world);
                    ClearcoatTerm cc = EvalClearcoat(N, V, L, clearcoat, coat_roughness);
                    area_sum += (base_brdf * cc.attenuation + cc.spec_times_nol) * att;
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
        float3 base_brdf = BrdfCookTorrance(N, V, L, albedo_rgb, metallic, roughness,
                                            anisotropy, aniso_T_world);
        ClearcoatTerm cc = EvalClearcoat(N, V, L, clearcoat, coat_roughness);
        col += point_color[j].xyz * (base_brdf * cc.attenuation + cc.spec_times_nol) * att;
    }

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
    Mat4 shadow_view_proj;
    Vec4 shadow_params;
};

struct ObjectCBLayout {
    Mat4 model;
    Vec4 base_color;
    Vec4 pbr_params;        // x=metallic, y=roughness, z=ao, w=pad
    Vec4 ext_params;        // x=clearcoat, y=coat_roughness, z=anisotropy, w=flags
    Vec4 aniso_tangent;     // xyz=tangent world, w=pad
};

template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

Result<void> PbrShader::Init(IRhiDevice& device, Format rt_format, Format depth_format) noexcept {
    ShaderDesc vs_d{};
    vs_d.stage = ShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Pbr.VS";
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else _vs = Move(r.Value());

    ShaderDesc ps_d{};
    ps_d.stage = ShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Pbr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else _ps = Move(r.Value());

    BufferDesc fb{};
    fb.size = CBSize<FrameCBLayout>();
    fb.usage = BufferUsage::Uniform;
    fb.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, fb); r.IsErr())
        return Err<void>(r.Error());
    else _frame_cb = Move(r.Value());

    BufferDesc ob{};
    ob.size = CBSize<ObjectCBLayout>();
    ob.usage = BufferUsage::Uniform;
    ob.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, ob); r.IsErr())
        return Err<void>(r.Error());
    else _object_cb = Move(r.Value());

    // 1x1 白テクスチャ (albedo fallback)
    const u8 white[4] = {255, 255, 255, 255};
    TextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = Format::R8G8B8A8_UNorm;
    td.initial_data = white; td.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, td); r.IsErr())
        return Err<void>(r.Error());
    else _white = Move(r.Value());

    // Shadow map fallback: 1x1 RGBA8 全 255 (.r=1.0 = far、shadow_params.y=0 で
    // shader 側が早期 return するので実際には sample されない。SRB の有効 binding 要件用)。
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    TextureDesc sd{};
    sd.width = 1; sd.height = 1;
    sd.format = Format::R8G8B8A8_UNorm;
    sd.initial_data = far_depth; sd.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, sd); r.IsErr())
        return Err<void>(r.Error());
    else _shadow_fb = Move(r.Value());

    // Normal map fallback: 1x1 RGBA8 (128,128,255,0) = tangent (0,0,1) → 無変化
    const u8 flat_nrm[4] = { 128, 128, 255, 0 };
    TextureDesc nt{};
    nt.width = 1; nt.height = 1;
    nt.format = Format::R8G8B8A8_UNorm;
    nt.initial_data = flat_nrm; nt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, nt); r.IsErr())
        return Err<void>(r.Error());
    else _normal_map_fb = Move(r.Value());

    // IBL fallback: 1x1x6 R11G11B10F cubemap + 1x1 RG16F 2D。
    // shader が ibl_enabled=0 で uniform branch して sample しない想定だが、
    // SRB に valid な texture を bind する必要があるので作っておく。内容は
    // undefined (driver は通常 0 化する)。
    TextureDesc ic{};
    ic.width = 1; ic.height = 1;
    ic.format = Format::R11G11B10_Float;
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
    bt.format = Format::R16G16_Float;
    if (auto r = CreateRhiTexture(device, bt); r.IsErr())
        return Err<void>(r.Error());
    else _ibl_brdf_fb = Move(r.Value());

    PipelineDesc pd{};
    pd.vs = _vs.Get();
    pd.ps = _ps.Get();
    pd.topology      = PrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = CullMode::Back;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    pd.texture_slots = 6;     // t0=albedo, t1=irradiance, t2=prefilter, t3=brdf_lut, t4=normal_map, t5=shadow_map
    pd.cbuffer_names[0] = "Frame";
    pd.cbuffer_names[1] = "Object";
    pd.texture_names[0] = "albedo";
    pd.texture_names[1] = "irradiance";
    pd.texture_names[2] = "prefilter";
    pd.texture_names[3] = "brdf_lut";
    pd.texture_names[4] = "normal_map";
    pd.texture_names[5] = "shadow_map";
    pd.static_sampler_count = 6;
    pd.static_samplers[0].filter    = SamplerFilter::Linear;
    pd.static_samplers[0].address_u = SamplerAddress::Wrap;
    pd.static_samplers[0].address_v = SamplerAddress::Wrap;
    pd.static_samplers[1].filter    = SamplerFilter::Linear;
    pd.static_samplers[1].address_u = SamplerAddress::Clamp;
    pd.static_samplers[1].address_v = SamplerAddress::Clamp;
    pd.static_samplers[1].address_w = SamplerAddress::Clamp;
    pd.static_samplers[2].filter    = SamplerFilter::Linear;
    pd.static_samplers[2].address_u = SamplerAddress::Clamp;
    pd.static_samplers[2].address_v = SamplerAddress::Clamp;
    pd.static_samplers[2].address_w = SamplerAddress::Clamp;
    pd.static_samplers[3].filter    = SamplerFilter::Linear;
    pd.static_samplers[3].address_u = SamplerAddress::Clamp;
    pd.static_samplers[3].address_v = SamplerAddress::Clamp;
    pd.static_samplers[3].address_w = SamplerAddress::Clamp;     // 2D LUT で w 軸は未使用だが一貫性のため
    pd.static_samplers[4].filter    = SamplerFilter::Linear;
    pd.static_samplers[4].address_u = SamplerAddress::Wrap;       // normal map は wrap (tileable)
    pd.static_samplers[4].address_v = SamplerAddress::Wrap;
    pd.static_samplers[5].filter    = SamplerFilter::Linear;
    pd.static_samplers[5].address_u = SamplerAddress::Clamp;       // shadow map は clamp
    pd.static_samplers[5].address_v = SamplerAddress::Clamp;
    pd.vertex_stride = sizeof(MeshVertex);
    // MeshVertex の Vec3 は alignas(16) で 16 バイト境界。
    // → position@0, normal@16, uv@32 (Standard と一致)。
    // 12/24 にしてしまうと normal が position パディング + normal の途中を読んで
    // ジオメトリが破壊され、PBR が「黒っぽくべったり影」状態に見える。
    pd.layout[0] = { "POSITION", 0, Format::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, Format::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, Format::R32G32_Float,    32 };
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
}

void PbrShader::SetNormalMap(IRhiTexture* tex) noexcept {
    _normal_map = tex;
}

void PbrShader::SetShadowMap(IRhiTexture* depth, const Mat4& light_vp,
                              f32 bias, f32 texel_size) noexcept {
    _shadow_depth     = depth;
    _shadow_view_proj = light_vp;
    _shadow_params    = Vec4{bias, depth ? 1.0f : 0.0f, texel_size, 0};
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
    cb.shadow_view_proj  = _shadow_view_proj;
    cb.shadow_params     = _shadow_params;
    cb.ibl_params = Vec4{
        _ibl_enabled ? 1.0f : 0.0f,
        static_cast<f32>(_ibl_mips),
        _sh9_enabled ? 1.0f : 0.0f,
        0
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
