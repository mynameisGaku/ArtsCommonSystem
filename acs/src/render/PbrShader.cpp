// SPDX-License-Identifier: Apache-2.0
// CPbrShader 実装 — Cook-Torrance BRDF (GGX + Smith + Schlick)
#include "render/PbrShader.h"
#include "render/NormalMatrix.h"
#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif
#include "asset/MeshAsset.h"   // MeshVertex
#include "foundation/Move.h"   // Move
#include "foundation/Log.h"

#include <cstring>
#include <cstddef>
#include <cmath>
#include <chrono>

namespace acs {

namespace {

#define ACS_MAX_DIR_LIGHTS   4
#define ACS_MAX_POINT_LIGHTS 4

/**
 * PBR Cook-Torrance シェーダの HLSL ソース (VSMain / PSMain)。
 *
 * @details CStandardShader と同じ vertex 入力 (pos / nrm / uv) と vs 出力
 * (world_p / world_n / uv) を持ち、IBL・shadow・SSAO/SSGI/SSR・拡張 lobe を含む。
 */
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
    float4   ibl_params;                              // x=ibl_enabled, y=prefilter_mip_count, z=use_sh9, w=environment_light_multiplier
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
    float4   fog_height_params;       // x=高度減衰, y=基準高度, z=HG 異方性, w=太陽散乱

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

    // Aerial perspective (WickedEngine 流 camera-volume LUT)。物理大気の froxel volume を
    // screen uv + 深度→スライスで trilinear サンプルし col = col*(1-ap.a)+ap.rgb で適用。
    //   x = enabled (0/1)、y = max_dist (scene 単位、深度→スライス逆変換用)、zw = pad
    float4   ap_params;

    // 立体物用の雲影。受光点を太陽方向に沿って基準面へ戻して透過率地図を引く。
    // 地図: xy=基準面上の左下XZ, z=1/範囲, w=有効
    // 投影: xyz=受光点から太陽への方向, w=基準面Y
    // 雲層: x=雲底高度, y=正規化画素幅, z=太陽Y下限, w=惑星半径
    float4   cloud_shadow_map_params;
    float4   cloud_shadow_projection;
    float4   cloud_shadow_layer;
    float4   cloud_shadow_world_origin;
};

cbuffer Object : register(b1) {
    float4x4 model;
    float4   normal_row0;
    float4   normal_row1;
    float4   normal_row2;
    float4   base_color;     // xyz=color, w=alpha
    float4   pbr_params;     // x=metallic, y=roughness, z=ao, w=normal-map strength
    float4   ext_params;     // x=clearcoat (0..1)、y=clearcoat_roughness (0..1)
                             // z=anisotropy (-1..1)、w=enable_flags (bit0=clearcoat, bit1=aniso)
    float4   aniso_tangent;  // xyz=anisotropic tangent direction (world)、w=pad
    float4   emissive;       // Phase 34l: xyz=自己発光色 * strength、w=pad
    float4   sheen_params;   // Phase 35-1a: xyz=sheen color, w=sheen weight (0=OFF)
    float4   sheen_rough;    // Phase 35-1a: x=sheen roughness, yzw=pad
    float4   irid_params;    // Phase 35-1b: x=weight, y=thickness(nm), z=film IOR, w=pad
    float4   sss_params;     // Phase 35-2: xyz=subsurface color, w=weight (0=OFF)
    float4   substrate_f0;   // xyz=direct slab F0
    float4   substrate_f90;  // xyz=direct slab F90
    float4   substrate_diffuse_coverage; // xyz=DiffuseAlbedo,w=coverage
    float4   substrate_secondary; // x=roughness2,y=weight,z=phase anisotropy,w=enabled
    float4   substrate_mfp_thickness; // xyz=MFP cm,w=thickness cm
    float4   substrate_transmittance; // xyz=normal-incidence transmittance,w=coverage
    float4   substrate_normal; // xyz=tangent-space normal,w=strength
    float4   substrate_coat_f0; // xyz=top coat F0
    // Typed Substrate expression VM. Instructions are 48-byte uint4x3 records.
    uint4    substrate_expr_instructions[64 * 3];
    // metadata, asuint(coefficient), asuint(authored literal), reserved.
    uint4    substrate_expr_bindings[39];
    uint4    substrate_expr_parameter_meta[32];
    float4   substrate_expr_parameter_values[32];
    // x=instruction count,y=binding count,z=parameter count,w=texture mask.
    uint4    substrate_expr_meta;
    float4   substrate_expr_context; // x=time seconds
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
Texture3D    ap_volume         : register(t10);   // 空気遠近法 + 霧のカメラボリューム LUT
Texture2D    expression_texture0 : register(t11);
Texture2D    expression_texture1 : register(t12);
Texture2D    expression_texture2 : register(t13);
Texture2D    expression_texture3 : register(t14);
Texture2D    cloud_shadow_transmittance : register(t15);
SamplerState albedo_sampler     : register(s0);
SamplerState irradiance_sampler : register(s1);
SamplerState prefilter_sampler  : register(s2);
SamplerState brdf_lut_sampler   : register(s3);
SamplerState normal_map_sampler : register(s4);
SamplerComparisonState shadow_map_sampler : register(s5);   // HW 比較 PCF (SampleCmpLevelZero、WickedEngine sampler_cmp_depth 相当)
SamplerState ssao_map_sampler   : register(s6);
SamplerState ssgi_color_sampler : register(s7);
SamplerState lightmap_sampler   : register(s8);
SamplerState ssr_color_sampler  : register(s9);
SamplerState ap_volume_sampler  : register(s10);   // linear clamp (3D trilinear)
SamplerState expression_texture0_sampler : register(s11);
SamplerState expression_texture1_sampler : register(s12);
SamplerState expression_texture2_sampler : register(s13);
SamplerState expression_texture3_sampler : register(s14);
SamplerState cloud_shadow_transmittance_sampler : register(s15);

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
    o.world_n = float3(
        v.nrm.x * normal_row0.x + v.nrm.y * normal_row1.x + v.nrm.z * normal_row2.x,
        v.nrm.x * normal_row0.y + v.nrm.y * normal_row1.y + v.nrm.z * normal_row2.y,
        v.nrm.x * normal_row0.z + v.nrm.y * normal_row1.z + v.nrm.z * normal_row2.z);
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
// (clearcoat 等の別ローブ用に残置。主鏡面は V_SmithGGXCorrelated を使う)
float G_Smith(float NdotV, float NdotL, float roughness) {
    float k = (roughness + 1.0);
    k = k * k * 0.125;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    return Gv * Gl;
}

// Smith height-correlated visibility (Heitz 2014、WickedEngine brdf.hlsli と同形)。
// 戻り値は «可視性» V = G / (4 NoV NoL) で、分母 1/(4 NoV NoL) を内包する。
// a2 = alpha^2 (= roughness^4)。分離型 Schlick-GGX より grazing/高roughness の遮蔽が正確。
float V_SmithGGXCorrelated(float NdotV, float NdotL, float a2) {
    float gv = NdotL * sqrt((NdotV - a2 * NdotV) * NdotV + a2);
    float gl = NdotV * sqrt((NdotL - a2 * NdotL) * NdotL + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

// Schlick Fresnel approximation
float3 F_Schlick(float VdotH, float3 F0) {
    float f = pow(saturate(1.0 - VdotH), 5.0);
    return F0 + (1.0 - F0) * f;
}

float3 F_Schlick90(float VdotH, float3 F0, float3 F90) {
    float f = pow(saturate(1.0 - VdotH), 5.0);
    return F0 + (F90 - F0) * f;
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
    float shadow_value = 1.0;
    float4 lp = mul(float4(world_p, 1.0), shadow_view_proj[cascade]);
    float3 ndc = float3(0.0, 0.0, 0.0);
    bool projection_valid = abs(lp.w) > 1.0e-5;
    if (projection_valid) {
        ndc = lp.xyz / lp.w;
    }
    bool inside_cascade =
        projection_valid &&
        ndc.x >= -1.0 && ndc.x <= 1.0 &&
        ndc.y >= -1.0 && ndc.y <= 1.0 &&
        ndc.z >=  0.0 && ndc.z <= 1.0;
    if (inside_cascade) {

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
    const float2 kHalfTexelInset = float2(ts * 0.5 * scale_x, ts * 0.5);
    const float2 atlasMin = float2(ofs_x, 0.0) + kHalfTexelInset;
    const float2 atlasMax = float2(ofs_x + scale_x, 1.0) - kHalfTexelInset;
    // shadow_map_sampler は比較サンプラ (SampleCmpLevelZero 用) なので生 depth は読めない。
    // blocker search の raw depth は .Load() (サンプラ不要の point fetch) で読む。
    uint smw, smh; shadow_map.GetDimensions(smw, smh);
    float2 smSize = float2(smw, smh);

    // ---- Blocker search (PCSS Fernando 2005、16 tap、半径 = 4 * texel) ----
    float blocker_sum = 0;
    int   blocker_cnt = 0;
    const int kBlockerN = 4;
    float search_r = ts * 4.0;
    [unroll]
    for (int by = -kBlockerN/2; by < kBlockerN/2; ++by) {
        [unroll]
        for (int bx = -kBlockerN/2; bx < kBlockerN/2; ++bx) {
            // +0.5 で中心化: {-2,-1,0,1} だと半 tap 偏心して penumbra が方向に偏る → {-1.5,-0.5,0.5,1.5}
            float2 off       = (float2(bx, by) + 0.5) * (search_r * 0.5);
            float2 atlas_uv  = (base_uv + off) * float2(scale_x, 1.0) + float2(ofs_x, 0);
            atlas_uv = clamp(atlas_uv, atlasMin, atlasMax);
            float sd = shadow_map.Load(int3(int2(atlas_uv * smSize), 0)).r;   // 生 depth は Load (比較サンプラは SampleCmp 専用)
            if (sd + bias < my_d) {
                blocker_sum += sd;
                blocker_cnt += 1;
            }
        }
    }

    if (blocker_cnt == kBlockerN * kBlockerN) {
        shadow_value = 0.0;
    } else if (blocker_cnt > 0) {
    float blocker_avg = blocker_sum / float(blocker_cnt);
    // penumbra ≒ (receiver - blocker) * light_size / blocker (UV 上の太陽径は固定)
    float penumbra = max((my_d - blocker_avg) / max(blocker_avg, 1e-3), 0.0);
    float filter_r = max(penumbra * 0.01 * kFilt, ts);

    // ---- PCF: 回転 Vogel ディスク 16 tap で penumbra-sized blur ----
    // 軸整列 4x4 グリッドだとブロック状/段差の penumbra になる。golden-angle の Vogel ディスク
    // (円板を均一サンプル) を «画素ごとに回転» (base_uv ハッシュ) して滑らかなノイズ状の penumbra に。
    // 残る粒は TAA が時間方向に均す。
    float ang = 6.2831853 * frac(52.9829189 * frac(dot(base_uv, float2(0.06711056, 0.00583715))));
    float ca = cos(ang), sa = sin(ang);
    float lit = 0;
    const int kPcfN = 16;
    [unroll]
    for (int s = 0; s < kPcfN; ++s) {
        float rr = sqrt((float(s) + 0.5) / float(kPcfN));        // sqrt → 円板面積均一
        float ta = float(s) * 2.39996323;                        // golden angle
        float2 vd = float2(cos(ta), sin(ta)) * rr;
        float2 jitter = float2(vd.x * ca - vd.y * sa, vd.x * sa + vd.y * ca);   // per-pixel 回転
        float2 off      = jitter * filter_r * 2.0;
        float2 atlas_uv = (base_uv + off) * float2(scale_x, 1.0) + float2(ofs_x, 0);
        atlas_uv = clamp(atlas_uv, atlasMin, atlasMax);
        // HW 比較 PCF: タップごとに 2x2 バイリニア深度比較で 0..1 を返す (lit ⇔ my_d-bias ≤ stored)。
        // 手動の二値比較 (point sample) より penumbra が滑らか。WickedEngine の SampleCmpLevelZero 相当。
        lit += shadow_map.SampleCmpLevelZero(shadow_map_sampler, atlas_uv, my_d - bias);
    }
    shadow_value = lit / float(kPcfN);
    }
    }
    return shadow_value;
}

// CSM 全体 (cascade 選択 + boundary blending)。
// Phase 36-2 cascade blending: 各 cascade の末尾 kBlendRatio (=15%) は
// 次 cascade と線形ブレンドして「カスケード seam」(解像度切替の影段差) を除去。
// blend 領域では PCSS を 2 回呼ぶので約 2x コスト、それ以外は単発。
float ComputeShadow(float3 world_p, float view_z) {
    float shadow_value = 1.0;
    if (shadow_params.y >= 0.5) {
    int cascade_count = clamp((int)(cascade_uv_scale.z + 0.5), 1, 4);
    float last_split = (cascade_count == 1) ? cascade_splits.x :
                       (cascade_count == 2) ? cascade_splits.y :
                       (cascade_count == 3) ? cascade_splits.z :
                                              cascade_splits.w;
    if (view_z <= last_split) {

    // ---- Cascade selection by view-space z (Phase 34b part 3 CSM) ----
    // single mode では cascade_splits.xyzw = inf なので常に cascade 0、
    // ブレンドも next_split=inf で発動しない (= 後方互換)。
    int cascade = 0;
    if (view_z > cascade_splits.x) cascade = 1;
    if (view_z > cascade_splits.y) cascade = 2;
    if (view_z > cascade_splits.z) cascade = 3;
    cascade = min(cascade, cascade_count - 1);

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

    if (cascade + 1 < cascade_count && next_split < 1e29) {
        const float kBlendRatio = 0.15;
        float cascade_range = max(next_split - prev_split, 1e-3);
        float blend_width   = cascade_range * kBlendRatio;
        float blend_start   = next_split - blend_width;
        if (view_z > blend_start) {
            float t = saturate((view_z - blend_start) / blend_width);
            float shadow_next = SamplePcssCascade(cascade + 1, world_p);
            shadow_c = lerp(shadow_c, shadow_next, t);
        }
    } else {
        const float kFarFadeRatio = 0.10;
        float cascade_range = max(last_split - prev_split, 1e-3);
        float fade_width = cascade_range * kFarFadeRatio;
        float fade_start = last_split - fade_width;
        shadow_c = lerp(shadow_c, 1.0, saturate((view_z - fade_start) / fade_width));
    }

    shadow_value = shadow_c;
    }
    }
    return shadow_value;
}

// 地図の外、雲内、太陽が地平線付近の受光点は遮光なしへ戻す。境界二画素は
// 透過率1へ滑らかにつなぎ、カメラ移動で地図の端が線として見えることを防ぐ。
float ComputeCloudShadowTransmittance(float3 world_p) {
    // どの分岐でも未初期化値を返さないよう、遮光なしを先に確定する。
    float transmittance = 1.0;
    bool mappingValid = cloud_shadow_map_params.w >= 0.5 && cloud_shadow_projection.y > cloud_shadow_layer.z && cloud_shadow_layer.w >= 100.0;
    if (mappingValid) {
        float3 local = world_p - cloud_shadow_world_origin.xyz;
        float radialY = max(cloud_shadow_layer.w + local.y, 1.0);
        float radialXzSquared = dot(local.xz, local.xz);
        float q = radialXzSquared / radialY;
        float receiverAltitude = local.y + q * (0.5 - q / (8.0 * radialY));
        if (receiverAltitude < cloud_shadow_layer.x) {
            float distanceAlongSun = (world_p.y - cloud_shadow_projection.w) / cloud_shadow_projection.y;
            float2 referenceXz = world_p.xz - cloud_shadow_projection.xz * distanceAlongSun;
            float2 uv = (referenceXz - cloud_shadow_map_params.xy) * cloud_shadow_map_params.z;
            if (all(uv > 0.0) && all(uv < 1.0)) {
                float edgeDistance = min(min(uv.x, uv.y), min(1.0 - uv.x, 1.0 - uv.y));
                float edgeWeight = smoothstep(0.0, max(2.0 * cloud_shadow_layer.y, 1e-6), edgeDistance);
                float sampled = cloud_shadow_transmittance.SampleLevel(cloud_shadow_transmittance_sampler, uv, 0).r;
                bool finiteAndBounded = sampled == sampled && sampled >= 0.0 && sampled <= 1.0;
                if (finiteAndBounded) transmittance = lerp(1.0, sampled, edgeWeight);
            }
        }
    }
    return transmittance;
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
                            float clearcoat, float coat_roughness, float3 coat_f0) {
    ClearcoatTerm o;
    o.spec_times_nol = float3(0, 0, 0);
    o.attenuation    = 1.0;
    if (clearcoat > 0.0) {
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    if (NdotL > 0.0) {
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float a  = max(coat_roughness * coat_roughness, 1e-3);
    float a2 = a * a;
    float distribution_base = NdotH * NdotH * (a2 - 1.0) + 1.0;
    float Dc = a2 / max(
        PI * distribution_base * distribution_base, 1e-7);
    // 簡易 Smith G (isotropic、coat 層は metallic 不可なので一般 GGX で十分)
    float k = (coat_roughness + 1.0); k = k * k * 0.125;
    float Gv = NdotV / max(NdotV * (1.0 - k) + k, 1e-7);
    float Gl = NdotL / max(NdotL * (1.0 - k) + k, 1e-7);
    float Gc = Gv * Gl;
    float3 Fc = F_Schlick90(VdotH, coat_f0, float3(1,1,1));

    float3 spec = (Dc * Gc * Fc) / max(4.0 * NdotV * NdotL, 1e-7);
    o.spec_times_nol = spec * NdotL * clearcoat;
    // base 層への透過: 1 - Fc * clearcoat (energy conservation)
    o.attenuation = 1.0 - max(Fc.r, max(Fc.g, Fc.b)) * clearcoat;
    }
    }
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
// SH 9 «radiance» reconstruction (コサイン畳み込み無しの素の SH 評価)。環境 prefilter cubemap が
// 無い backend (raw-DX12) で specular IBL の反射元 radiance を SH9 から近似するのに使う。
// 低周波なので滑らかな空グラデの反射に向く (鋭い反射/太陽 disc は出ないが gradient は再現)。
float3 Sh9Radiance(float3 d, float4 L[9]) {
    float x = d.x, y = d.y, z = d.z;
    float3 r = L[0].rgb * 0.282095
             + L[1].rgb * (0.488603 * y)
             + L[2].rgb * (0.488603 * z)
             + L[3].rgb * (0.488603 * x)
             + L[4].rgb * (1.092548 * x * y)
             + L[5].rgb * (1.092548 * y * z)
             + L[6].rgb * (0.315392 * (3.0 * z * z - 1.0))
             + L[7].rgb * (1.092548 * x * z)
             + L[8].rgb * (0.546274 * (x * x - y * y));
    return max(r, float3(0, 0, 0));    // clamp 負値 (SH ringing 対策)
}
)" R"(
float3 ProbeGridIrradiance(float3 world_p, float3 N) {
    int n = (int)probe_params.x;
    float3 irradiance_value = float3(0, 0, 0);
    if (n > 0) {
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
    irradiance_value =
        max(Sh9Irradiance(N, blended), float3(0, 0, 0));
    }
    return irradiance_value;
}

// IBL ambient (split-sum approximation):
//   diffuse_ibl  = (1 - F) * (1 - metallic) * base * irradiance.Sample(N)
//   specular_ibl = prefilter.SampleLevel(R, roughness * (mips-1)) * (F0 * lut.r + lut.g)
float3 ComputeIblAmbient(float3 N, float3 V, float3 world_p, float3 base,
                        float metallic, float roughness, float ao,
                        float3 ssr_radiance, float ssr_weight,
                        float3 sheenColor, float sheenWeight,
                        float3 iridFresnel, float iridWeight,
                        float3 directF0, float3 directF90,
                        float secondRoughness, float secondWeight,
                        out float3 diffuseLighting)
{
    float NoV = saturate(dot(N, V));
    float3 R  = reflect(-V, N);
    float environment_light_multiplier = max(ibl_params.w, 0.0);

    float3 F0 = directF0;
    float3 F  = F_Schlick90(NoV, F0, directF90);

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
    irr *= environment_light_multiplier;
    float3 diffuse_ibl = kd * base * irr;
    float3 material_diffuse_ibl = diffuse_ibl;
    // Sheen ambient (Phase 35-1a): irradiance を sheen 色で着色した簡易 ambient + base 減衰
    if (sheenWeight > 0.0) {
        float maxC = max(sheenColor.r, max(sheenColor.g, sheenColor.b));
        float baseAttenuation =
            saturate(1.0 - sheenWeight * maxC * 0.5);
        material_diffuse_ibl *= baseAttenuation;
        diffuse_ibl = diffuse_ibl * baseAttenuation
                    + irr * sheenColor * sheenWeight * 0.25;
    }

    // 環境鏡面の反射元 radiance。prefilter cubemap が無い backend (raw-DX12、SH9 モード) では
    // SH9 を反射方向で素に評価して «空グラデの反射» を近似する (cubemap 不要・metals が空を映す)。
    // cubemap 有り (Diligent) では従来通り roughness 段階の prefilter mip を使う。
    float3 prefilt;
    if (ibl_params.z >= 0.5) {
        prefilt = Sh9Radiance(R, sh9);
    } else {
        float mip_lvl = roughness * max(ibl_params.y - 1.0, 0.0);
        prefilt = prefilter.SampleLevel(prefilter_sampler, R, mip_lvl).rgb;
    }
    prefilt *= environment_light_multiplier;
    float2 lut_xy = brdf_lut.SampleLevel(brdf_lut_sampler, float2(NoV, roughness), 0).rg;
    // Phase 34e-2fix: 反射元の radiance を環境 prefilter (off-screen) から SSR
    // (on-screen の実ジオメトリ) へ blend。BRDF 応答 (split-sum scale+bias) は共通。
    float3 reflected = lerp(prefilt, ssr_radiance, saturate(ssr_weight));
    // Iridescence (Phase 35-1b): split-sum の F0 を薄膜変調した値へ差し替える
    float3 specF0 = lerp(F0, iridFresnel, iridWeight);
    float3 specular_ibl = reflected * (specF0 * lut_xy.x + directF90 * lut_xy.y);
    if (secondWeight > 0.0) {
        float secondMip = secondRoughness * max(ibl_params.y - 1.0, 0.0);
        float3 secondPrefilt = (ibl_params.z >= 0.5)
            ? Sh9Radiance(R, sh9)
            : prefilter.SampleLevel(prefilter_sampler, R, secondMip).rgb;
        secondPrefilt *= environment_light_multiplier;
        float2 secondLut = brdf_lut.SampleLevel(
            brdf_lut_sampler, float2(NoV, secondRoughness), 0).rg;
        float3 secondSpec = secondPrefilt
            * (specF0 * secondLut.x + directF90 * secondLut.y);
        specular_ibl = lerp(specular_ibl, secondSpec, saturate(secondWeight));
    }

    // 拡散 IBL: WickedEngine は間接拡散を削らない (GIBoost>=1 で寧ろ増す)。以前の ×0.45 は «浮く»
    // 対策の local hack で影/環境光領域が ~55% 暗く dull/flat だった。接地は AO + 強い太陽キー
    // (SunIntensity) + 接地影/キャスト影で担保し、IBL は ~0.85 (ほぼ等倍) へ戻す。鏡面 IBL は据え置き。
    diffuseLighting = material_diffuse_ibl * (0.85 * ao);
    return (diffuse_ibl * 0.85 + specular_ibl) * ao;
}

// IBL specular for clear-coat layer (split-sum、F0=0.04 固定の dielectric)。
// 戻り値: (1) coat の IBL specular、(2) base 層への透過率 (= 1 - F_coat)。
struct IblCoatTerm { float3 spec; float attenuation; };
IblCoatTerm ComputeIblClearcoat(float3 N, float3 V, float clearcoat,
                                float coat_roughness, float3 F0c) {
    IblCoatTerm o;
    o.spec = float3(0, 0, 0);
    o.attenuation = 1.0;
    if (clearcoat > 0.0) {
    float NoV = saturate(dot(N, V));
    float3 R  = reflect(-V, N);
    float3 Fc  = FresnelSchlickRoughness(NoV, F0c, coat_roughness);
    float mip_lvl = coat_roughness * max(ibl_params.y - 1.0, 0.0);
    float3 prefilt = prefilter.SampleLevel(prefilter_sampler, R, mip_lvl).rgb;
    float2 lut_xy = brdf_lut.SampleLevel(brdf_lut_sampler, float2(NoV, coat_roughness), 0).rg;
    o.spec = prefilt * (F0c * lut_xy.x + lut_xy.y) * clearcoat
           * max(ibl_params.w, 0.0);
    // base 透過: Fresnel * clearcoat 強度ぶんを引く
    o.attenuation = 1.0 - max(Fc.r, max(Fc.g, Fc.b)) * clearcoat;
    }
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
    float3 iridescence_value = float3(1.0, 1.0, 1.0);
    // 膜厚 0 付近で filmIor を媒質側へ寄せ、効果を滑らかに消す
    float iridIor = lerp(outsideIor, filmIor, smoothstep(0.0, 0.03, thicknessNm));
    // 膜内の屈折角 (Snell の法則)
    float sinTheta2Sq = (outsideIor / iridIor) * (outsideIor / iridIor)
                      * (1.0 - cosTheta1 * cosTheta1);
    float cosTheta2Sq = 1.0 - sinTheta2Sq;
    if (cosTheta2Sq >= 0.0) {
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
    iridescence_value = max(I, float3(0.0, 0.0, 0.0));
    }
    return iridescence_value;
}

// Cook-Torrance BRDF * NdotL (light vector L is from surface to light source).
// anisotropy ≠ 0 のとき D / G を anisotropic 版に切替える。tangent (T) はオブジェクト
// から与えられる主軸方向、B (bitangent) は cross(N, T) で生成。
float3 BrdfCookTorrance(float3 N, float3 V, float3 L,
                       float3 base, float metallic, float roughness,
                       float anisotropy, float3 T_world,
                       float3 iridFresnel, float iridWeight,
                       float3 directF0, float3 directF90,
                       float secondRoughness, float secondWeight,
                       out float3 diffuseLighting)
{
    float3 brdf_value = float3(0, 0, 0);
    diffuseLighting = float3(0, 0, 0);
    float3 H = normalize(V + L);
    float NdotL = saturate(dot(N, L));
    if (NdotL > 0.0) {
    float NdotV = saturate(dot(N, V));
    float NdotH = saturate(dot(N, H));
    float VdotH = saturate(dot(V, H));

    float3 F0 = directF0;
    float  a  = roughness * roughness;
    float  D = 0.0;
    float  G = 0.0;
    float3 F = F_Schlick90(VdotH, F0, directF90);
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
        // G_Smith_Aniso は既に可視性形 V = G/(4 NoV NoL) (Heitz 2014) を返す
        G = G_Smith_Aniso(NdotV, ToV, BoV, NdotL, ToL, BoL, ax, ay);
    } else {
        D = D_GGX(NdotH, a * a);
        G = V_SmithGGXCorrelated(NdotV, NdotL, a * a);   // height-correlated 可視性 (1/4NoVNoL 内包)
    }

    // G は可視性 V (= G/(4 NoV NoL)) なので分母の別掛けは不要 (WickedEngine 同形)
    float3 specular = D * G * F;
    if (secondWeight > 0.0) {
        float a_second = max(secondRoughness * secondRoughness, 1e-3);
        float D_second = D_GGX(NdotH, a_second * a_second);
        float G_second = V_SmithGGXCorrelated(NdotV, NdotL, a_second * a_second);
        specular = lerp(specular, D_second * G_second * F, saturate(secondWeight));
    }
    float3 kd = (1.0 - F) * (1.0 - metallic);
    float3 diffuse = kd * base / PI;
    diffuseLighting = diffuse * NdotL;
    brdf_value = (diffuse + specular) * NdotL;
    }
    return brdf_value;
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
    float3 f0;
    float3 f90;
    float  second_roughness;
    float  second_weight;
    float3 mean_free_path_cm;
    float  phase_anisotropy;
    float3 coat_f0;
};

// ---- Typed per-pixel Substrate expression VM -------------------------------
static const uint ACS_EXPR_CONSTANT = 0u;
static const uint ACS_EXPR_SCALAR_PARAMETER = 1u;
static const uint ACS_EXPR_VECTOR_PARAMETER = 2u;
static const uint ACS_EXPR_TEXTURE_SAMPLE_2D = 3u;
static const uint ACS_EXPR_UV0 = 4u;
static const uint ACS_EXPR_TIME = 5u;
static const uint ACS_EXPR_WORLD_POSITION = 6u;
static const uint ACS_EXPR_WORLD_NORMAL = 7u;
static const uint ACS_EXPR_ADD = 8u;
static const uint ACS_EXPR_MULTIPLY = 9u;
static const uint ACS_EXPR_LERP = 10u;
static const uint ACS_EXPR_CLAMP = 11u;
static const uint ACS_EXPR_POWER = 12u;
static const uint ACS_EXPR_DOT = 13u;
static const uint ACS_EXPR_NORMALIZE = 14u;
static const uint ACS_EXPR_NOISE = 15u;
static const uint ACS_EXPR_COMPONENT = 16u;

uint4 AcsExprWords0(uint instruction) {
    return substrate_expr_instructions[instruction * 3u + 0u];
}
uint4 AcsExprWords1(uint instruction) {
    return substrate_expr_instructions[instruction * 3u + 1u];
}
uint4 AcsExprWords2(uint instruction) {
    return substrate_expr_instructions[instruction * 3u + 2u];
}
uint AcsExprOp(uint instruction) {
    return AcsExprWords0(instruction).x & 255u;
}
uint AcsExprWidth(uint instruction) {
    return (AcsExprWords0(instruction).x >> 8u) & 255u;
}
int AcsExprSigned16(uint value) {
    return (value & 32768u) != 0u
        ? int(value | 4294901760u) : int(value);
}
int AcsExprInput(uint instruction, uint slot) {
    uint4 words = AcsExprWords0(instruction);
    if (slot == 0u) return AcsExprSigned16(words.y & 65535u);
    if (slot == 1u) return AcsExprSigned16(words.y >> 16u);
    return AcsExprSigned16(words.z & 65535u);
}
float4 AcsExprLiteral(uint instruction) {
    uint4 words1 = AcsExprWords1(instruction);
    uint4 words2 = AcsExprWords2(instruction);
    return asfloat(uint4(words1.w, words2.x, words2.y, words2.z));
}
float4 AcsExprCanonical(float4 value, uint width) {
    float4 canonical = 0.0.xxxx;
    if (width == 1u) {
        canonical = float4(value.x, 0.0, 0.0, 0.0);
    } else if (width == 2u) {
        canonical = float4(value.xy, 0.0, 0.0);
    } else if (width == 3u) {
        canonical = float4(value.xyz, 0.0);
    } else {
        canonical = value;
    }
    return canonical;
}
float4 AcsExprBroadcast(float4 value, uint source_width) {
    return source_width == 1u ? value.xxxx : value;
}
float AcsExprSafePower(float base, float exponent) {
    float power_value = 1.0;
    if (base == 0.0 && exponent > 0.0) {
        power_value = 0.0;
    } else if (!(base == 0.0 && exponent == 0.0)) {
        float p = log2(max(abs(base), 1.0e-6))
                * clamp(exponent, -32.0, 32.0);
        power_value = exp2(clamp(p, -126.0, 126.0));
    }
    return power_value;
}
float4 AcsExprSafePower4(float4 base, float4 exponent) {
    return float4(
        AcsExprSafePower(base.x, exponent.x),
        AcsExprSafePower(base.y, exponent.y),
        AcsExprSafePower(base.z, exponent.z),
        AcsExprSafePower(base.w, exponent.w));
}
float AcsExprClampScalar(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}
float4 AcsExprClamp4(float4 value, float4 low, float4 high) {
    return float4(
        AcsExprClampScalar(value.x, low.x, high.x),
        AcsExprClampScalar(value.y, low.y, high.y),
        AcsExprClampScalar(value.z, low.z, high.z),
        AcsExprClampScalar(value.w, low.w, high.w));
}
float4 AcsExprNormalize(float4 value, uint width) {
    float length_squared = value.x * value.x;
    if (width > 1u) length_squared += value.y * value.y;
    if (width > 2u) length_squared += value.z * value.z;
    if (width > 3u) length_squared += value.w * value.w;
    return length_squared > 1.0e-20
        ? value * rsqrt(length_squared) : 0.0.xxxx;
}
float AcsExprNoise(float4 sample_position, uint width) {
    float projection = sample_position.x * 12.9898;
    if (width > 1u) projection += sample_position.y * 78.233;
    if (width > 2u) projection += sample_position.z * 37.719;
    if (width > 3u) projection += sample_position.w * 19.913;
    return frac(sin(projection) * 43758.5453123);
}
float AcsExprSrgbChannel(float value) {
    float decoded_value = value / 12.92;
    if (value > 0.04045) {
        float power_base = max((value + 0.055) / 1.055, 0.0);
        decoded_value = pow(power_base, 2.4);
    }
    return decoded_value;
}
float4 AcsExprDecodeSrgb(float4 value, uint flags) {
    float4 decoded_value = value;
    if ((flags & 8u) != 0u) {
        decoded_value = float4(
            AcsExprSrgbChannel(value.r),
            AcsExprSrgbChannel(value.g),
            AcsExprSrgbChannel(value.b),
            value.a);
    }
    return decoded_value;
}
float2 AcsExprLinearUv(float2 uv, uint flags, uint2 dimensions) {
    float2 half_texel =
        0.5 / max(float2(dimensions), float2(1.0, 1.0));
    if ((flags & 2u) != 0u) {
        uv.x = clamp(uv.x, half_texel.x, 1.0 - half_texel.x);
    }
    if ((flags & 4u) != 0u) {
        uv.y = clamp(uv.y, half_texel.y, 1.0 - half_texel.y);
    }
    return uv;
}
int2 AcsExprPointCoord(float2 uv, uint flags, uint2 dimensions) {
    uv.x = (flags & 2u) != 0u ? saturate(uv.x) : frac(uv.x);
    uv.y = (flags & 4u) != 0u ? saturate(uv.y) : frac(uv.y);
    int2 coord = int2(floor(uv * float2(dimensions)));
    return clamp(coord, int2(0, 0), int2(dimensions) - int2(1, 1));
}
float4 AcsExprSampleTexture0(float2 uv, uint flags) {
    uint width = 1u, height = 1u;
    expression_texture0.GetDimensions(width, height);
    uint2 dimensions = uint2(max(width, 1u), max(height, 1u));
    float4 value = 0.0.xxxx;
    if ((flags & 1u) != 0u) {
        value = expression_texture0.SampleLevel(
            expression_texture0_sampler,
            AcsExprLinearUv(uv, flags, dimensions), 0.0);
    } else {
        value = expression_texture0.Load(
            int3(AcsExprPointCoord(uv, flags, dimensions), 0));
    }
    return AcsExprDecodeSrgb(value, flags);
}
float4 AcsExprSampleTexture1(float2 uv, uint flags) {
    uint width = 1u, height = 1u;
    expression_texture1.GetDimensions(width, height);
    uint2 dimensions = uint2(max(width, 1u), max(height, 1u));
    float4 value = 0.0.xxxx;
    if ((flags & 1u) != 0u) {
        value = expression_texture1.SampleLevel(
            expression_texture1_sampler,
            AcsExprLinearUv(uv, flags, dimensions), 0.0);
    } else {
        value = expression_texture1.Load(
            int3(AcsExprPointCoord(uv, flags, dimensions), 0));
    }
    return AcsExprDecodeSrgb(value, flags);
}
float4 AcsExprSampleTexture2(float2 uv, uint flags) {
    uint width = 1u, height = 1u;
    expression_texture2.GetDimensions(width, height);
    uint2 dimensions = uint2(max(width, 1u), max(height, 1u));
    float4 value = 0.0.xxxx;
    if ((flags & 1u) != 0u) {
        value = expression_texture2.SampleLevel(
            expression_texture2_sampler,
            AcsExprLinearUv(uv, flags, dimensions), 0.0);
    } else {
        value = expression_texture2.Load(
            int3(AcsExprPointCoord(uv, flags, dimensions), 0));
    }
    return AcsExprDecodeSrgb(value, flags);
}
float4 AcsExprSampleTexture3(float2 uv, uint flags) {
    uint width = 1u, height = 1u;
    expression_texture3.GetDimensions(width, height);
    uint2 dimensions = uint2(max(width, 1u), max(height, 1u));
    float4 value = 0.0.xxxx;
    if ((flags & 1u) != 0u) {
        value = expression_texture3.SampleLevel(
            expression_texture3_sampler,
            AcsExprLinearUv(uv, flags, dimensions), 0.0);
    } else {
        value = expression_texture3.Load(
            int3(AcsExprPointCoord(uv, flags, dimensions), 0));
    }
    return AcsExprDecodeSrgb(value, flags);
}
float4 AcsExprSampleTexture(uint slot, float2 uv, uint flags,
                            float4 fallback_value) {
    float4 sampled_value = fallback_value;
    if (slot < 4u &&
        (substrate_expr_meta.w & (1u << slot)) != 0u) {
        if (slot == 0u) {
            sampled_value = AcsExprSampleTexture0(uv, flags);
        } else if (slot == 1u) {
            sampled_value = AcsExprSampleTexture1(uv, flags);
        } else if (slot == 2u) {
            sampled_value = AcsExprSampleTexture2(uv, flags);
        } else {
            sampled_value = AcsExprSampleTexture3(uv, flags);
        }
    }
    return sampled_value;
}
float4 AcsExprParameter(uint id, uint type, float4 fallback_value) {
    [loop]
    for (uint i = 0u; i < substrate_expr_meta.z && i < 32u; ++i) {
        if (substrate_expr_parameter_meta[i].x == id &&
            substrate_expr_parameter_meta[i].y == type) {
            return AcsExprCanonical(
                substrate_expr_parameter_values[i], type);
        }
    }
    return fallback_value;
}
void AcsEvaluateExpressions(float2 uv, float3 world_position,
                            float3 world_normal,
                            out float4 registers[64]) {
    [unroll]
    for (uint register_index = 0u; register_index < 64u;
         ++register_index) {
        registers[register_index] = 0.0.xxxx;
    }
    [loop]
    for (uint instruction = 0u;
         instruction < substrate_expr_meta.x && instruction < 64u;
         ++instruction) {
        uint op = AcsExprOp(instruction);
        uint width = AcsExprWidth(instruction);
        int input0 = AcsExprInput(instruction, 0u);
        int input1 = AcsExprInput(instruction, 1u);
        int input2 = AcsExprInput(instruction, 2u);
        float4 a = input0 >= 0 ? registers[input0] : 0.0.xxxx;
        float4 b = input1 >= 0 ? registers[input1] : 0.0.xxxx;
        float4 c = input2 >= 0 ? registers[input2] : 0.0.xxxx;
        uint width_a = input0 >= 0 ? AcsExprWidth(uint(input0)) : 0u;
        uint width_b = input1 >= 0 ? AcsExprWidth(uint(input1)) : 0u;
        uint width_c = input2 >= 0 ? AcsExprWidth(uint(input2)) : 0u;
        float4 aa = AcsExprBroadcast(a, width_a);
        float4 bb = AcsExprBroadcast(b, width_b);
        float4 cc = AcsExprBroadcast(c, width_c);
        float4 value = 0.0.xxxx;
        uint4 words0 = AcsExprWords0(instruction);
        uint4 words1 = AcsExprWords1(instruction);
        if (op == ACS_EXPR_CONSTANT) {
            value = AcsExprLiteral(instruction);
        } else if (op == ACS_EXPR_SCALAR_PARAMETER ||
                   op == ACS_EXPR_VECTOR_PARAMETER) {
            value = AcsExprParameter(
                words0.w, width, AcsExprLiteral(instruction));
        } else if (op == ACS_EXPR_TEXTURE_SAMPLE_2D) {
            uint slot = words1.x & 255u;
            uint flags = (words1.x >> 8u) & 255u;
            value = AcsExprSampleTexture(
                slot, a.xy, flags, AcsExprLiteral(instruction));
        } else if (op == ACS_EXPR_UV0) {
            value = float4(uv, 0.0, 0.0);
        } else if (op == ACS_EXPR_TIME) {
            value = float4(substrate_expr_context.x, 0.0, 0.0, 0.0);
        } else if (op == ACS_EXPR_WORLD_POSITION) {
            value = float4(world_position, 0.0);
        } else if (op == ACS_EXPR_WORLD_NORMAL) {
            value = float4(world_normal, 0.0);
        } else if (op == ACS_EXPR_ADD) {
            value = aa + bb;
        } else if (op == ACS_EXPR_MULTIPLY) {
            value = aa * bb;
        } else if (op == ACS_EXPR_LERP) {
            value = aa + (bb - aa) * cc;
        } else if (op == ACS_EXPR_CLAMP) {
            value = AcsExprClamp4(aa, bb, cc);
        } else if (op == ACS_EXPR_POWER) {
            value = AcsExprSafePower4(aa, bb);
        } else if (op == ACS_EXPR_DOT) {
            float dot_value = a.x * b.x;
            if (width_a > 1u) dot_value += a.y * b.y;
            if (width_a > 2u) dot_value += a.z * b.z;
            if (width_a > 3u) dot_value += a.w * b.w;
            value = float4(dot_value, 0.0, 0.0, 0.0);
        } else if (op == ACS_EXPR_NORMALIZE) {
            value = AcsExprNormalize(a, width);
        } else if (op == ACS_EXPR_NOISE) {
            value = float4(
                AcsExprNoise(a, width_a), 0.0, 0.0, 0.0);
        } else if (op == ACS_EXPR_COMPONENT) {
            uint component = words1.x & 255u;
            value = float4(a[component], 0.0, 0.0, 0.0);
        }
        if (!all(isfinite(value))) value = 0.0.xxxx;
        registers[instruction] = AcsExprCanonical(value, width);
    }
}

// 1 光源ぶんの layered BRDF を評価 (base Cook-Torrance + clearcoat 層 + sheen 層)。
// L = surface→light、戻り値は放射輝度係数 (光色・距離減衰・shadow は呼び側で乗算)。
// dir/area/point の 3 ループはこの 1 関数を呼ぶだけにし、lobe 追加を 1 箇所に集約する。
float3 EvalSurfaceForLight(float3 N, float3 V, float3 L, SurfaceMaterial m,
                           bool applyAnalyticSss,
                           out float3 diffuseLighting) {
    float3 baseDiffuse = float3(0, 0, 0);
    float3 base_brdf = BrdfCookTorrance(N, V, L, m.albedo, m.metallic, m.roughness,
                                        m.anisotropy, m.aniso_tangent,
                                        m.irid_fresnel, m.irid_weight,
                                        m.f0, m.f90,
                                        m.second_roughness, m.second_weight,
                                        baseDiffuse);
    ClearcoatTerm cc = EvalClearcoat(N, V, L, m.clearcoat, m.coat_roughness,
                                     m.coat_f0);
    float3 lit = base_brdf * cc.attenuation + cc.spec_times_nol;
    diffuseLighting = baseDiffuse * cc.attenuation;

    // Thin-material subsurface approximation.  This is deliberately not advertised as
    // screen-space SSSS: it only redistributes the direct diffuse lobe and adds a small,
    // bounded back-light response.  Replacing the Lambert lobe (instead of adding on top
    // of it) keeps the material from creating energy as sss_weight approaches one.
    if (applyAnalyticSss && m.sss_weight > 0.0 && m.metallic < 1.0) {
        float mfp = dot(m.mean_free_path_cm, float3(0.2126, 0.7152, 0.0722));
        const float kWrap = lerp(0.20, 0.60, saturate(mfp / (mfp + 1.0)));
        const float kTransmissionShare = lerp(0.08, 0.24, saturate(mfp / (mfp + 0.5)));
        const float kDistortion = lerp(0.05, 0.35,
                                      saturate(m.phase_anisotropy * 0.5 + 0.5));
        const float kPower = 4.0;

        float rawNoL = dot(N, L);
        float NoL = saturate(rawNoL);

        float3 halfVector = V + L;
        float3 H = halfVector * rsqrt(max(dot(halfVector, halfVector), 1e-6));
        float3 F = F_Schlick90(saturate(dot(V, H)), m.f0, m.f90);
        F = lerp(F, m.irid_fresnel, m.irid_weight);
        float3 kd = (1.0 - F) * (1.0 - m.metallic);

        // 1/(1+wrap) normalizes the wrapped cosine over all incident directions.
        float wrappedResponse = saturate((rawNoL + kWrap) / (1.0 + kWrap))
                              / (1.0 + kWrap);
        float3 bentBackLight = normalize(-L + N * kDistortion);
        float transmission = pow(saturate(dot(V, bentBackLight)), kPower)
                           * saturate(-rawNoL);
        float scatterResponse = wrappedResponse * (1.0 - kTransmissionShare)
                              + transmission * kTransmissionShare;

        // Tint can only absorb channels; it never raises the lobe above the normalized
        // response.  The delta replaces the exact diffuse term used by the base BRDF.
        float3 scatterTint = lerp(float3(1.0, 1.0, 1.0), saturate(m.sss_color), 0.75);
        float3 lambertDiffuse = kd * m.albedo * (NoL / PI);
        float3 scatteredDiffuse = kd * m.albedo * scatterTint * (scatterResponse / PI);
        float3 analyticSssDelta =
            (scatteredDiffuse - lambertDiffuse)
            * saturate(m.sss_weight) * cc.attenuation;
        lit += analyticSssDelta;
        diffuseLighting += analyticSssDelta;
    }

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
        float baseAttenuation =
            saturate(1.0 - m.sheen_weight * maxC * 0.5);
        diffuseLighting *= baseAttenuation;
        lit = lit * baseAttenuation + sheen;
    }
    return lit;
}

// camera と surface の間の指数 height density を解析積分し、区間平均を返す。
// fog_base 以下は密度一定なので、区間が基準面を横切る場合も piecewise に厳密積分する。
float FogHeightAverage(float camera_h, float surface_h, float falloff) {
    float k = max(falloff, 0.0);
    float average_density = 1.0;
    if (k >= 1e-5) {
        float a = camera_h;
        float b = surface_h;
        float d = b - a;
        if (abs(d) < 1e-4 || abs(k * d) < 1e-4) {
            average_density =
                exp(-min(k * max(0.5 * (a + b), 0.0), 80.0));
        } else if (a <= 0.0 && b <= 0.0) {
            average_density = 1.0;
        } else if (a > 0.0 && b > 0.0) {
            float ea = exp(-min(k * a, 80.0));
            float eb = exp(-min(k * b, 80.0));
            average_density = saturate((ea - eb) / (k * d));
        } else {
            float crossing = saturate(-a / d);
            if (a <= 0.0) {
                float eb = exp(-min(k * max(b, 0.0), 80.0));
                average_density =
                    saturate(crossing + (1.0 - eb) / (k * d));
            } else {
                float ea = exp(-min(k * max(a, 0.0), 80.0));
                average_density = saturate(
                    (ea - 1.0) / (k * d) + (1.0 - crossing));
            }
        }
    }
    return average_density;
}

// 1 / (4*pi) を除いた HG 位相関数。g=0 で 1 になり、既存 fog 色の露出を保つ。
float FogHgPhase(float cos_theta, float g) {
    g = clamp(g, -0.85, 0.85);
    float g2 = g * g;
    float denom = max(1.0 + g2 - 2.0 * g * cos_theta, 1e-3);
    return (1.0 - g2) / (denom * sqrt(denom));
}
)" R"(
struct PbrSurfaceOutputs {
    float4 scene_color;
    float4 diffuse_lighting;
    float4 ssss_material;
    float4 world_normal;
};

PbrSurfaceOutputs EvaluatePbr(VSOut v, bool applyAnalyticSss) {
    float3 geometric_normal = normalize(v.world_n);
    bool substrate_enabled = substrate_secondary.w > 0.5;
    float slab_values[39];
    slab_values[0] = substrate_diffuse_coverage.x;
    slab_values[1] = substrate_diffuse_coverage.y;
    slab_values[2] = substrate_diffuse_coverage.z;
    slab_values[3] = substrate_f0.x;
    slab_values[4] = substrate_f0.y;
    slab_values[5] = substrate_f0.z;
    slab_values[6] = substrate_f90.x;
    slab_values[7] = substrate_f90.y;
    slab_values[8] = substrate_f90.z;
    slab_values[9] = pbr_params.y;
    slab_values[10] = substrate_secondary.x;
    slab_values[11] = substrate_secondary.y;
    slab_values[12] = ext_params.z;
    slab_values[13] = aniso_tangent.x;
    slab_values[14] = aniso_tangent.y;
    slab_values[15] = aniso_tangent.z;
    slab_values[16] = substrate_mfp_thickness.x;
    slab_values[17] = substrate_mfp_thickness.y;
    slab_values[18] = substrate_mfp_thickness.z;
    slab_values[19] = substrate_secondary.z;
    slab_values[20] = emissive.x;
    slab_values[21] = emissive.y;
    slab_values[22] = emissive.z;
    slab_values[23] = substrate_transmittance.x;
    slab_values[24] = substrate_transmittance.y;
    slab_values[25] = substrate_transmittance.z;
    slab_values[26] = substrate_mfp_thickness.w;
    slab_values[27] = sheen_params.x;
    slab_values[28] = sheen_params.y;
    slab_values[29] = sheen_params.z;
    slab_values[30] = sheen_params.w;
    slab_values[31] = sheen_rough.x;
    slab_values[32] = irid_params.x;
    slab_values[33] = irid_params.y;
    slab_values[34] = irid_params.z;
    slab_values[35] = substrate_normal.x;
    slab_values[36] = substrate_normal.y;
    slab_values[37] = substrate_normal.z;
    slab_values[38] = substrate_normal.w;

    if (substrate_enabled && substrate_expr_meta.x > 0u &&
        substrate_expr_meta.y > 0u) {
        float4 expression_registers[64];
        AcsEvaluateExpressions(
            v.uv, v.world_p, geometric_normal, expression_registers);
        [loop]
        for (uint binding_index = 0u;
             binding_index < substrate_expr_meta.y &&
             binding_index < 39u;
             ++binding_index) {
            uint4 binding = substrate_expr_bindings[binding_index];
            uint target = binding.x & 255u;
            uint instruction = (binding.x >> 8u) & 255u;
            uint component = (binding.x >> 16u) & 3u;
            if (target < 39u && instruction < substrate_expr_meta.x) {
                float coefficient = asfloat(binding.y);
                float authored_literal = asfloat(binding.z);
                slab_values[target] += coefficient *
                    (expression_registers[instruction][component] -
                     authored_literal);
            }
        }
    }

    float3 dynamic_diffuse = saturate(float3(
        slab_values[0], slab_values[1], slab_values[2]));
    float3 dynamic_f0 = saturate(float3(
        slab_values[3], slab_values[4], slab_values[5]));
    float3 dynamic_f90 = saturate(float3(
        slab_values[6], slab_values[7], slab_values[8]));
    float dynamic_roughness = max(slab_values[9], 0.04);
    float dynamic_second_roughness = max(slab_values[10], 0.04);
    float dynamic_second_weight = saturate(slab_values[11]);
    float dynamic_anisotropy = clamp(slab_values[12], -1.0, 1.0);
    float3 dynamic_tangent = float3(
        slab_values[13], slab_values[14], slab_values[15]);
    float3 dynamic_mfp = max(float3(
        slab_values[16], slab_values[17], slab_values[18]), 0.0.xxx);
    float dynamic_phase = clamp(slab_values[19], -0.99, 0.99);
    float3 dynamic_emissive = max(float3(
        slab_values[20], slab_values[21], slab_values[22]), 0.0.xxx);
    float3 dynamic_transmittance = saturate(float3(
        slab_values[23], slab_values[24], slab_values[25]));
    float dynamic_thickness = max(slab_values[26], 0.0);
    float3 dynamic_fuzz_color = saturate(float3(
        slab_values[27], slab_values[28], slab_values[29]));
    float dynamic_fuzz_amount = saturate(slab_values[30]);
    float dynamic_fuzz_roughness = saturate(slab_values[31]);
    float dynamic_irid_weight = saturate(slab_values[32]);
    float dynamic_irid_thickness = max(slab_values[33], 0.0);
    float dynamic_irid_ior = max(slab_values[34], 1.0);
    float3 dynamic_normal = float3(
        slab_values[35], slab_values[36], slab_values[37]);
    float dynamic_normal_strength = clamp(slab_values[38], 0.0, 4.0);
    float dynamic_coverage = saturate(substrate_diffuse_coverage.w);

    float3 N = geometric_normal;
    // Blend tangent-space normals as slopes. This preserves a stable positive
    // Z hemisphere at high strengths instead of merely multiplying a unit
    // normal and renormalizing it back to the original direction.
    float3 sampled_nm =
        normal_map.Sample(normal_map_sampler, v.uv).rgb * 2.0 - 1.0;
    sampled_nm = normalize(sampled_nm + float3(0, 0, 1e-6));
    float texture_strength = clamp(pbr_params.w, 0.0, 4.0);
    float substrate_strength =
        substrate_enabled ? dynamic_normal_strength : 1.0;
    float2 texture_slope =
        sampled_nm.xy / max(abs(sampled_nm.z), 1e-4);
    texture_slope *= texture_strength * substrate_strength;
    float2 authored_slope = float2(0, 0);
    if (substrate_enabled) {
        float3 authored_nm =
            normalize(dynamic_normal + float3(0, 0, 1e-6));
        authored_slope =
            authored_nm.xy / max(abs(authored_nm.z), 1e-4);
        authored_slope *= substrate_strength;
    }
    float3 nm = normalize(float3(
        texture_slope + authored_slope, 1.0));
    N = PerturbNormal(v.world_p, N, v.uv, nm);
    float3 V = normalize(camera_pos.xyz - v.world_p);

    // Source color textures are uploaded as linear UNORM so expression slots
    // can choose their own color space. Standard albedo is defined as sRGB:
    // decode exactly once before multiplying linear material constants.
    float3 albedo_texel_linear = AcsExprDecodeSrgb(
        albedo.Sample(albedo_sampler, v.uv), 8u).rgb;
    float3 albedo_rgb = albedo_texel_linear *
        (substrate_enabled ? dynamic_diffuse : base_color.xyz);
    float  metallic   = substrate_enabled ? 0.0 : pbr_params.x;
    float  roughness  = substrate_enabled
        ? dynamic_roughness : max(pbr_params.y, 0.04);
    float  ao         = pbr_params.z;
    // 拡張 material
    float  clearcoat       = saturate(ext_params.x);
    float  coat_roughness  = max(ext_params.y, 0.04);
    float  anisotropy      = substrate_enabled
        ? dynamic_anisotropy : ext_params.z;
    float3 aniso_T_world   = substrate_enabled
        ? dynamic_tangent : aniso_tangent.xyz;

    // 光源ループ 3 種 (dir/area/point) が共有する per-pixel マテリアル
    SurfaceMaterial mat;
    mat.albedo         = albedo_rgb;
    mat.metallic       = metallic;
    mat.roughness      = roughness;
    mat.clearcoat      = clearcoat;
    mat.coat_roughness = coat_roughness;
    mat.anisotropy     = anisotropy;
    mat.aniso_tangent  = aniso_T_world;
    mat.sheen_color     = substrate_enabled
        ? dynamic_fuzz_color : sheen_params.xyz;
    mat.sheen_weight    = substrate_enabled
        ? dynamic_fuzz_amount : sheen_params.w;
    mat.sheen_roughness = substrate_enabled
        ? dynamic_fuzz_roughness : sheen_rough.x;
    // Iridescence (Phase 35-1b): 薄膜 Fresnel を per-pixel に 1 度だけ NoV で評価。
    mat.irid_weight  = substrate_enabled
        ? dynamic_irid_weight : irid_params.x;
    mat.irid_fresnel = float3(0.0, 0.0, 0.0);
    if (mat.irid_weight > 0.0) {
        float3 F0_base = substrate_enabled
            ? dynamic_f0
            : lerp(float3(0.04, 0.04, 0.04), albedo_rgb, metallic);
        float film_ior = substrate_enabled
            ? dynamic_irid_ior : irid_params.z;
        float film_thickness = substrate_enabled
            ? dynamic_irid_thickness : irid_params.y;
        mat.irid_fresnel = EvalIridescence(
            1.0, film_ior, saturate(dot(N, V)),
            film_thickness, F0_base);
    }
    float dynamic_mfp_max = max(
        dynamic_mfp.x, max(dynamic_mfp.y, dynamic_mfp.z));
    float dynamic_thickness_response =
        dynamic_thickness / (dynamic_thickness + 0.01);
    mat.sss_color = substrate_enabled
        ? lerp(dynamic_diffuse, dynamic_transmittance, 0.5)
        : sss_params.xyz;
    mat.sss_weight = substrate_enabled
        ? saturate(dynamic_mfp_max) * saturate(dynamic_thickness_response)
        : sss_params.w;
    mat.f0 = substrate_enabled
        ? dynamic_f0
        : lerp(float3(0.04,0.04,0.04), albedo_rgb, metallic);
    mat.f90 = substrate_enabled ? dynamic_f90 : float3(1,1,1);
    mat.second_roughness = substrate_enabled
        ? dynamic_second_roughness : roughness;
    mat.second_weight = substrate_enabled ? dynamic_second_weight : 0.0;
    mat.mean_free_path_cm = substrate_enabled
        ? dynamic_mfp : float3(0,0,0);
    mat.phase_anisotropy = substrate_enabled
        ? dynamic_phase : 0.0;
    mat.coat_f0 = substrate_enabled
        ? saturate(substrate_coat_f0.xyz) : float3(0.04,0.04,0.04);

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
        // roughness 連動の «滑らかな» 反射ブラー。疎な 3x3 box gather (半径 roughness*0.015) は
        // タップ間に隙間が空き反射がブロック状/低解像に見えていた。Vogel ディスク 12 tap (golden
        // angle, sqrt で面積均一, 画素毎回転) で隙間なく平滑化。半径は roughness^2 で立ち上げ →
        // glossy 面は半径≈0 でほぼ素通り (crisp/フル解像)、rough 面のみ広くぼかす (WickedEngine の
        // roughness-aware resolve に相当)。
        float  rough2  = roughness * roughness;
        float  radius  = rough2 * 0.010;
        float  rot     = frac(sin(dot(ssr_uv, float2(12.9898, 78.233))) * 43758.5453) * 6.2831853;
        float3 sum     = float3(0, 0, 0);
        float  amask   = 0.0;
        const int kSsrTaps = 12;
        [unroll]
        for (int i = 0; i < kSsrTaps; ++i) {
            float  t   = (float(i) + 0.5) / float(kSsrTaps);
            float  ang = rot + float(i) * 2.39996323;          // golden angle
            float2 off = float2(cos(ang), sin(ang)) * (sqrt(t) * radius);
            float4 s   = ssr_color.SampleLevel(ssr_color_sampler, ssr_uv + off, 0);
            sum   += s.rgb;
            amask += s.a;
        }
        ssr_rgb    = sum * (1.0 / float(kSsrTaps));
        // hit mask * intensity * (1-roughness) で rough 面ほど SSR を弱める
        ssr_weight = (amask / float(kSsrTaps)) * ssr_params.y * (1.0 - roughness);
    }

    // 環境光: ibl_params.x が 1 なら IBL ambient、0 なら flat ambient。
    // uniform branching なので片方の TextureCube サンプルは PSO の dead code
    // として削除される (FXC の判断による)。
    float3 col;
    float3 diffuse_col = float3(0, 0, 0);
    // IBL cubemap が無くても SH9 / probe grid が有れば ambient 経路へ (SH9 standalone を許可)。
    if (ibl_params.x >= 0.5 || ibl_params.z >= 0.5 || probe_params.x >= 1.0) {
        float3 base_ibl_diffuse = float3(0, 0, 0);
        float3 base_ibl = ComputeIblAmbient(N, V, v.world_p, albedo_rgb, metallic, roughness, ao,
                                            ssr_rgb, ssr_weight,
                                            mat.sheen_color, mat.sheen_weight,
                                            mat.irid_fresnel, mat.irid_weight,
                                            mat.f0, mat.f90,
                                            mat.second_roughness, mat.second_weight,
                                            base_ibl_diffuse);
        IblCoatTerm cc_ibl = ComputeIblClearcoat(N, V, clearcoat, coat_roughness,
                                                 mat.coat_f0);
        col = (base_ibl * cc_ibl.attenuation + cc_ibl.spec * ao) * ssao_factor;
        diffuse_col =
            base_ibl_diffuse * cc_ibl.attenuation * ssao_factor;
    } else {
        col = ambient.xyz * albedo_rgb * ao * ssao_factor
            * max(ibl_params.w, 0.0);
        diffuse_col = col;
    }

    // SSGI (Phase 33c): screen-space 1 bounce indirect light を ambient に加算。
    // 非金属の albedo に modulate (Lambertian-like)、AO も乗せる (SSGI ray 自身は
    // hemisphere sampling だが、SSAO の幾何遮蔽でさらに抑える)。
    if (ssgi_params.x >= 0.5) {
        float2 screen_uv = v.pos.xy * float2(ssao_params.z, ssao_params.w);
        float3 gi = ssgi_color.SampleLevel(ssgi_color_sampler, screen_uv, 0).rgb;
        float3 gi_diffuse =
            gi * albedo_rgb * (1.0 - metallic) *
            ssgi_params.y * ssao_factor;
        col += gi_diffuse;
        diffuse_col += gi_diffuse;
    }

    // Lightmap (Phase 33f): mesh の uv で baked static GI を sample して
    // ambient に加算。非金属の diffuse のみ (metallic surface には baked light は乗らない)。
    if (lightmap_params.x >= 0.5) {
        float3 lm = lightmap.SampleLevel(lightmap_sampler, v.uv, 0).rgb;
        float3 lightmap_diffuse =
            lm * albedo_rgb * (1.0 - metallic) * lightmap_params.y;
        col += lightmap_diffuse;
        diffuse_col += lightmap_diffuse;
    }

    // 有向光源 (i==0 のみ shadow_map で遮蔽)
    float shadow = ComputeShadow(v.world_p, v.view_z);
    float cloud_shadow = ComputeCloudShadowTransmittance(v.world_p);
    int dir_count = (int)ambient.w;
    [loop]
    for (int i = 0; i < ACS_MAX_DIR_LIGHTS; ++i) {
        if (i >= dir_count) break;
        float3 L = normalize(light_dir[i].xyz);
        // i==0 (= sun) は shadow map (PCSS) と contact shadow (Phase 34q) の両方で遮蔽
        float k = (i == 0)
            ? (shadow * contact_shadow * cloud_shadow)
            : 1.0;
        float3 light_diffuse = float3(0, 0, 0);
        float3 light_lit = EvalSurfaceForLight(
            N, V, L, mat, applyAnalyticSss, light_diffuse);
        col += light_color[i].xyz * light_lit * k;
        diffuse_col += light_color[i].xyz * light_diffuse * k;
    }

    // 矩形 area light: 4x4 stratified sample で Monte Carlo 積分。
    // それぞれのサンプル点を point light として扱い、area 全面積で正規化する。
    // (面積光の特徴: 軟らかい highlight elongation、近距離での明るい照り)
    int area_count = (int)point_count_pad.y;
    [loop]
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
            float3 area_diffuse_sum = float3(0, 0, 0);
            const int kSamples = 4;
            [loop]
            for (int sy = 0; sy < kSamples; ++sy) {
                float vy = ((float)sy + 0.5) / (float)kSamples * 2.0 - 1.0;
                [loop]
                for (int sx = 0; sx < kSamples; ++sx) {
                    float vx = ((float)sx + 0.5) / (float)kSamples * 2.0 - 1.0;
                    float3 sample_pos = area_c + axisX * vx + axisY * vy;
                    float3 to_l = sample_pos - v.world_p;
                    float  dist = length(to_l);
                    float3 L = to_l / max(dist, 1e-4);
                    // inverse square + 面要素の cos(法線方向)
                    float cos_area = saturate(-dot(area_n, L));
                    float att = cos_area / max(dist * dist, 1e-4);
                    float3 area_diffuse = float3(0, 0, 0);
                    area_sum += EvalSurfaceForLight(
                        N, V, L, mat, applyAnalyticSss,
                        area_diffuse) * att;
                    area_diffuse_sum += area_diffuse * att;
                }
            }
            // (axisX × axisY の長さ) = 面の半面積 ×4。N_sample で割って full area で乗算。
            float area = 4.0 * length(cross(axisX, axisY));
            float area_scale = area / (float)(kSamples * kSamples);
            col += col_a * area_sum * area_scale;
            diffuse_col += col_a * area_diffuse_sum * area_scale;
        }
    }

    // 点光源 (距離減衰)
    int pt_count = (int)point_count_pad.x;
    [loop]
    for (int j = 0; j < ACS_MAX_POINT_LIGHTS; ++j) {
        if (j >= pt_count) break;
        float3 to_light = point_pos_range[j].xyz - v.world_p;
        float  dist = length(to_light);
        float  rng  = max(point_pos_range[j].w, 1e-4);
        if (dist >= rng) continue;
        float3 L = to_light / max(dist, 1e-4);
        float  att = 1.0 - dist / rng;
        att = att * att;
        float3 point_diffuse = float3(0, 0, 0);
        float3 point_lit = EvalSurfaceForLight(
            N, V, L, mat, applyAnalyticSss, point_diffuse);
        col += point_color[j].xyz * point_lit * att;
        diffuse_col += point_color[j].xyz * point_diffuse * att;
    }

    // Emissive (Phase 34l): 自己発光。lighting と無関係に加算する。fog より前に
    // 足すので、発光体も距離フォグで減衰する (遠くの発光は霞む)。
    col += substrate_enabled ? dynamic_emissive : emissive.rgb;

    // Volumetric height fog: Beer-Lambert extinction を camera→surface の全区間で解析積分。
    // endpoint density だけを距離へ掛ける近似と異なり、斜面・谷・俯瞰カメラでも連続する。
    if (fog_color_density.w > 0.0) {
        float3 camera_to_surface = v.world_p - camera_pos.xyz;
        float dist = min(length(camera_to_surface), 100000.0);
        float3 view_ray = camera_to_surface / max(dist, 1e-4);
        float camera_h = camera_pos.y - fog_height_params.y;
        float surface_h = v.world_p.y - fog_height_params.y;
        float average_density = FogHeightAverage(camera_h, surface_h, fog_height_params.x);
        float optical_depth = min(fog_color_density.w * dist * average_density, 80.0);
        float transmittance = exp(-optical_depth);

        float3 in_scatter = fog_color_density.xyz;
        if (dir_count > 0 && fog_height_params.w > 0.0) {
            // light_dir[0] は surface→sun と同じ向き。太陽を正面に見ると前方散乱が最大。
            float cos_theta = dot(view_ray, normalize(light_dir[0].xyz));
            float phase = FogHgPhase(cos_theta, fog_height_params.z);
            // surface shadow は経路全体の厳密な volumetric shadow ではないため、完全には消さず
            // direct in-scatter のみを穏やかに減衰する。
            float sun_visibility = lerp(0.55, 1.0, saturate(shadow));
            in_scatter += fog_color_density.xyz * max(light_color[0].xyz, 0.0)
                          * (phase * fog_height_params.w * sun_visibility);
        }
        col = col * transmittance + in_scatter * (1.0 - transmittance);
        // Atmospheric in-scatter is not material diffuse and must not be
        // diffused by the later screen-space pass.
        diffuse_col *= transmittance;
    }

    // Aerial perspective (WickedEngine camera-volume LUT)。screen uv + 深度→スライス (LUT 焼きの
    // squared 分布の逆 = sqrt) で froxel volume を trilinear サンプル → col = col*(1-ap.a) + ap.rgb。
    // 物理大気を積分した滑らかな 3D LUT なので、cubemap サンプル由来の «斜めの段» が原理的に出ない。
    if (ap_params.x > 0.5) {
        float apDist = length(v.world_p - camera_pos.xyz);
        float zc = sqrt(saturate(apDist / max(ap_params.y, 1e-3)));
        // SSAO の設定有無に依存させず、同じ view-projection から正規化 screen UV を得る。
        // SV_POSITION / viewport size の組では SetSsao 未使用時に inv size が 0 になっていた。
        float4 apClip = mul(float4(v.world_p, 1.0), view_proj);
        float2 apNdc = apClip.xy / max(abs(apClip.w), 1e-6);
        float2 sUv = saturate(float2(apNdc.x * 0.5 + 0.5, -apNdc.y * 0.5 + 0.5));
        float4 ap = ap_volume.SampleLevel(ap_volume_sampler, float3(sUv, zc), 0);
        col = col * (1.0 - ap.a) + ap.rgb;
        diffuse_col *= (1.0 - ap.a);
    }

    float output_alpha =
        substrate_enabled ? dynamic_coverage : base_color.w;
    // The material MRT carries a physical RGB diffusion profile, not a
    // quantized colour hash. Substrate authors mean-free-path and thickness in
    // centimetres; editor scene units are metres. Legacy SSS uses its scalar
    // as both coverage and maximum MFP, with subsurfaceColor defining the
    // relative RGB distances. A neutral 0.5 thickness response keeps that
    // established one-scalar authoring at exactly 1x radius.
    float thickness_response = substrate_enabled
        ? saturate(dynamic_thickness / (dynamic_thickness + 0.01))
        : 0.5;
    float3 profile_radius_cm = substrate_enabled
        ? dynamic_mfp
        : saturate(sss_params.xyz) * saturate(sss_params.w);
    float radius_scale =
        lerp(0.75, 1.25, thickness_response);
    float3 scatter_radius_world = min(
        max(profile_radius_cm, 0.0.xxx) * (0.01 * radius_scale),
        1000.0.xxx);
    float profile_max_world = max(
        scatter_radius_world.x,
        max(scatter_radius_world.y, scatter_radius_world.z));
    float coverage = profile_max_world > 1e-7
        ? saturate(mat.sss_weight)
        : 0.0;

    PbrSurfaceOutputs outputs;
    outputs.scene_color = float4(col, output_alpha);
    outputs.diffuse_lighting = float4(diffuse_col, output_alpha);
    outputs.ssss_material = coverage > 1e-4
        ? float4(scatter_radius_world, coverage)
        : float4(0, 0, 0, 0);
    // Preserve the final shading normal, after tangent-space normal mapping
    // and per-pixel Substrate expressions. SSSS uses this instead of a
    // geometric prepass normal so diffusion cannot cross authored detail.
    outputs.world_normal = float4(normalize(N), 1.0);
    return outputs;
}

float4 PSMain(VSOut v) : SV_TARGET {
    // The established one-RT fallback keeps its bounded analytic thin SSS.
    return EvaluatePbr(v, true).scene_color;
}

struct PbrSsssMrtOutput {
    float4 scene_color : SV_TARGET0;
    float4 diffuse_lighting : SV_TARGET1;
    float4 ssss_material : SV_TARGET2;
    float4 world_normal : SV_TARGET3;
};

PbrSsssMrtOutput PSMainSsss(VSOut v) {
    // MRT starts from unblurred diffuse. Analytic SSS is disabled so the
    // blurred-original replacement cannot double-scatter the same energy.
    PbrSurfaceOutputs surface = EvaluatePbr(v, false);
    PbrSsssMrtOutput outputs;
    outputs.scene_color = surface.scene_color;
    outputs.diffuse_lighting = surface.diffuse_lighting;
    outputs.ssss_material = surface.ssss_material;
    outputs.world_normal = surface.world_normal;
    return outputs;
}
)";

/** 有向光源の最大数 (HLSL の ACS_MAX_DIR_LIGHTS と一致)。 */
constexpr u32 kMaxDirLights   = 4;

/** 点光源の最大数 (HLSL の ACS_MAX_POINT_LIGHTS と一致)。 */
constexpr u32 kMaxPointLights = 4;

/** 矩形 area light の最大数 (HLSL の ACS_MAX_AREA_LIGHTS と一致)。 */
constexpr u32 kMaxAreaLights  = 2;

/**
 * per-frame 定数バッファ (b0) の C++ 側ミラーレイアウト。
 *
 * @details HLSL の cbuffer Frame と完全に一致させる。ライト・環境・probe・fog・
 * shadow (CSM 4 cascade)・SSAO/SSGI/lightmap/SSR の各パラメータを保持する。
 */
struct FFrameCBLayout {
    /** カメラの view-projection 行列。 */
    FMat4 view_proj;

    /** カメラ world position (xyz=eye、w=pad)。 */
    FVec4 camera_pos;

    /** 環境光 (xyz=ambient color、w=dir_count)。 */
    FVec4 ambient;

    /** 光源数 (x=point_count、y=area_count)。 */
    FVec4 point_count_pad;

    /** 各有向光源の方向 (xyz)。 */
    FVec4 light_dir   [kMaxDirLights];

    /** 各有向光源の色 (xyz)。 */
    FVec4 light_color [kMaxDirLights];

    /** 各点光源の位置と range (xyz=pos、w=range)。 */
    FVec4 point_pos_range[kMaxPointLights];

    /** 各点光源の色 (xyz)。 */
    FVec4 point_color    [kMaxPointLights];

    /** IBL パラメータ (x=有効、y=prefilter mip 数、z=SH9、w=環境光倍率)。 */
    FVec4 ibl_params;

    /** SH 9 single mode の係数 (各 xyz=RGB)。 */
    FVec4 sh9[9];

    /** 各 area light の中心 world position。 */
    FVec4 area_center [kMaxAreaLights];

    /** 各 area light の axis_x*half_width。 */
    FVec4 area_axis_x [kMaxAreaLights];

    /** 各 area light の axis_y*half_height。 */
    FVec4 area_axis_y [kMaxAreaLights];

    /** 各 area light の色。 */
    FVec4 area_color  [kMaxAreaLights];

    /** probe grid パラメータ (x=probe_count)。 */
    FVec4 probe_params;

    /** 各 probe の world position (xyz)。 */
    FVec4 probe_pos[4];

    /** 各 probe の SH 9 係数 (probe ごとに連続した 9 個)。 */
    FVec4 probe_sh9[4 * 9];

    /** fog の色と密度 (xyz=color、w=density)。 */
    FVec4 fog_color_density;

    /** fog パラメータ (x=height_falloff、y=base、z=anisotropy、w=sun_scatter)。 */
    FVec4 fog_height_params;

    /**
     * 各 cascade の light VP (最大 4)。
     *
     * @details single mode は shadow_view_proj[0] のみ使用、残りは backward compat の
     * inf split で常にスキップされる。
     */
    FMat4 shadow_view_proj[4];

    /** shadow パラメータ (x=bias、y=enabled、z=texel_size、w=filter_radius)。 */
    FVec4 shadow_params;

    /** 各 cascade の view-space z far (xyzw=cascade 0/1/2/3、inf=未使用)。 */
    FVec4 cascade_splits;

    /** cascade atlas の UV スケール (x=atlas X scale: single=1/N-cascade=1/N、y=1)。 */
    FVec4 cascade_uv_scale;

    /** SSAO パラメータ (x=enabled、y=intensity、zw=inv_viewport)。 */
    FVec4 ssao_params;

    /** SSGI パラメータ (x=enabled、y=intensity)。 */
    FVec4 ssgi_params;

    /** lightmap パラメータ (x=enabled、y=intensity)。 */
    FVec4 lightmap_params;

    /** SSR パラメータ (x=enabled、y=intensity)。 */
    FVec4 ssr_params;

    /** Aerial perspective パラメータ (x=enabled、y=max_dist scene)。 */
    FVec4 ap_params;

    /** 雲影地図の座標 (xy=左下XZ、z=1/範囲、w=有効)。 */
    FVec4 cloud_shadow_map_params;

    /** 雲影の投影 (xyz=受光点から太陽への方向、w=基準面Y)。 */
    FVec4 cloud_shadow_projection;

    /** 雲影の受光範囲 (x=雲底高度、y=正規化画素幅、z=太陽Y下限、w=惑星半径)。 */
    FVec4 cloud_shadow_layer;

    /** 曲面雲殻の接平面を置いたワールド原点。 */
    FVec4 cloud_shadow_world_origin;
};

/**
 * per-object 定数バッファ (b1) の C++ 側ミラーレイアウト。
 *
 * @details HLSL の cbuffer Object と完全に一致させる。model 行列と PBR 基本パラメータ +
 * 拡張 lobe (clearcoat/anisotropy/emissive/sheen/iridescence/SSS) を保持する。
 */
struct FObjectCbLayout {
    /** モデル行列。 */
    FMat4 model;

    /** inverse-transpose normal matrix の上位 3 行。 */
    FVec4 normal_row0;
    FVec4 normal_row1;
    FVec4 normal_row2;

    /** ベースカラー (xyz=color、w=alpha)。 */
    FVec4 base_color;

    /** PBR 基本パラメータ (x=metallic、y=roughness、z=ao)。 */
    FVec4 pbr_params;

    /** 拡張パラメータ (x=clearcoat、y=coat_roughness、z=anisotropy、w=flags)。 */
    FVec4 ext_params;

    /** anisotropic tangent (xyz=world tangent)。 */
    FVec4 aniso_tangent;

    /** emissive (xyz=color*strength)。 */
    FVec4 emissive;

    /** sheen (xyz=sheen color、w=sheen weight)。 */
    FVec4 sheen_params;

    /** sheen roughness (x=roughness)。 */
    FVec4 sheen_rough;

    /** iridescence (x=weight、y=thickness(nm)、z=film IOR)。 */
    FVec4 irid_params;

    /** subsurface (xyz=color、w=weight)。 */
    FVec4 sss_params;

    /** Direct Substrate interface and medium values. */
    FVec4 substrate_f0;
    FVec4 substrate_f90;
    FVec4 substrate_diffuse_coverage;
    FVec4 substrate_secondary;
    FVec4 substrate_mfp_thickness;
    FVec4 substrate_transmittance;
    FVec4 substrate_normal;
    FVec4 substrate_coat_f0;
    FShaderExpressionInstruction
        substrate_expr_instructions[kShaderExpressionMaxNodes];
    FSubstrateExpressionBinding
        substrate_expr_bindings[kSubstrateSlabScalarCount];
    u32 substrate_expr_parameter_meta[kShaderExpressionMaxParameters][4];
    FVec4 substrate_expr_parameter_values[kShaderExpressionMaxParameters];
    u32 substrate_expr_meta[4];
    FVec4 substrate_expr_context;
};

static_assert(
    offsetof(FObjectCbLayout, substrate_expr_instructions) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_instructions) ==
    offsetof(FObjectCbLayout, substrate_coat_f0) + sizeof(FVec4));
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_bindings) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_bindings) ==
    offsetof(FObjectCbLayout, substrate_expr_instructions) +
        sizeof(FShaderExpressionInstruction) * kShaderExpressionMaxNodes);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_parameter_meta) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_parameter_meta) ==
    offsetof(FObjectCbLayout, substrate_expr_bindings) +
        sizeof(FSubstrateExpressionBinding) *
            kSubstrateSlabScalarCount);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_parameter_values) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_parameter_values) ==
    offsetof(FObjectCbLayout, substrate_expr_parameter_meta) +
        sizeof(u32) * kShaderExpressionMaxParameters * 4u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_meta) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_meta) ==
    offsetof(FObjectCbLayout, substrate_expr_parameter_values) +
        sizeof(FVec4) * kShaderExpressionMaxParameters);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_context) % 16u == 0u);
static_assert(
    offsetof(FObjectCbLayout, substrate_expr_context) ==
    offsetof(FObjectCbLayout, substrate_expr_meta) + sizeof(u32) * 4u);
static_assert(
    sizeof(FObjectCbLayout) ==
    offsetof(FObjectCbLayout, substrate_expr_context) + sizeof(FVec4));
static_assert(sizeof(FObjectCbLayout) % 16u == 0u);

/**
 * 定数バッファサイズを 256 バイト境界に切り上げて返す。
 *
 * @tparam T サイズを求める CB レイアウト型。
 * @return 256 バイトアラインされた T のサイズ。
 */
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

} // namespace

EShaderStatus CPbrShader::FCompiledShaders::Status() const noexcept {
    if (!vertex || !pixel) return EShaderStatus::Failed;
    const EShaderStatus vertex_status = vertex->Status();
    const EShaderStatus pixel_status = pixel->Status();
    if (vertex_status == EShaderStatus::Failed ||
        pixel_status == EShaderStatus::Failed) {
        return EShaderStatus::Failed;
    }
    if (vertex_status == EShaderStatus::Compiling ||
        pixel_status == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    // The MRT shader is optional, but if a backend accepted an asynchronous
    // compile request, wait for it to settle before installing the set.
    // A settled failure is ignored so the one-RT path remains available.
    if (pixel_subsurface_mrt &&
        pixel_subsurface_mrt->Status() == EShaderStatus::Compiling) {
        return EShaderStatus::Compiling;
    }
    return EShaderStatus::Ready;
}

/** Compile raw-DX12 bytecode without accessing the render device. */
TResult<CPbrShader::FCompiledShaders>
CPbrShader::CompileShadersCpu(bool include_subsurface_mrt) noexcept {
#if !WITH_RENDER_DILIGENT
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Pbr.VS";

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Pbr.PS";

    auto vertex = MakeUnique<FDx12Shader>();
    if (!vertex)
        return ACS_ERR(Memory, 371, "PBR vertex shader allocation failed");
    const FHrResult vertex_result = vertex->Init(vs_d);
    if (vertex_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 372, "PBR vertex shader CPU compile failed",
            static_cast<u32>(vertex_result.hr));
    }

    auto pixel = MakeUnique<FDx12Shader>();
    if (!pixel)
        return ACS_ERR(Memory, 373, "PBR pixel shader allocation failed");
    const FHrResult pixel_result = pixel->Init(ps_d);
    if (pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 374, "PBR pixel shader CPU compile failed",
            static_cast<u32>(pixel_result.hr));
    }

    FCompiledShaders compiled{};
    compiled.vertex = TUniquePtr<IRhiShader>(
        vertex.Release(), vertex.GetAllocator());
    compiled.pixel = TUniquePtr<IRhiShader>(
        pixel.Release(), pixel.GetAllocator());
    // Optional capability: base PBR installation must survive a missing
    // compiler feature, shader error or allocation failure here.
    if (include_subsurface_mrt) {
        FShaderDesc ssss_ps_d = ps_d;
        ssss_ps_d.entry_point = "PSMainSsss";
        ssss_ps_d.debug_name = "Pbr.SsssMrt.PS";
        if (auto ssss_pixel = MakeUnique<FDx12Shader>(); ssss_pixel) {
            const FHrResult ssss_result = ssss_pixel->Init(ssss_ps_d);
            if (ssss_result.IsOk()) {
                compiled.pixel_subsurface_mrt = TUniquePtr<IRhiShader>(
                    ssss_pixel.Release(), ssss_pixel.GetAllocator());
            }
        }
    }
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    (void)include_subsurface_mrt;
    return ACS_ERR(
        Render, 375,
        "PBR CPU compilation is available only on the raw DX12 backend");
#endif
}

TResult<CPbrShader::FCompiledShaders>
CPbrShader::BeginCompileShadersAsync(
    IRhiDevice& device,
    bool include_subsurface_mrt) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 377,
            "PBR backend-managed asynchronous compilation is unsupported");
    }

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name = "Pbr.VS";
    vs_d.compile_async = true;

    FCompiledShaders compiled{};
    auto vertex = CreateRhiShader(device, vs_d);
    if (vertex.IsErr()) return Err<FCompiledShaders>(vertex.Error());
    compiled.vertex = Move(vertex.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name = "Pbr.PS";
    ps_d.compile_async = true;
    auto pixel = CreateRhiShader(device, ps_d);
    if (pixel.IsErr()) return Err<FCompiledShaders>(pixel.Error());
    compiled.pixel = Move(pixel.Value());

    if (include_subsurface_mrt) {
        FShaderDesc ssss_ps_d = ps_d;
        ssss_ps_d.entry_point = "PSMainSsss";
        ssss_ps_d.debug_name = "Pbr.SsssMrt.PS";
        auto ssss_pixel = CreateRhiShader(device, ssss_ps_d);
        if (ssss_pixel.IsOk()) {
            compiled.pixel_subsurface_mrt = Move(ssss_pixel.Value());
        }
    }

    return TResult<FCompiledShaders>(OkInit, Move(compiled));
}

/** シェーダ・PSO・CB・fallback テクスチャ群を生成する。 */
TResult<void> CPbrShader::Init(
    IRhiDevice& device,
    EFormat rt_format,
    EFormat depth_format,
    ECullMode cull_mode,
    bool include_subsurface_mrt) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kPbrHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "Pbr.VS";

    FCompiledShaders compiled{};
    if (auto r = CreateRhiShader(device, vs_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.vertex = Move(r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kPbrHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "Pbr.PS";
    if (auto r = CreateRhiShader(device, ps_d); r.IsErr())
        return Err<void>(r.Error());
    else
        compiled.pixel = Move(r.Value());

    if (include_subsurface_mrt) {
        FShaderDesc ssss_ps_d = ps_d;
        ssss_ps_d.entry_point = "PSMainSsss";
        ssss_ps_d.debug_name = "Pbr.SsssMrt.PS";
        if (auto r = CreateRhiShader(device, ssss_ps_d); r.IsOk()) {
            compiled.pixel_subsurface_mrt = Move(r.Value());
        }
    }

    return InitWithCompiledShaders(
        device, Move(compiled), rt_format, depth_format, cull_mode);
}

/** Install CPU-compiled bytecode and create all owner-thread RHI resources. */
TResult<void> CPbrShader::InitWithCompiledShaders(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    EFormat rt_format,
    EFormat depth_format,
    ECullMode cull_mode) noexcept {
    return InitWithCompiledShadersInternal(
        device, Move(shaders), rt_format, depth_format, cull_mode);
}

TResult<void> CPbrShader::BuildInitializedCandidateForRawDx12(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    EFormat rt_format,
    EFormat depth_format,
    ECullMode cull_mode) noexcept {
#if !WITH_RENDER_DILIGENT
    const char* const backend = device.BackendName();
    if (backend == nullptr || std::strcmp(backend, "DX12") != 0) {
        return ACS_ERR(
            Render, 378,
            "PBR background candidate creation requires raw DX12");
    }
    if (m_ResourceDevice != nullptr) {
        return ACS_ERR(
            Render, 379,
            "PBR background candidate target is already initialized");
    }
    return InitWithCompiledShadersInternal(
        device, Move(shaders), rt_format, depth_format, cull_mode);
#else
    (void)device;
    (void)shaders;
    (void)rt_format;
    (void)depth_format;
    (void)cull_mode;
    return ACS_ERR(
        Render, 378,
        "PBR background candidate creation requires raw DX12");
#endif
}

TResult<void> CPbrShader::InitWithCompiledShadersInternal(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    EFormat rt_format,
    EFormat depth_format,
    ECullMode cull_mode) noexcept {
    if (shaders.vertex.Get() == nullptr || shaders.pixel.Get() == nullptr) {
        return ACS_ERR(Render, 376, "PBR compiled shader set is incomplete");
    }
    using FInitClock = std::chrono::steady_clock;
    const auto init_started = FInitClock::now();

    // Keep the live shader fully usable until every replacement resource,
    // including the PSO, has been created.  Besides giving re-initialization a
    // strong failure guarantee, leaving `shaders` untouched until commit lets
    // a caller retry a transient owner-thread RHI failure without recompiling
    // the expensive bytecode.
    struct FInitCandidates {
        TUniquePtr<IRhiPipeline> pipeline;
        TUniquePtr<IRhiPipeline> subsurface_mrt_pipeline;
        TUniquePtr<IRhiBuffer> frame_cb;
        /** per-object定数をまとめる未公開arena。 */
        CTransientUploadArena object_arena;
        TUniquePtr<IRhiTexture> white;
        TUniquePtr<IRhiTexture> shadow_fb;
        TUniquePtr<IRhiTexture> normal_map_fb;
        TUniquePtr<IRhiTexture> ssao_fb;
        TUniquePtr<IRhiTexture> ssgi_fb;
        TUniquePtr<IRhiTexture> lightmap_fb;
        TUniquePtr<IRhiTexture> ssr_fb;
        TUniquePtr<IRhiTexture> ap_fb;
        TUniquePtr<IRhiTexture> ibl_irradiance_fb;
        TUniquePtr<IRhiTexture> ibl_prefilter_fb;
        TUniquePtr<IRhiTexture> ibl_brdf_fb;
    } candidates{};

    FBufferDesc fb{};
    fb.size = CBSize<FFrameCBLayout>();
    fb.usage = EBufferUsage::Uniform;
    fb.cpu_writable = true;
    if (auto r = CreateRhiBuffer(device, fb); r.IsErr())
        return Err<void>(r.Error());
    else candidates.frame_cb = Move(r.Value());

    // 64個の論理slotを一つの共有GPUページへまとめ、drawごとのresource生成を除く。
    /** 共有object upload arenaの生成結果。 */
    auto object_arena_result = candidates.object_arena.Init(device, CBSize<FObjectCbLayout>(), kInitialObjectBufferCapacity);
    if (object_arena_result.IsErr()) return Err<void>(object_arena_result.Error());
    const auto constant_buffers_ready = FInitClock::now();

    // 1x1 白テクスチャ (albedo fallback)
    const u8 white[4] = {255, 255, 255, 255};
    FTextureDesc td{};
    td.width = 1; td.height = 1;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = white; td.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, td); r.IsErr())
        return Err<void>(r.Error());
    else candidates.white = Move(r.Value());

    // Shadow map fallback: 1x1 RGBA8 全 255 (.r=1.0 = far、shadow_params.y=0 で
    // shader 側が早期 return するので実際には sample されない。SRB の有効 binding 要件用)。
    const u8 far_depth[4] = { 255, 255, 255, 255 };
    FTextureDesc sd{};
    sd.width = 1; sd.height = 1;
    sd.format = EFormat::R8G8B8A8_UNorm;
    sd.initial_data = far_depth; sd.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, sd); r.IsErr())
        return Err<void>(r.Error());
    else candidates.shadow_fb = Move(r.Value());

    // Normal map fallback: 1x1 RGBA8 (128,128,255,0) = tangent (0,0,1) → 無変化
    const u8 flat_nrm[4] = { 128, 128, 255, 0 };
    FTextureDesc nt{};
    nt.width = 1; nt.height = 1;
    nt.format = EFormat::R8G8B8A8_UNorm;
    nt.initial_data = flat_nrm; nt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, nt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.normal_map_fb = Move(r.Value());

    // SSAO fallback: 1x1 全 255 = visibility 1.0 (AO 無し)。ssao_params.x=0 でも
    // SRB に valid texture を bind する要件のため作成しておく。
    const u8 full_vis[4] = { 255, 255, 255, 255 };
    FTextureDesc st{};
    st.width = 1; st.height = 1;
    st.format = EFormat::R8G8B8A8_UNorm;
    st.initial_data = full_vis; st.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, st); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ssao_fb = Move(r.Value());

    // SSGI fallback: 1x1 R11G11B10F、初期値 0 (indirect light なし)。SRB binding 用。
    // initial_data 経路は R11G11B10F でサポート無いので、blank RT として作る。
    // ssgi_params.x=0 で shader が早期 return するので内容は未定義のままで OK。
    FTextureDesc gt{};
    gt.width = 1; gt.height = 1;
    gt.format = EFormat::R11G11B10_Float;
    gt.is_render_target = true;     // RT 兼用にすると SRV が自動で付く
    if (auto r = CreateRhiTexture(device, gt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ssgi_fb = Move(r.Value());

    // Lightmap fallback: 1x1 RGBA8 全 0 (baked light なし)。
    // lightmap_params.x=0 で shader が早期 return するので unused。
    const u8 zero_rgba[4] = { 0, 0, 0, 0 };
    FTextureDesc lt{};
    lt.width = 1; lt.height = 1;
    lt.format = EFormat::R8G8B8A8_UNorm;
    lt.initial_data = zero_rgba; lt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, lt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.lightmap_fb = Move(r.Value());

    // SSR fallback: 1x1 RGBA8 全 0 (.a=0 → hit mask 0 = 反射なし)。
    // ssr_params.x=0 で shader が早期 return するので unused だが SRB binding 用。
    FTextureDesc rt{};
    rt.width = 1; rt.height = 1;
    rt.format = EFormat::R8G8B8A8_UNorm;
    rt.initial_data = zero_rgba; rt.initial_data_size = 4;
    if (auto r = CreateRhiTexture(device, rt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ssr_fb = Move(r.Value());

    // Aerial perspective fallback: 1x1x2 RGBA16F。RHI は depth > 1 のときだけ
    // Texture3D を作るため depth=1 では Texture2D SRV になり、Diligent の
    // Texture3D ap_volume binding validation に失敗する。ap_params.x=0 なので内容は未参照。
    FTextureDesc apt{};
    apt.width = 1; apt.height = 1; apt.depth = 2;
    apt.format = EFormat::R16G16B16A16_Float;
    if (auto r = CreateRhiTexture(device, apt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ap_fb = Move(r.Value());

    // IBL fallback: 1x1x6 R11G11B10F cubemap + 1x1 RG16F 2D。
    // shader が ibl_enabled=0 で uniform branch して sample しない想定だが、
    // SRB に valid な texture を bind する必要があるので作っておく。内容は
    // undefined (driver は通常 0 化する)。
    FTextureDesc ic{};
    ic.width = 1; ic.height = 1;
    ic.format = EFormat::R11G11B10_Float;
    ic.array_size = 6;
    ic.is_cubemap = true;
    if (auto r = CreateRhiTexture(device, ic); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ibl_irradiance_fb = Move(r.Value());
    if (auto r = CreateRhiTexture(device, ic); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ibl_prefilter_fb = Move(r.Value());

    FTextureDesc bt{};
    bt.width = 1; bt.height = 1;
    bt.format = EFormat::R16G16_Float;
    if (auto r = CreateRhiTexture(device, bt); r.IsErr())
        return Err<void>(r.Error());
    else candidates.ibl_brdf_fb = Move(r.Value());
    const auto fallback_resources_ready = FInitClock::now();

    FPipelineDesc pd{};
    pd.vs = shaders.vertex.Get();
    pd.ps = shaders.pixel.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = true;
    pd.depth_write   = true;
    pd.cull_mode     = cull_mode;
    pd.cbuffer_slots = 2;     // b0=Frame, b1=Object
    // t0..t10 はフレーム入力、t11..t14 は式、t15 は雲影に使う。
    pd.texture_slots = 16;
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
    pd.texture_names[10] = "ap_volume";
    pd.texture_names[11] = "expression_texture0";
    pd.texture_names[12] = "expression_texture1";
    pd.texture_names[13] = "expression_texture2";
    pd.texture_names[14] = "expression_texture3";
    pd.texture_names[15] = "cloud_shadow_transmittance";
    pd.static_sampler_count = 16;
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
    pd.static_samplers[5].filter     = ESamplerFilter::Linear;
    pd.static_samplers[5].address_u  = ESamplerAddress::Clamp;       // shadow map は clamp
    pd.static_samplers[5].address_v  = ESamplerAddress::Clamp;
    pd.static_samplers[5].comparison = true;                        // HW 比較 PCF (SampleCmpLevelZero、LessEqual)
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
    pd.static_samplers[10].filter    = ESamplerFilter::Linear;      // AP volume: 3D trilinear、clamp
    pd.static_samplers[10].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[10].address_v = ESamplerAddress::Clamp;
    pd.static_samplers[10].address_w = ESamplerAddress::Clamp;
    // Expression samplers are linear-wrap. ClampU/V is implemented exactly
    // in shader coordinates and point filtering uses Texture2D.Load.
    for (u32 i = 11u; i < 15u; ++i) {
        pd.static_samplers[i].filter = ESamplerFilter::Linear;
        pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
        pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
        pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
    }
    pd.static_samplers[15].filter = ESamplerFilter::Linear;
    pd.static_samplers[15].address_u = ESamplerAddress::Clamp;
    pd.static_samplers[15].address_v = ESamplerAddress::Clamp;
    pd.vertex_stride = sizeof(FMeshVertex);
    // MeshVertex の FVec3 は alignas(16) で 16 バイト境界。
    // → position@0, normal@16, uv@32 (Standard と一致)。
    // 12/24 にしてしまうと normal が position パディング + normal の途中を読んで
    // ジオメトリが破壊され、PBR が「黒っぽくべったり影」状態に見える。
    pd.layout[0] = { "POSITION", 0, EFormat::R32G32B32_Float, 0  };
    pd.layout[1] = { "NORMAL",   0, EFormat::R32G32B32_Float, 16 };
    pd.layout[2] = { "TEXCOORD", 0, EFormat::R32G32_Float,    32 };
    pd.layout_count = 3;
    if (auto r = CreateRhiPipeline(device, pd); r.IsErr())
        return Err<void>(r.Error());
    else candidates.pipeline = Move(r.Value());
    const auto base_pipeline_ready = FInitClock::now();

    // SSSS extraction is strictly optional. A shader/PSO/backend failure here
    // must not poison a complete and usable single-target PBR installation.
    if (shaders.pixel_subsurface_mrt &&
        shaders.pixel_subsurface_mrt->Status() == EShaderStatus::Ready) {
        FPipelineDesc ssss_pd = pd;
        ssss_pd.ps = shaders.pixel_subsurface_mrt.Get();
        ssss_pd.rt_count = 4u;
        ssss_pd.rt_formats[0] = rt_format;
        ssss_pd.rt_formats[1] = EFormat::R16G16B16A16_Float;
        ssss_pd.rt_formats[2] = EFormat::R16G16B16A16_Float;
        ssss_pd.rt_formats[3] = EFormat::R16G16B16A16_Float;
        if (auto r = CreateRhiPipeline(device, ssss_pd); r.IsOk()) {
            candidates.subsurface_mrt_pipeline = Move(r.Value());
        }
    }
    const auto mrt_pipeline_ready = FInitClock::now();
    const auto elapsed_ms = [](auto begin, auto end) noexcept {
        return std::chrono::duration<double, std::milli>(
            end - begin).count();
    };
    const char* const mrt_state =
        shaders.pixel_subsurface_mrt.Get() == nullptr
            ? "off"
            : (candidates.subsurface_mrt_pipeline.Get() != nullptr
                ? "ready" : "unavailable");
    ACS_LOG_INFO(
        "PBR owner commit phases: constant_buffers=%.3f ms, "
        "fallback_textures=%.3f ms, fallback_resources=%.3f ms, "
        "base_pso=%.3f ms, mrt_pso=%.3f ms, total=%.3f ms, mrt=%s",
        elapsed_ms(init_started, constant_buffers_ready),
        elapsed_ms(constant_buffers_ready, fallback_resources_ready),
        elapsed_ms(init_started, fallback_resources_ready),
        elapsed_ms(fallback_resources_ready, base_pipeline_ready),
        elapsed_ms(base_pipeline_ready, mrt_pipeline_ready),
        elapsed_ms(init_started, mrt_pipeline_ready),
        mrt_state);

    const bool device_changed =
        m_ResourceDevice != nullptr && m_ResourceDevice != &device;

    // Commit cannot fail.  Release the old PSO before its shaders/resources,
    // then publish the replacement PSO last so no live state can be observed
    // with mismatched dependencies.
    m_SubsurfaceMrtPipeline.Reset();
    m_Pipeline.Reset();
    m_SubsurfaceMrtPs.Reset();
    m_Vs = Move(shaders.vertex);
    m_Ps = Move(shaders.pixel);
    if (candidates.subsurface_mrt_pipeline) {
        m_SubsurfaceMrtPs = Move(shaders.pixel_subsurface_mrt);
    }
    m_FrameCb = Move(candidates.frame_cb);
    m_ObjectArena = Move(candidates.object_arena);
    m_White = Move(candidates.white);
    m_ShadowFb = Move(candidates.shadow_fb);
    m_NormalMapFb = Move(candidates.normal_map_fb);
    m_SsaoFb = Move(candidates.ssao_fb);
    m_SsgiFb = Move(candidates.ssgi_fb);
    m_LightmapFb = Move(candidates.lightmap_fb);
    m_SsrFb = Move(candidates.ssr_fb);
    m_ApFb = Move(candidates.ap_fb);
    m_IblIrradianceFb = Move(candidates.ibl_irradiance_fb);
    m_IblPrefilterFb = Move(candidates.ibl_prefilter_fb);
    m_IblBrdfFb = Move(candidates.ibl_brdf_fb);
    m_Pipeline = Move(candidates.pipeline);
    m_SubsurfaceMrtPipeline =
        Move(candidates.subsurface_mrt_pipeline);
    m_ObjectCbCursor = 0u;
    // The manual bind path may query before its first SetObject.
    m_CurrentObjectCb =
        m_ObjectArena.Capacity() == 0u ? kInvalidObjectBuffer : 0u;
    m_ObjectCapacityFailureLogged = false;

    // Non-owning texture bindings cannot cross RHI devices.  Preserve them for
    // a same-device PSO rebuild, but neutralize every dependent enable value
    // when the resource device changes so fallback textures remain harmless.
    if (device_changed) {
        m_IblIrradiance = nullptr;
        m_IblPrefilter = nullptr;
        m_IblBrdf = nullptr;
        m_IblMips = 0;
        m_IblEnabled = false;
        m_NormalMap = nullptr;
        m_NormalMapStrength = 1.0f;
        m_ShadowDepth = nullptr;
        m_ShadowParams.y = 0.0f;
        m_CloudShadowTransmittance = nullptr;
        m_CloudShadowMapParams = FVec4{0, 0, 0, 0};
        m_CloudShadowWorldOrigin = FVec4{0, 0, 0, 0};
        m_SsaoTex = nullptr;
        m_SsaoIntensity = 0.0f;
        m_SsaoInvW = 0.0f;
        m_SsaoInvH = 0.0f;
        m_SsgiTex = nullptr;
        m_SsgiIntensity = 0.0f;
        m_LightmapTex = nullptr;
        m_LightmapIntensity = 0.0f;
        m_SsrTex = nullptr;
        m_SsrIntensity = 0.0f;
        m_ApVol = nullptr;
        m_ApParams = FVec4{0, 0, 0, 0};
        m_SubstrateExpressionTextureMask = 0u;
        for (u32 slot = 0u;
             slot < kShaderExpressionMaxTextureSlots;
             ++slot) {
            m_SubstrateExpressionTextures[slot] = nullptr;
        }
    }
    m_ResourceDevice = &device;

    return Ok();
}

/** 全 GPU リソースと参照ポインタを解放する。 */
void CPbrShader::Shutdown() noexcept {
    ClearSubstrateSurface();
    m_SubsurfaceMrtPipeline.Reset();
    m_Pipeline.Reset();
    m_ObjectArena.Reset();
    m_FrameCb.Reset();
    m_ShadowFb.Reset();
    m_ShadowDepth = nullptr;
    m_CloudShadowTransmittance = nullptr;
    m_CloudShadowMapParams = FVec4{0, 0, 0, 0};
    m_CloudShadowProjection = FVec4{0, 1, 0, 0};
    m_CloudShadowLayer = FVec4{0, 0, 0.03f, 0};
    m_CloudShadowWorldOrigin = FVec4{0, 0, 0, 0};
    m_NormalMapFb.Reset();
    m_NormalMap = nullptr;
    m_NormalMapStrength = 1.0f;
    m_SsaoFb.Reset();
    m_SsaoTex = nullptr;
    m_SsaoIntensity = 0.0f;
    m_SsaoInvW = 0.0f;
    m_SsaoInvH = 0.0f;
    m_SsgiFb.Reset();
    m_SsgiTex = nullptr;
    m_SsgiIntensity = 0.0f;
    m_LightmapFb.Reset();
    m_LightmapTex = nullptr;
    m_LightmapIntensity = 0.0f;
    m_SsrFb.Reset();
    m_SsrTex = nullptr;
    m_SsrIntensity = 0.0f;
    m_ApFb.Reset();
    m_ApVol = nullptr;
    m_ApParams = FVec4{0, 0, 0, 0};
    m_IblBrdfFb.Reset();
    m_IblPrefilterFb.Reset();
    m_IblIrradianceFb.Reset();
    m_White.Reset();
    m_SubsurfaceMrtPs.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
    m_ResourceDevice = nullptr;
    m_IblIrradiance = nullptr;
    m_IblPrefilter  = nullptr;
    m_IblBrdf       = nullptr;
    m_IblMips       = 0;
    m_IblEnabled    = false;
    m_ShadowParams.y = 0.0f;
    m_ObjectCbCursor = 0u;
    m_CurrentObjectCb = kInvalidObjectBuffer;
    m_ObjectCapacityFailureLogged = false;
}

bool CPbrShader::BeginFrame(u32 required_object_draws) noexcept {
    /** 必要容量の確保結果。 */
    const bool capacity_ready = m_ObjectArena.BeginFrame(required_object_draws);
    m_ObjectCbCursor = 0u;
    m_CurrentObjectCb =
        m_ObjectArena.Capacity() == 0u ? kInvalidObjectBuffer : 0u;
    m_ObjectCapacityFailureLogged = false;
    return capacity_ready;
}

/** IBL テクスチャを記録し、3 つ揃っていれば IBL ambient を有効化する。 */
void CPbrShader::SetIbl(IRhiTexture* irradiance,
                       IRhiTexture* prefilter,
                       IRhiTexture* brdf_lut,
                       u32 prefilter_mips) noexcept {
    m_IblIrradiance = irradiance;
    m_IblPrefilter  = prefilter;
    m_IblBrdf       = brdf_lut;
    m_IblMips       = prefilter_mips;
    m_IblEnabled    = (irradiance != nullptr) && (prefilter != nullptr)
                       && (brdf_lut != nullptr) && (prefilter_mips > 0);
    FlushFrameCB();
}

/** 有限かつ 0 以上の値だけを受理し、環境由来の間接光倍率を更新する。 */
void CPbrShader::SetIblLightMultiplier(f32 multiplier) noexcept
{
    if (!std::isfinite(multiplier) || multiplier < 0.0f) return;
    m_IblLightMultiplier = multiplier;
    FlushFrameCB();
}

/** 従来 ABI を保ちつつ、高品質 fog の標準位相値を適用する。 */
void CPbrShader::SetFog(FVec3 color, f32 density, f32 height_falloff, f32 height_base) noexcept {
    SetFog(color, density, height_falloff, height_base, 0.35f, 0.18f);
}

/** fog の色・密度・高さ・位相パラメータを記録して frame CB を更新する。 */
void CPbrShader::SetFog(FVec3 color, f32 density, f32 height_falloff, f32 height_base,
                        f32 anisotropy, f32 sun_scatter) noexcept {
    if (density < 0.0f) density = 0.0f;
    if (height_falloff < 0.0f) height_falloff = 0.0f;
    if (anisotropy < -0.85f) anisotropy = -0.85f;
    if (anisotropy >  0.85f) anisotropy =  0.85f;
    if (sun_scatter < 0.0f) sun_scatter = 0.0f;
    m_FogColorDensity = FVec4{color.x, color.y, color.z, density};
    m_FogHeightParams = FVec4{height_falloff, height_base, anisotropy, sun_scatter};
    FlushFrameCB();
}

/** probe grid (位置 + SH9) を最大 4 個記録し、残りを 0 埋めして frame CB を更新する。 */
void CPbrShader::SetProbeGrid(const FLightProbe* probes, u32 count) noexcept {
    if (count > 4) count = 4;
    m_ProbeCount = count;
    for (u32 i = 0; i < count; ++i) {
        m_ProbePos[i] = FVec4{probes[i].position.x, probes[i].position.y, probes[i].position.z, 0};
        for (u32 k = 0; k < 9; ++k) {
            m_ProbeSh9[i * 9 + k] = probes[i].sh9[k];
        }
    }
    for (u32 i = count; i < 4; ++i) {
        m_ProbePos[i] = FVec4{0, 0, 0, 0};
        for (u32 k = 0; k < 9; ++k) m_ProbeSh9[i * 9 + k] = FVec4{0, 0, 0, 0};
    }
    FlushFrameCB();
}

/** SH 9 係数を記録して SH9 ambient mode を切り替え、frame CB を更新する。 */
void CPbrShader::SetSh9(const FVec4* sh9_or_null) noexcept {
    if (sh9_or_null) {
        for (u32 i = 0; i < 9; ++i) m_Sh9[i] = sh9_or_null[i];
        m_bSh9Enabled = true;
    } else {
        for (u32 i = 0; i < 9; ++i) m_Sh9[i] = FVec4{0, 0, 0, 0};
        m_bSh9Enabled = false;
    }
    FlushFrameCB();
}

/** IBLと各フレーム入力を、実テクスチャまたは無害な代替テクスチャへ割り当てる。 */
void CPbrShader::BindIblTextures(IRhiCommandList& cmd) noexcept {
    if (m_IblEnabled) {
        cmd.SetTexture(1, *m_IblIrradiance);
        cmd.SetTexture(2, *m_IblPrefilter);
        cmd.SetTexture(3, *m_IblBrdf);
    } else {
        if (m_IblIrradianceFb) cmd.SetTexture(1, *m_IblIrradianceFb);
        if (m_IblPrefilterFb)  cmd.SetTexture(2, *m_IblPrefilterFb);
        if (m_IblBrdfFb)       cmd.SetTexture(3, *m_IblBrdfFb);
    }
    // Normal map: 必ず slot 4 を bind
    if (m_NormalMap) {
        cmd.SetTexture(4, *m_NormalMap);
    } else if (m_NormalMapFb) {
        cmd.SetTexture(4, *m_NormalMapFb);
    }
    // Shadow map: slot 5
    if (m_ShadowDepth) {
        cmd.SetTexture(5, *m_ShadowDepth);
    } else if (m_ShadowFb) {
        cmd.SetTexture(5, *m_ShadowFb);
    }
    // SSAO map: slot 6
    if (m_SsaoTex) {
        cmd.SetTexture(6, *m_SsaoTex);
    } else if (m_SsaoFb) {
        cmd.SetTexture(6, *m_SsaoFb);
    }
    // SSGI color: slot 7
    if (m_SsgiTex) {
        cmd.SetTexture(7, *m_SsgiTex);
    } else if (m_SsgiFb) {
        cmd.SetTexture(7, *m_SsgiFb);
    }
    // Lightmap: slot 8
    if (m_LightmapTex) {
        cmd.SetTexture(8, *m_LightmapTex);
    } else if (m_LightmapFb) {
        cmd.SetTexture(8, *m_LightmapFb);
    }
    // SSR: slot 9
    if (m_SsrTex) {
        cmd.SetTexture(9, *m_SsrTex);
    } else if (m_SsrFb) {
        cmd.SetTexture(9, *m_SsrFb);
    }
    // Aerial perspective volume: slot 10 (3D)。無効時は 1x1x1 fallback (ap_params.x=0 で sample しない)
    if (m_ApVol) {
        cmd.SetTexture(10, *m_ApVol);
    } else if (m_ApFb) {
        cmd.SetTexture(10, *m_ApFb);
    }
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        IRhiTexture* texture = m_SubstrateExpressionTextures[slot];
        if (texture != nullptr) {
            cmd.SetTexture(11u + slot, *texture);
        } else if (m_White) {
            cmd.SetTexture(11u + slot, *m_White);
        }
    }
    if (m_CloudShadowTransmittance) {
        cmd.SetTexture(15u, *m_CloudShadowTransmittance);
    } else if (m_White) {
        cmd.SetTexture(15u, *m_White);
    }
}

/** normal map テクスチャ参照を差し替える。 */
void CPbrShader::SetNormalMap(IRhiTexture* tex, f32 strength) noexcept {
    m_NormalMap = tex;
    m_NormalMapStrength =
        !std::isfinite(strength) ? 1.0f :
        (strength < 0.0f ? 0.0f :
         (strength > 4.0f ? 4.0f : strength));
}

/** SSAO テクスチャ・強度・viewport inv size を記録して frame CB を更新する。 */
void CPbrShader::SetSsao(IRhiTexture* ssao_tex, f32 intensity,
                        u32 viewport_w, u32 viewport_h) noexcept {
    m_SsaoTex       = ssao_tex;
    m_SsaoIntensity = intensity < 0 ? 0.0f : intensity;
    m_SsaoInvW     = (viewport_w > 0) ? (1.0f / static_cast<f32>(viewport_w)) : 0.0f;
    m_SsaoInvH     = (viewport_h > 0) ? (1.0f / static_cast<f32>(viewport_h)) : 0.0f;
    FlushFrameCB();
}

/** SSGI テクスチャと強度を記録して frame CB を更新する。 */
void CPbrShader::SetSsgi(IRhiTexture* ssgi_tex, f32 intensity) noexcept {
    m_SsgiTex       = ssgi_tex;
    m_SsgiIntensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

/** SSR テクスチャと強度を記録して frame CB を更新する。 */
void CPbrShader::SetSsr(IRhiTexture* ssr_tex, f32 intensity) noexcept {
    m_SsrTex       = ssr_tex;
    m_SsrIntensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

/** aerial perspective volume と max_dist を記録して frame CB を更新する。 */
void CPbrShader::SetAerialPerspective(IRhiTexture* ap_vol, f32 max_dist) noexcept {
    m_ApVol    = ap_vol;
    m_ApParams = FVec4{ ap_vol ? 1.0f : 0.0f, max_dist > 0.0f ? max_dist : 1.0f, 0.0f, 0.0f };
    FlushFrameCB();
}

/** lightmap テクスチャと強度を記録して frame CB を更新する。 */
void CPbrShader::SetLightmap(IRhiTexture* lightmap_tex, f32 intensity) noexcept {
    m_LightmapTex       = lightmap_tex;
    m_LightmapIntensity = intensity < 0 ? 0.0f : intensity;
    FlushFrameCB();
}

/** single-cascade 互換で全 cascade に同じ VP を書き、splits を inf にして frame CB を更新する。 */
void CPbrShader::SetShadowMap(IRhiTexture* depth, const FMat4& light_vp,
                              f32 bias, f32 texel_size, f32 filter_radius) noexcept {
    m_ShadowDepth     = depth;
    // 後方互換: 全 cascade スロットに同じ VP を書き、splits を inf にして
    // HLSL の cascade 選択が常に cascade 0 を選ぶようにする。
    // uv_scale = {1, 1, 0, 0} で atlas 変換をパススルー (single texture モード)。
    for (u32 c = 0; c < kMaxShadowCascades; ++c) m_ShadowViewProj[c] = light_vp;
    m_ShadowParams     = FVec4{bias, depth ? 1.0f : 0.0f, texel_size, filter_radius};
    m_CascadeSplits    = FVec4{1e30f, 1e30f, 1e30f, 1e30f};
    m_CascadeUvScale  = FVec4{1.0f, 1.0f, 1.0f, 0};
    FlushFrameCB();
}

/** 各 cascade の VP・split・atlas UV スケールを記録して frame CB を更新する。 */
void CPbrShader::SetShadowMapCascades(IRhiTexture* depth,
                                      const FMat4* light_vp,
                                      const f32*  cascade_splits,
                                      u32 cascade_count,
                                      f32 bias, f32 texel_size,
                                      f32 filter_radius) noexcept {
    m_ShadowDepth = depth;
    if (cascade_count == 0) cascade_count = 1;
    if (cascade_count > kMaxShadowCascades) cascade_count = kMaxShadowCascades;
    for (u32 c = 0; c < cascade_count; ++c) m_ShadowViewProj[c] = light_vp[c];
    // 未使用 slot は cascade_count-1 を再利用 (cascade 選択結果が不正値になっても無害)
    for (u32 c = cascade_count; c < kMaxShadowCascades; ++c)
        m_ShadowViewProj[c] = light_vp[cascade_count - 1];

    // cascade_splits xyzw に 4 cascade の z far を詰め、未使用は inf
    f32 splits[kMaxShadowCascades] = {1e30f, 1e30f, 1e30f, 1e30f};
    for (u32 c = 0; c < cascade_count; ++c) splits[c] = cascade_splits[c];
    m_CascadeSplits = FVec4{splits[0], splits[1], splits[2], splits[3]};

    m_ShadowParams    = FVec4{bias, depth ? 1.0f : 0.0f, texel_size, filter_radius};
    // atlas X scale = 1 / cascade_count (single mode は cascade_count=1 で 1)
    const f32 scale_x = 1.0f / static_cast<f32>(cascade_count);
    m_CascadeUvScale = FVec4{scale_x, 1.0f, static_cast<f32>(cascade_count), 0};
    FlushFrameCB();
}

/** 雲影の透過率地図と、受光点を基準面へ投影する座標を記録する。 */
void CPbrShader::SetCloudShadowMap(const FVolumetricCloudWorldShadowMap& shadow_map) noexcept {
    m_CloudShadowTransmittance = nullptr;
    m_CloudShadowMapParams = FVec4{0, 0, 0, 0};
    m_CloudShadowProjection = FVec4{0, 1, 0, 0};
    m_CloudShadowLayer = FVec4{0, 0, 0.03f, 0};
    m_CloudShadowWorldOrigin = FVec4{0, 0, 0, 0};

    const f32 sunLengthSquared =
        shadow_map.sun_direction.x * shadow_map.sun_direction.x +
        shadow_map.sun_direction.y * shadow_map.sun_direction.y +
        shadow_map.sun_direction.z * shadow_map.sun_direction.z;
    const bool finiteMapping =
        std::isfinite(shadow_map.minimum_reference_xz.x) &&
        std::isfinite(shadow_map.minimum_reference_xz.y) &&
        std::isfinite(shadow_map.inverse_extent) &&
        std::isfinite(shadow_map.reference_height) &&
        std::isfinite(shadow_map.world_origin.x) &&
        std::isfinite(shadow_map.world_origin.y) &&
        std::isfinite(shadow_map.world_origin.z) &&
        std::isfinite(shadow_map.cloud_base_altitude) &&
        std::isfinite(shadow_map.planet_radius) &&
        std::isfinite(sunLengthSquared);
    if (shadow_map.IsValid() && finiteMapping && sunLengthSquared > 1.0e-12f) {
        const f32 inverseSunLength =
            1.0f / static_cast<f32>(std::sqrt(sunLengthSquared));
        const FVec3 sun{
            shadow_map.sun_direction.x * inverseSunLength,
            shadow_map.sun_direction.y * inverseSunLength,
            shadow_map.sun_direction.z * inverseSunLength};
        if (sun.y > 0.03f) {
            m_CloudShadowTransmittance = shadow_map.transmittance;
            m_CloudShadowMapParams = FVec4{
                shadow_map.minimum_reference_xz.x,
                shadow_map.minimum_reference_xz.y,
                shadow_map.inverse_extent,
                1.0f};
            m_CloudShadowProjection = FVec4{
                sun.x, sun.y, sun.z,
                shadow_map.reference_height};
            m_CloudShadowLayer = FVec4{
                shadow_map.cloud_base_altitude,
                1.0f / static_cast<f32>(shadow_map.resolution),
                0.03f, shadow_map.planet_radius};
            m_CloudShadowWorldOrigin = FVec4{
                shadow_map.world_origin.x,
                shadow_map.world_origin.y,
                shadow_map.world_origin.z,
                0.0f};
        }
    }
    FlushFrameCB();
}

/** カメラ・環境光・有向光源を記録して frame CB を更新する。 */
void CPbrShader::SetLights(const FMat4& vp, FVec3 eye,
                          const FDirLight* lights, u32 count,
                          FVec3 ambient) noexcept {
    m_Vp = vp;
    m_Eye = eye;
    m_Ambient = ambient;
    if (count > kMaxDirLights) count = kMaxDirLights;
    m_DirCount = count;
    for (u32 i = 0; i < count; ++i) m_DirLights[i] = lights[i];
    FlushFrameCB();
}

/** 点光源を記録して frame CB を更新する。 */
void CPbrShader::SetPointLights(const FPointLight* lights, u32 count) noexcept {
    if (count > kMaxPointLights) count = kMaxPointLights;
    m_PointCount = count;
    for (u32 i = 0; i < count; ++i) m_PointLights[i] = lights[i];
    FlushFrameCB();
}

/** 矩形 area light を記録して frame CB を更新する。 */
void CPbrShader::SetAreaLights(const FAreaLight* lights, u32 count) noexcept {
    if (count > kMaxAreaLights) count = kMaxAreaLights;
    m_AreaCount = count;
    for (u32 i = 0; i < count; ++i) m_AreaLights[i] = lights[i];
    FlushFrameCB();
}

/** 全 member 値から FrameCBLayout を構築して frame CB に書き込む。 */
void CPbrShader::FlushFrameCB() noexcept {
    if (!m_FrameCb) return;
    FFrameCBLayout cb{};
    cb.view_proj  = m_Vp;
    cb.camera_pos = FVec4{m_Eye.x, m_Eye.y, m_Eye.z, 1.0f};
    cb.ambient    = FVec4{m_Ambient.x, m_Ambient.y, m_Ambient.z, static_cast<f32>(m_DirCount)};
    cb.point_count_pad = FVec4{static_cast<f32>(m_PointCount), static_cast<f32>(m_AreaCount), 0, 0};
    for (u32 i = 0; i < m_DirCount; ++i) {
        const FVec3& d = m_DirLights[i].direction;
        const FVec3& c = m_DirLights[i].color;
        cb.light_dir[i]   = FVec4{d.x, d.y, d.z, 0};
        cb.light_color[i] = FVec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < m_PointCount; ++i) {
        const FVec3& p = m_PointLights[i].position;
        const FVec3& c = m_PointLights[i].color;
        cb.point_pos_range[i] = FVec4{p.x, p.y, p.z, m_PointLights[i].range};
        cb.point_color[i]     = FVec4{c.x, c.y, c.z, 1};
    }
    for (u32 i = 0; i < m_AreaCount; ++i) {
        const FAreaLight& a = m_AreaLights[i];
        cb.area_center[i] = FVec4{a.center.x, a.center.y, a.center.z, 0};
        cb.area_axis_x[i] = FVec4{a.axis_x.x, a.axis_x.y, a.axis_x.z, 0};
        cb.area_axis_y[i] = FVec4{a.axis_y.x, a.axis_y.y, a.axis_y.z, 0};
        cb.area_color[i]  = FVec4{a.color.x,  a.color.y,  a.color.z,  0};
    }
    cb.probe_params = FVec4{static_cast<f32>(m_ProbeCount), 0, 0, 0};
    for (u32 i = 0; i < 4; ++i) cb.probe_pos[i] = m_ProbePos[i];
    for (u32 i = 0; i < 4 * 9; ++i) cb.probe_sh9[i] = m_ProbeSh9[i];
    cb.fog_color_density = m_FogColorDensity;
    cb.fog_height_params = m_FogHeightParams;
    for (u32 c = 0; c < kMaxShadowCascades; ++c)
        cb.shadow_view_proj[c] = m_ShadowViewProj[c];
    cb.shadow_params     = m_ShadowParams;
    cb.cascade_splits    = m_CascadeSplits;
    cb.cascade_uv_scale  = m_CascadeUvScale;
    cb.ibl_params = FVec4{
        m_IblEnabled ? 1.0f : 0.0f,
        static_cast<f32>(m_IblMips),
        m_bSh9Enabled ? 1.0f : 0.0f,
        m_IblLightMultiplier
    };
    cb.ssao_params = FVec4{
        (m_SsaoTex && m_SsaoInvW > 0 && m_SsaoInvH > 0) ? 1.0f : 0.0f,
        m_SsaoIntensity,
        m_SsaoInvW,
        m_SsaoInvH
    };
    cb.ssgi_params = FVec4{
        m_SsgiTex ? 1.0f : 0.0f,
        m_SsgiIntensity,
        0, 0
    };
    cb.lightmap_params = FVec4{
        m_LightmapTex ? 1.0f : 0.0f,
        m_LightmapIntensity,
        0, 0
    };
    cb.ssr_params = FVec4{
        m_SsrTex ? 1.0f : 0.0f,
        m_SsrIntensity,
        0, 0
    };
    cb.ap_params = m_ApParams;
    cb.cloud_shadow_map_params = m_CloudShadowMapParams;
    cb.cloud_shadow_projection = m_CloudShadowProjection;
    cb.cloud_shadow_layer = m_CloudShadowLayer;
    cb.cloud_shadow_world_origin = m_CloudShadowWorldOrigin;
    for (u32 i = 0; i < 9; ++i) cb.sh9[i] = m_Sh9[i];
    m_FrameCb->Update(&cb, sizeof(cb));
}

/** model と PBR/拡張パラメータから ObjectCBLayout を構築して object CB に書き込む。 */
void CPbrShader::SetObject(const FMat4& model, FVec3 base_color,
                          f32 metallic, f32 roughness, f32 ao) noexcept {
    FObjectCbLayout cb{};
    cb.model = model;
    const FMat4 normal_matrix = MakeSafeNormalMatrix(model);
    cb.normal_row0 = FVec4{normal_matrix.m[0][0], normal_matrix.m[0][1],
                           normal_matrix.m[0][2], 0};
    cb.normal_row1 = FVec4{normal_matrix.m[1][0], normal_matrix.m[1][1],
                           normal_matrix.m[1][2], 0};
    cb.normal_row2 = FVec4{normal_matrix.m[2][0], normal_matrix.m[2][1],
                           normal_matrix.m[2][2], 0};
    const bool substrate_enabled = m_SubstrateSecondary.w > 0.5f;
    cb.base_color = substrate_enabled
        ? m_SubstrateDiffuseCoverage
        : FVec4{base_color.x, base_color.y, base_color.z, 1.0f};
    cb.pbr_params = FVec4{
        substrate_enabled ? 0.0f : metallic,
        roughness,
        ao,
        m_NormalMap != nullptr
            ? m_NormalMapStrength
            : 0.0f};
    cb.ext_params    = m_ExtParams;
    cb.aniso_tangent = m_AnisoTangent;
    cb.emissive      = m_Emissive;
    cb.sheen_params  = m_SheenParams;
    cb.sheen_rough   = m_SheenRough;
    cb.irid_params   = m_IridParams;
    cb.sss_params    = m_SssParams;
    cb.substrate_f0 = m_SubstrateF0;
    cb.substrate_f90 = m_SubstrateF90;
    cb.substrate_diffuse_coverage = m_SubstrateDiffuseCoverage;
    cb.substrate_secondary = m_SubstrateSecondary;
    cb.substrate_mfp_thickness = m_SubstrateMfpThickness;
    cb.substrate_transmittance = m_SubstrateTransmittance;
    cb.substrate_normal = m_SubstrateNormal;
    cb.substrate_coat_f0 = m_SubstrateCoatF0;
    if (m_SubstrateExpressionInstructionCount > 0u) {
        std::memcpy(
            cb.substrate_expr_instructions,
            m_SubstrateExpressionInstructions,
            sizeof(FShaderExpressionInstruction) *
                m_SubstrateExpressionInstructionCount);
    }
    if (m_SubstrateExpressionBindingCount > 0u) {
        std::memcpy(
            cb.substrate_expr_bindings,
            m_SubstrateExpressionBindings,
            sizeof(FSubstrateExpressionBinding) *
                m_SubstrateExpressionBindingCount);
    }
    for (u32 i = 0u;
         i < m_SubstrateExpressionParameterCount;
         ++i) {
        cb.substrate_expr_parameter_meta[i][0] =
            m_SubstrateExpressionParameters[i].id;
        cb.substrate_expr_parameter_meta[i][1] = static_cast<u32>(
            m_SubstrateExpressionParameters[i].type);
        cb.substrate_expr_parameter_values[i] = FVec4{
            m_SubstrateExpressionParameters[i].value.x,
            m_SubstrateExpressionParameters[i].value.y,
            m_SubstrateExpressionParameters[i].value.z,
            m_SubstrateExpressionParameters[i].value.w};
    }
    cb.substrate_expr_meta[0] =
        m_SubstrateExpressionInstructionCount;
    cb.substrate_expr_meta[1] =
        m_SubstrateExpressionBindingCount;
    cb.substrate_expr_meta[2] =
        m_SubstrateExpressionParameterCount;
    cb.substrate_expr_meta[3] =
        m_SubstrateExpressionTextureMask;
    cb.substrate_expr_context =
        FVec4{m_SubstrateExpressionTime, 0, 0, 0};
    /** 今回の定数を保持する論理slice。 */
    IRhiBuffer* const object_cb = m_ObjectArena.Upload(&cb, sizeof(cb));
    if (object_cb == nullptr) {
        if (!m_ObjectCapacityFailureLogged) {
            ACS_LOG_WARN("CPbrShader: unable to grow shared object upload arena (required=%u, retained=%u); remaining PBR draws are skipped", m_ObjectCbCursor == kInvalidObjectBuffer ? kInvalidObjectBuffer : m_ObjectCbCursor + 1u, m_ObjectArena.Capacity());
            m_ObjectCapacityFailureLogged = true;
        }
        m_CurrentObjectCb = kInvalidObjectBuffer;
        return;
    }
    m_ObjectCbCursor = m_ObjectArena.Used();
    m_CurrentObjectCb = m_ObjectCbCursor - 1u;
}

/** clearcoat/anisotropy パラメータを member に格納する (次の SetObject で反映)。 */
void CPbrShader::SetExtParams(f32 clearcoat, f32 clearcoat_roughness,
                             f32 anisotropy, FVec3 tangent) noexcept {
    m_ExtParams    = FVec4{clearcoat, clearcoat_roughness, anisotropy, 0};
    m_AnisoTangent = FVec4{tangent.x, tangent.y, tangent.z, 0};
    // 注: SetObject が CB を flush するので、SetExtParams 単独では反映されない。
    // SetObject 直後に呼んでも次の SetObject で 上書き されない (member に格納)。
    // 描画前に SetObject() が再度呼ばれて反映される設計。
}

/** emissive (color*strength) を member に格納する (次の SetObject で反映)。 */
void CPbrShader::SetEmissive(FVec3 color, f32 strength) noexcept {
    const f32 s = strength < 0.0f ? 0.0f : strength;
    m_Emissive = FVec4{color.x * s, color.y * s, color.z * s, 0.0f};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

/** sheen パラメータを [0,1] にクランプして member に格納する (次の SetObject で反映)。 */
void CPbrShader::SetSheen(FVec3 sheen_color, f32 weight, f32 roughness) noexcept {
    // weight は [0,1] の blend 係数、sheen_color は反射率なので各 ch を [0,1] に収める。
    // これでシェーダの energy 減衰係数 (1 - weight*maxC*0.5) が負へ振れない。
    auto sat01 = [](f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    const f32 w = sat01(weight);
    const f32 r = roughness < 0.04f ? 0.04f : roughness;
    m_SheenParams = FVec4{sat01(sheen_color.x), sat01(sheen_color.y), sat01(sheen_color.z), w};
    m_SheenRough  = FVec4{r, 0, 0, 0};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

/** iridescence パラメータを物理範囲にクランプして member に格納する (次の SetObject で反映)。 */
void CPbrShader::SetIridescence(f32 weight, f32 thickness_nm, f32 film_ior) noexcept {
    // weight は [0,1] の blend 係数。thickness は非負、film_ior は物理的に >= 1。
    const f32 w = weight < 0.0f ? 0.0f : (weight > 1.0f ? 1.0f : weight);
    const f32 t = thickness_nm < 0.0f ? 0.0f : thickness_nm;
    const f32 i = film_ior < 1.0f ? 1.0f : film_ior;
    m_IridParams = FVec4{w, t, i, 0};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

/** subsurface パラメータを [0,1] にクランプして member に格納する (次の SetObject で反映)。 */
void CPbrShader::SetSubsurface(FVec3 sss_color, f32 weight) noexcept {
    // weight は [0,1] の blend 係数、sss_color は内部散乱の色 (各 ch [0,1])。
    auto sat01 = [](f32 v) noexcept { return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v); };
    m_SssParams = FVec4{sat01(sss_color.x), sat01(sss_color.y), sat01(sss_color.z),
                       sat01(weight)};
    // SetExtParams と同じく member 格納。次の SetObject / DrawMesh が CB に反映する。
}

void CPbrShader::SetSubstrateSurface(
    const FSubstrateResolvedSurface& surface) noexcept {
    // SetSubstrateSurface is also a public static-surface path.  It must not
    // inherit the previous material's per-pixel program or texture bindings.
    // SetSubstrateMaterial calls this first and installs its freshly compiled
    // program immediately afterwards.
    m_SubstrateExpressionInstructionCount = 0u;
    m_SubstrateExpressionBindingCount = 0u;
    m_SubstrateExpressionParameterCount = 0u;
    m_SubstrateExpressionTextureMask = 0u;
    m_SubstrateExpressionTime = 0.0f;
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        m_SubstrateExpressionTextures[slot] = nullptr;
    }
    auto sat = [](f32 v) noexcept {
        return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
    };
    m_SubstrateF0 = FVec4{sat(surface.f0.x), sat(surface.f0.y),
                          sat(surface.f0.z), 0};
    m_SubstrateF90 = FVec4{sat(surface.f90.x), sat(surface.f90.y),
                           sat(surface.f90.z), 0};
    m_SubstrateDiffuseCoverage = FVec4{
        sat(surface.diffuse_albedo.x), sat(surface.diffuse_albedo.y),
        sat(surface.diffuse_albedo.z), sat(surface.coverage)};
    m_SubstrateSecondary = FVec4{
        sat(surface.second_roughness), sat(surface.second_roughness_weight),
        surface.phase_anisotropy < -0.99f ? -0.99f :
            (surface.phase_anisotropy > 0.99f ? 0.99f : surface.phase_anisotropy),
        1.0f};
    m_SubstrateMfpThickness = FVec4{
        surface.mean_free_path_cm.x < 0 ? 0 : surface.mean_free_path_cm.x,
        surface.mean_free_path_cm.y < 0 ? 0 : surface.mean_free_path_cm.y,
        surface.mean_free_path_cm.z < 0 ? 0 : surface.mean_free_path_cm.z,
        surface.thickness_cm < 0 ? 0 : surface.thickness_cm};
    m_SubstrateTransmittance = FVec4{
        sat(surface.transmittance.x), sat(surface.transmittance.y),
        sat(surface.transmittance.z), sat(surface.coverage)};
    m_SubstrateNormal = FVec4{surface.normal.x, surface.normal.y, surface.normal.z,
                              surface.normal_strength < 0 ? 0 :
                              (surface.normal_strength > 4 ? 4 : surface.normal_strength)};
    m_ExtParams = FVec4{sat(surface.coat_weight), sat(surface.coat_roughness),
                        surface.anisotropy, 0};
    m_SubstrateCoatF0 = FVec4{sat(surface.coat_f0.x), sat(surface.coat_f0.y),
                              sat(surface.coat_f0.z), 0};
    m_AnisoTangent = FVec4{surface.tangent.x, surface.tangent.y, surface.tangent.z, 0};
    m_Emissive = FVec4{surface.emissive.x, surface.emissive.y, surface.emissive.z, 0};
    m_SheenParams = FVec4{sat(surface.fuzz_color.x), sat(surface.fuzz_color.y),
                          sat(surface.fuzz_color.z), sat(surface.fuzz_amount)};
    m_SheenRough = FVec4{sat(surface.fuzz_roughness), 0, 0, 0};
    m_IridParams = FVec4{sat(surface.thin_film_weight),
                         surface.thin_film_thickness_nm < 0
                             ? 0 : surface.thin_film_thickness_nm,
                         surface.thin_film_ior < 1 ? 1 : surface.thin_film_ior, 0};
    const f32 max_mfp = surface.mean_free_path_cm.x > surface.mean_free_path_cm.y
        ? (surface.mean_free_path_cm.x > surface.mean_free_path_cm.z
            ? surface.mean_free_path_cm.x : surface.mean_free_path_cm.z)
        : (surface.mean_free_path_cm.y > surface.mean_free_path_cm.z
            ? surface.mean_free_path_cm.y : surface.mean_free_path_cm.z);
    const f32 sss_weight = sat(max_mfp);
    m_SssParams = FVec4{sat(surface.diffuse_albedo.x), sat(surface.diffuse_albedo.y),
                        sat(surface.diffuse_albedo.z), sss_weight};
}

bool CPbrShader::SetSubstrateMaterial(
    const FSubstrateMaterial& material,
    f32 time_seconds) noexcept {
    FSubstrateResolvedSurface surface{};
    if (!material.enabled ||
        !ResolveSubstrateMaterial(material, surface, nullptr)) {
        ClearSubstrateSurface();
        return false;
    }
    const FSubstrateExpressionLinkResult linked =
        CompileSubstrateExpressionLinks(material);
    if (!linked.Succeeded() ||
        linked.binding_count > kSubstrateSlabScalarCount) {
        ClearSubstrateSurface();
        return false;
    }

    SetSubstrateSurface(surface);
    m_SubstrateExpressionInstructionCount = 0u;
    m_SubstrateExpressionBindingCount = 0u;
    m_SubstrateExpressionParameterCount = 0u;
    m_SubstrateExpressionTextureMask = 0u;
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        m_SubstrateExpressionTextures[slot] = nullptr;
    }
    m_SubstrateExpressionTime =
        std::isfinite(time_seconds) ? time_seconds : 0.0f;

    // An authored but unreferenced expression graph has zero per-pixel cost.
    if (linked.binding_count == 0u) return true;
    if (linked.expression_program.instruction_count == 0u ||
        linked.expression_program.instruction_count >
            kShaderExpressionMaxNodes) {
        ClearSubstrateSurface();
        return false;
    }
    m_SubstrateExpressionInstructionCount =
        linked.expression_program.instruction_count;
    m_SubstrateExpressionBindingCount = linked.binding_count;
    std::memcpy(
        m_SubstrateExpressionInstructions,
        linked.expression_program.instructions,
        sizeof(FShaderExpressionInstruction) *
            m_SubstrateExpressionInstructionCount);
    std::memcpy(
        m_SubstrateExpressionBindings,
        linked.bindings,
        sizeof(FSubstrateExpressionBinding) *
            m_SubstrateExpressionBindingCount);
    return true;
}

void CPbrShader::SetSubstrateExpressionParameters(
    const FShaderExpressionParameter* parameters,
    u32 count) noexcept {
    m_SubstrateExpressionParameterCount = 0u;
    if (parameters == nullptr) return;
    if (count > kShaderExpressionMaxParameters) {
        count = kShaderExpressionMaxParameters;
    }
    for (u32 i = 0u; i < count; ++i) {
        const FShaderExpressionParameter& source = parameters[i];
        const u32 width = static_cast<u32>(source.type);
        if (width < 1u || width > 4u ||
            !std::isfinite(source.value.x) ||
            !std::isfinite(source.value.y) ||
            !std::isfinite(source.value.z) ||
            !std::isfinite(source.value.w)) {
            continue;
        }
        FShaderExpressionParameter& destination =
            m_SubstrateExpressionParameters[
                m_SubstrateExpressionParameterCount++];
        destination = source;
        if (width < 4u) destination.value.w = 0.0f;
        if (width < 3u) destination.value.z = 0.0f;
        if (width < 2u) destination.value.y = 0.0f;
    }
}

void CPbrShader::SetSubstrateExpressionTime(f32 time_seconds) noexcept {
    m_SubstrateExpressionTime =
        std::isfinite(time_seconds) ? time_seconds : 0.0f;
}

void CPbrShader::SetSubstrateExpressionTexture(
    u32 slot, IRhiTexture* texture) noexcept {
    if (slot >= kShaderExpressionMaxTextureSlots) return;
    m_SubstrateExpressionTextures[slot] = texture;
    const u32 bit = u32{1} << slot;
    if (texture != nullptr) {
        m_SubstrateExpressionTextureMask |= bit;
    } else {
        m_SubstrateExpressionTextureMask &= ~bit;
    }
}

void CPbrShader::ClearSubstrateSurface() noexcept {
    m_SubstrateSecondary.w = 0.0f;
    m_SubstrateExpressionInstructionCount = 0u;
    m_SubstrateExpressionBindingCount = 0u;
    m_SubstrateExpressionParameterCount = 0u;
    m_SubstrateExpressionTextureMask = 0u;
    m_SubstrateExpressionTime = 0.0f;
    for (u32 slot = 0u;
         slot < kShaderExpressionMaxTextureSlots;
         ++slot) {
        m_SubstrateExpressionTextures[slot] = nullptr;
    }
}

/** SetObject + パイプライン/CB/テクスチャ/VB/IB bind + DrawIndexed をまとめて発行する。 */
bool CPbrShader::DrawMesh(IRhiCommandList& cmd, const FGpuMesh& mesh, const FMat4& model,
                         FVec3 base_color, f32 metallic, f32 roughness, f32 ao,
                         IRhiTexture* albedo) noexcept {
    if (!m_Pipeline || !m_FrameCb || m_ObjectArena.Capacity() == 0u || !m_ObjectArena.Get(0u) || !mesh.vertex_buffer || !mesh.index_buffer) {
        return false;
    }
    SetObject(model, base_color, metallic, roughness, ao);   // リングを次へ進め、現在バッファへ書込む
    IRhiBuffer* object_cb = PerObjectCB();
    if (!object_cb) return false;
    cmd.SetPipeline(*m_Pipeline);
    cmd.SetConstantBuffer(0, *m_FrameCb);
    cmd.SetConstantBuffer(1, *object_cb);                     // SetObject が書いた «現在» のリングバッファ
    cmd.SetTexture(0, *(albedo ? albedo : m_White.Get()));
    BindIblTextures(cmd);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
    return true;
}

bool CPbrShader::DrawMeshSubsurfaceMrt(IRhiCommandList& cmd, const FGpuMesh& mesh, const FMat4& model, FVec3 base_color, f32 metallic, f32 roughness, f32 ao, IRhiTexture* albedo) noexcept {
    if (!m_SubsurfaceMrtPipeline || !m_FrameCb || m_ObjectArena.Capacity() == 0u || !m_ObjectArena.Get(0u) || !mesh.vertex_buffer || !mesh.index_buffer) {
        return false;
    }
    SetObject(model, base_color, metallic, roughness, ao);
    IRhiBuffer* object_cb = PerObjectCB();
    if (!object_cb) return false;
    cmd.SetPipeline(*m_SubsurfaceMrtPipeline);
    cmd.SetConstantBuffer(0, *m_FrameCb);
    cmd.SetConstantBuffer(1, *object_cb);
    cmd.SetTexture(0, *(albedo ? albedo : m_White.Get()));
    BindIblTextures(cmd);
    cmd.SetVertexBuffer(*mesh.vertex_buffer, mesh.vertex_stride);
    cmd.SetIndexBuffer(*mesh.index_buffer);
    cmd.DrawIndexed(mesh.index_count);
    return true;
}

} // namespace acs
