// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ実装
#include "render/Sky.h"
#include "render/VolumetricCloudAmbientCacheInternal.h"
#include "render/VolumetricCloudDensityIntegrationInternal.h"
#include "render/VolumetricCloudRayMarchInternal.h"
#include "render/VolumetricCloudTemporalInternal.h"
#include "render/Atmosphere.h"
#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif
#include "math/Camera.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstddef>
#include <cmath>
#include <cstring>
#include <limits>

namespace acs {

namespace {

/** 外殻の接線候補だけを救済する、二進32bit係数演算八回分の相対判別式許容差。 */
constexpr f32 kCloudShellDiscriminantRelativeToleranceInternal =
    9.5367431640625e-7f;

/** スカイの HLSL ソース (フルスクリーン三角形 + 視線方向ベースの空と太陽。VB 不要)。 */
const char* kSkyHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer CSky : register(b0) {
    float4x4 inv_view_proj;       // 画面 NDC → ワールドへの逆変換
    float4   camera_pos;          // xyz=eye
    float4   sun_dir;             // xyz=方向 (camera→sun)
    float4   sun_color;           // xyz=色
    // x=radius (1-cos), y=glow, z=出力ディザ強度, w=予約
    float4   sun_params;
    float4   zenith_color;
    float4   horizon_color;
    float4   ground_color;
    float4   cloud_params0;       // x=coverage(0..1), y=density(sharpness), z=time, w=enabled(0/1)
    float4   cloud_params1;       // xyz=cloud_color, w=wind_speed
    float4   physical_params;     // x=有効状態, y=視点高度(km), z/w=予約
    float4   physical_sun_intensity; // xyz=太陽放射輝度, w=予約
    float4   physical_ground_albedo; // xyz=地表アルベド, w=予約
};

struct VSOut {
    float4 pos : SV_POSITION;
    float2 ndc : TEXCOORD0;       // -1..+1 の 2D NDC
};

float3 CameraRelativeViewDirection(float2 ndc) {
    // 遠点が無限遠になって w=0 でも、同次座標の xyz は透視投影の視線を保持する。
    float4 farHomogeneous=mul(float4(ndc,1.0,1.0),inv_view_proj);
    bool perspective=abs(inv_view_proj[2][3])>1.0e-7;
    float3 candidate=perspective
        ?farHomogeneous.xyz
        :mul(float4(0.0,0.0,1.0,0.0),inv_view_proj).xyz;
    float lengthSquared=dot(candidate,candidate);
    return lengthSquared>1.0e-12&&lengthSquared<3.0e38
        ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}

VSOut VSMain(uint id : SV_VertexID) {
    // 大きな三角形を 1 枚張ってフルスクリーンを覆う
    float2 uv = float2((id << 1) & 2, id & 2);
    VSOut o;
    o.ndc = uv * 2.0 - 1.0;
    // D3D の Y は下が +、ndc.y を反転して上から下に統一
    o.pos = float4(o.ndc.x, -o.ndc.y, 1.0, 1.0);    // z=1 (far)
    return o;
}

// ---- 雲用の value noise + FBM ----------------------------------------------
// 軽量なハッシュ value noise を 6 octave 重ね、octave ごとに座標を回転させて
// 軸に揃ったタイリングを崩す。gradient/simplex より単純だが、回転 + 多 octave で
// 十分に有機的な雲になる。
float Hash2(float2 p) {
    p = frac(p * float2(127.1, 311.7));
    p += dot(p, p + 34.23);
    return frac(p.x * p.y);
}
float ValueNoise(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    float2 u = f * f * (3.0 - 2.0 * f);          // smoothstep 補間で格子段差を消す
    float a = Hash2(i + float2(0.0, 0.0));
    float b = Hash2(i + float2(1.0, 0.0));
    float c = Hash2(i + float2(0.0, 1.0));
    float d = Hash2(i + float2(1.0, 1.0));
    return lerp(lerp(a, b, u.x), lerp(c, d, u.x), u.y);
}
float Fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    const float2x2 rot = float2x2(0.80, -0.60, 0.60, 0.80);   // ~37deg 回転
    [unroll]
    for (int i = 0; i < 6; ++i) {
        sum += amp * ValueNoise(p);
        p    = mul(rot, p) * 2.02;               // lacunarity ~2 + 回転
        amp *= 0.5;                              // gain 0.5
    }
    return sum;
}

// ---- volumetric clouds 用 3D value noise + FBM (レイマーチ立体雲) ------------
float Hash3(float3 p) {
    p = frac(p * 0.3183099 + float3(0.1, 0.2, 0.3));
    p *= 17.0;
    return frac(p.x * p.y * p.z * (p.x + p.y + p.z));
}
float ValueNoise3(float3 p) {
    float3 i = floor(p);
    float3 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);                          // smoothstep 補間
    float n000 = Hash3(i + float3(0,0,0)), n100 = Hash3(i + float3(1,0,0));
    float n010 = Hash3(i + float3(0,1,0)), n110 = Hash3(i + float3(1,1,0));
    float n001 = Hash3(i + float3(0,0,1)), n101 = Hash3(i + float3(1,0,1));
    float n011 = Hash3(i + float3(0,1,1)), n111 = Hash3(i + float3(1,1,1));
    float nx00 = lerp(n000, n100, f.x), nx10 = lerp(n010, n110, f.x);
    float nx01 = lerp(n001, n101, f.x), nx11 = lerp(n011, n111, f.x);
    return lerp(lerp(nx00, nx10, f.y), lerp(nx01, nx11, f.y), f.z);
}
float Fbm3(float3 p) {
    float sum = 0.0, amp = 0.5;
    [loop] for (int i = 0; i < 5; ++i) { sum += amp * ValueNoise3(p); p *= 2.03; amp *= 0.5; }
    return sum;
}
// 雲スラブ内の点 p の密度 (0..1)。高周波 base + 高度プロファイル + 多段 detail 侵食で «くっきりした»
// 立体雲に (低周波の大きい塊だとボケて低解像度に見えるため、周波数とコントラストを上げる)。
float CloudDensity3(float3 p, float coverage, float windOff) {
    // Keep one fully initialized return value.  Some FXC/DXC backend versions
    // failed to prove the former early-return form initialized the inlined
    // call result and emitted real undefined-density speckles on the fallback
    // sky path while volumetric pipelines were warming up.
    float result = 0.0;
    // 高度プロファイル: スラブ中央で濃く上下で薄い → 丸みのある雲塊 (cumulus)。p.y はスラブ高度。
    float hf = saturate((p.y - 1.0) / 1.6);
    float profile = smoothstep(0.0, 0.22, hf) * smoothstep(1.0, 0.5, hf);
    if (profile > 0.001) {
        float3 q = p * 2.4 + float3(windOff, windOff * 0.2, windOff * 0.6);   // 高周波化 → 雲の数とディテール増
        float base  = Fbm3(q);
        float shape = saturate((base - (1.0 - coverage)) /
                               max(coverage, 0.001)) * profile;
        if (shape > 0.0) {
            // 2 段の高周波 detail で縁を侵食 → もこもこ + wispy なディテール
            float d1 = Fbm3(q * 3.0 + 11.3);
            float d2 = Fbm3(q * 8.0 + 27.1);
            float d  = saturate(
                shape - (1.0 - shape) * (d1 * 0.45 + d2 * 0.25));
            result = d * d * (3.0 - 2.0 * d);
        }
    }
    return result;                                         // smoothstep でコントラスト強調 (くっきり)
}

// ---- 物理大気 fallback ----------------------------------------------------
// 大気表を作れない raw DX12 でも、CSky の固定色へ戻さず同じ物理量を積分する。
// 単位はGPU大気表と同じ km。地表半径・上端半径・密度高さ・散乱係数は
// render/Atmosphere.cpp の共通大気モデルと一致させる。
static const float kPhysicalGroundRadiusKm = 6360.0;
static const float kPhysicalTopRadiusKm = 6460.0;
static const float3 kPhysicalRayleighBeta = float3(5.802, 13.558, 33.1) * 0.001;
static const float kPhysicalMieBeta = 3.996 * 0.001;
static const float kPhysicalMieAbsorption = 4.4 * 0.001;
static const float3 kPhysicalOzoneAbsorption = float3(0.650, 1.881, 0.085) * 0.001;
static const float kPhysicalRayleighScaleHeightKm = 8.0;
static const float kPhysicalMieScaleHeightKm = 1.2;
static const float kPhysicalMieAnisotropy = 0.8;

float PhysicalRayleighPhase(float cos_theta) {
    return 3.0 / (16.0 * 3.14159265) * (1.0 + cos_theta * cos_theta);
}

float PhysicalMiePhase(float cos_theta) {
    float g = kPhysicalMieAnisotropy;
    float g2 = g * g;
    float denominator = max(1.0 + g2 - 2.0 * g * cos_theta, 1.0e-4);
    return (1.0 - g2) /
        (4.0 * 3.14159265 * pow(denominator, 1.5));
}

float PhysicalRaySphereOuter(float3 origin, float3 direction, float radius) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    return discriminant >= 0.0
        ? -b + sqrt(discriminant) : -1.0;
}

float PhysicalRaySphereNear(float3 origin, float3 direction, float radius) {
    float b = dot(origin, direction);
    float c = dot(origin, origin) - radius * radius;
    float discriminant = b * b - c;
    if (discriminant < 0.0) return -1.0;
    float distance = -b - sqrt(discriminant);
    return distance > 0.0 ? distance : -1.0;
}

void SamplePhysicalMedium(float altitude_km, out float3 rayleigh_density,
                          out float mie_density, out float3 extinction) {
    float safe_altitude = max(altitude_km, 0.0);
    rayleigh_density = exp(-safe_altitude / kPhysicalRayleighScaleHeightKm);
    mie_density = exp(-safe_altitude / kPhysicalMieScaleHeightKm);
    float ozone = saturate(1.0 - abs(safe_altitude - 25.0) / 15.0);
    extinction = kPhysicalRayleighBeta * rayleigh_density
        + (kPhysicalMieBeta + kPhysicalMieAbsorption) * mie_density
        + kPhysicalOzoneAbsorption * ozone;
}

float3 PhysicalTransmittance(float3 origin, float3 direction,
                             float distance) {
    if (distance <= 0.0) return float3(1.0, 1.0, 1.0);
    // 太陽光線が地表球へ入る場合は、地球の内部を透過させず遮蔽する。
    float ground_distance = PhysicalRaySphereNear(
        origin, direction, kPhysicalGroundRadiusKm);
    if (ground_distance > 0.0 && ground_distance < distance)
        return float3(0.0, 0.0, 0.0);
    const int kTransmittanceSteps = 8;
    float step_length = distance / float(kTransmittanceSteps);
    float3 optical_depth = float3(0.0, 0.0, 0.0);
    [loop]
    for (int i = 0; i < kTransmittanceSteps; ++i) {
        float3 sample_position = origin + direction *
            (step_length * (float(i) + 0.5));
        float altitude = length(sample_position) - kPhysicalGroundRadiusKm;
        float3 rayleigh_density;
        float mie_density;
        float3 extinction;
        SamplePhysicalMedium(altitude, rayleigh_density, mie_density, extinction);
        optical_depth += extinction * step_length;
    }
    return exp(-optical_depth);
}

float3 EvaluatePhysicalSky(float3 view_direction, float3 sun_direction,
                           float3 sun_intensity, float altitude_km,
                           float3 ground_albedo) {
    float3 origin = float3(0.0, kPhysicalGroundRadiusKm + max(altitude_km, 0.0), 0.0);
    float top_distance = PhysicalRaySphereOuter(
        origin, view_direction, kPhysicalTopRadiusKm);
    if (top_distance <= 0.0) return float3(0.0, 0.0, 0.0);

    float ground_distance = PhysicalRaySphereNear(
        origin, view_direction, kPhysicalGroundRadiusKm);
    bool hits_ground = ground_distance > 0.0 && ground_distance < top_distance;
    float ray_distance = hits_ground ? ground_distance : top_distance;
    const int kViewSteps = 16;
    float step_length = ray_distance / float(kViewSteps);
    float3 view_transmittance = float3(1.0, 1.0, 1.0);
    float3 radiance = float3(0.0, 0.0, 0.0);
    float cos_view_sun = dot(view_direction, sun_direction);
    float rayleigh_phase = PhysicalRayleighPhase(cos_view_sun);
    float mie_phase = PhysicalMiePhase(cos_view_sun);

    [loop]
    for (int i = 0; i < kViewSteps; ++i) {
        float3 sample_position = origin + view_direction *
            (step_length * (float(i) + 0.5));
        float altitude = length(sample_position) - kPhysicalGroundRadiusKm;
        if (altitude < 0.0) continue;

        float3 rayleigh_density;
        float mie_density;
        float3 extinction;
        SamplePhysicalMedium(altitude, rayleigh_density, mie_density, extinction);
        float sun_distance = PhysicalRaySphereOuter(
            sample_position, sun_direction, kPhysicalTopRadiusKm);
        float3 sun_transmittance = PhysicalTransmittance(
            sample_position, sun_direction, sun_distance);
        float3 source = sun_intensity * sun_transmittance * (
            kPhysicalRayleighBeta * rayleigh_density * rayleigh_phase
            + kPhysicalMieBeta * mie_density * mie_phase);
        float3 segment_tau = extinction * step_length;
        float3 segment_transfer = (1.0 - exp(-segment_tau)) /
            max(segment_tau, 1.0e-6);
        radiance += view_transmittance * source * step_length * segment_transfer;
        view_transmittance *= exp(-segment_tau);
    }

    if (hits_ground) {
        float3 ground_position = origin + view_direction * ground_distance;
        float3 ground_normal = normalize(ground_position);
        float sun_cosine = max(dot(ground_normal, sun_direction), 0.0);
        float sun_distance = PhysicalRaySphereOuter(
            ground_position + ground_normal * 0.001,
            sun_direction, kPhysicalTopRadiusKm);
        float3 sun_transmittance = PhysicalTransmittance(
            ground_position + ground_normal * 0.001,
            sun_direction, sun_distance);
        float3 ground_radiance = max(ground_albedo, 0.0)
            * sun_intensity * sun_transmittance
            * (sun_cosine / 3.14159265);
        radiance += view_transmittance * ground_radiance;
    }
    return max(radiance, 0.0);
}

// IGN ベースの dither (8-bit 量子化前にバンディングを消す)。
float SkyDither(float2 pix, float t) {
    pix += t * float2(5.588238, 1.715728);
    return frac(52.9829189 * frac(dot(pix, float2(0.06711056, 0.00583715))));
}

float4 PSMain(VSOut v) : SV_TARGET {
    // 1) NDCからカメラ相対視線を復元する。
    float3 dir   = CameraRelativeViewDirection(float2(v.ndc.x,-v.ndc.y));
    float3 sundn = normalize(sun_dir.xyz);

    // 2) 高さ角でグラデ。dir.y = 0 が地平線、+1 が天頂、-1 が真下。
    //    指数を 0.5 に緩めて天頂までの遷移を滑らかにする。
    float t = dir.y;
    float physical_atmosphere = physical_params.x;
    float3 sky;
    if (physical_atmosphere >= 0.5) {
        sky = EvaluatePhysicalSky(
            dir, sundn, physical_sun_intensity.xyz,
            physical_params.y, physical_ground_albedo.xyz);
    } else if (t >= 0.0) {
        sky = lerp(horizon_color.xyz, zenith_color.xyz, pow(saturate(t), 0.5));
    } else {
        sky = lerp(horizon_color.xyz, ground_color.xyz, pow(saturate(-t), 0.5));
    }

    // 2.5) 太陽方向の地平線グロー。角度と高度を別々に減衰させ、空を横断する
    //      巨大な平滑塊ではなく、低い太陽の周囲だけへ前方散乱を残す。
    float sun_d = saturate(dot(dir, sundn));
    float sunAngle = max(1.0 - sun_d, 0.0);
    float angularAa = max(fwidth(sunAngle), 1.0e-7);
    float discWeight = 1.0 - smoothstep(max(sun_params.x - angularAa, 0.0), sun_params.x + angularAa, sunAngle);
    float haloStart = sun_params.x + angularAa;
    float haloEnd = max(sun_params.y, haloStart + 1.0e-6);
    float haloProfile = 1.0 - smoothstep(haloStart, haloEnd, sunAngle);
    float haloWeight = haloProfile * haloProfile * 0.28 * (1.0 - discWeight);
    float horizonBand = exp(-abs(t) * 12.0);
    float forwardGlow = exp(-sunAngle / max(sun_params.y * 0.35, 1.0e-5)) * horizonBand * 0.18;
    if (physical_atmosphere < 0.5) sky += sun_color.xyz * forwardGlow;

    // 3) 太陽円盤は画素微分で輪郭だけを滑らかにし、光彩は太陽色への混合を0.28へ制限する。
    float3 sun_disc_color = physical_atmosphere >= 0.5
        ? physical_sun_intensity.xyz : sun_color.xyz;
    sky = lerp(sky, sun_disc_color, saturate(discWeight + haloWeight));

    // 4) volumetric clouds: 雲スラブ [h0,h1] を視線方向にレイマーチして密度を積分。各サンプルで太陽へ
    //    ライトマーチして自己影 (Beer-Lambert) → 立体的な雲。地平線より上のみ。
    if (cloud_params0.w >= 0.5 && dir.y > 0.02) {
        float coverage  = saturate(cloud_params0.x);
        float density   = max(cloud_params0.y, 0.1);
        float windOff   = cloud_params0.z * cloud_params1.w * 0.03;   // time*wind で流す

        const float h0 = 1.0, h1 = 2.6;                  // 雲スラブの仮想高度 (dome 空間)
        float t0 = h0 / dir.y, t1 = h1 / dir.y;          // ray がスラブに入る/出る距離
        const int  N  = 48;                              // 高周波ノイズを拾うためステップ増 (undersample 回避)
        float dt = (t1 - t0) / float(N);
        float3 litCol   = lerp(cloud_params1.xyz, sun_color.xyz, sun_d * 0.6);
        float3 shadowCol= cloud_params1.xyz * 0.30;

        // per-pixel ジッタ: 全ピクセルで march 位相が揃うと縦縞バンドが出るため、開始位相を画素ごとに散らす
        // (Bayer/blue-noise ディザ相当)。TAA + 出力ディザが残差を均す。
        float jit = SkyDither(v.pos.xy, cloud_params0.z);   // [0,1)
        float  transmit = 1.0;                           // 視線方向の透過率 (1=空, 0=厚い雲)
        float3 scatter  = float3(0,0,0);
        [loop] for (int i = 0; i < N; ++i) {
            float3 p = dir * (t0 + dt * (float(i) + jit));
            float dens = CloudDensity3(p, coverage, windOff) * density;
            if (dens > 0.01) {
                // 太陽方向へ 3 ステップ light march → 上流の雲量で自己影
                float lightDens = 0.0;
                [loop] for (int l = 1; l <= 3; ++l)
                    lightDens += CloudDensity3(p + sundn * (0.12 * float(l)), coverage, windOff);
                float lightT = exp(-lightDens * 0.9);
                float3 lit   = lerp(shadowCol, litCol, lightT);
                lit += sun_color.xyz * pow(sun_d, 8.0) * (1.0 - lightT) * 0.5;   // silver lining
                float a = 1.0 - exp(-dens * dt * 6.0);    // この区間の不透明度 (消衰を上げ縁をくっきり)
                scatter  += transmit * a * lit;           // front-to-back 合成
                transmit *= (1.0 - a);
                if (transmit < 0.02) break;
            }
        }
        float hFade = smoothstep(0.02, 0.10, dir.y);      // 地平線で雲を消す
        float cloudA = (1.0 - transmit) * hFade;
        sky = sky * (1.0 - cloudA) + scatter * hFade;     // 空の上に雲を合成
    }

    // 5) ディザ: 8-bitへ直接描く場合だけ階調縞を消す。HDRでは最終トーンマッピング側へ一任する。
    //    d2 は画素座標を90度回転してから軸別オフセットと別の時間位相を与え、
    //    同じ内積勾配を平行移動しただけの相関系列にならないようにする。
    float d1 = SkyDither(v.pos.xy, cloud_params0.z);
    float d2 = SkyDither(float2(-v.pos.y, v.pos.x) + float2(113.0, 71.0), cloud_params0.z * 0.37 + 0.5);
    sky += (d1 + d2 - 1.0) * sun_params.z;

    return float4(sky, 1.0);
}
)";

/**
 * スカイ定数バッファのレイアウト (HLSL の cbuffer CSky と一致)。
 */
struct FSkyCb {
    /** 画面 NDC からカメラ相対座標への逆 view-projection。 */
    FMat4 inv_view_proj;

    /** 視点ワールド座標 (xyz=eye)。 */
    FVec4 camera_pos;

    /** 太陽方向 (xyz、camera→sun)。 */
    FVec4 sun_dir;

    /** 太陽の色 (xyz)。 */
    FVec4 sun_color;

    /** 太陽パラメータ (x=radius(1-cos), y=glow, z=出力ディザ強度, w=予約)。 */
    FVec4 sun_params;

    /** 天頂の色。 */
    FVec4 zenith;

    /** 地平線の色。 */
    FVec4 horizon;

    /** 地面方向の色。 */
    FVec4 ground;

    /** 雲パラメータ0 (x=coverage, y=density, z=time, w=enabled)。 */
    FVec4 cloud0;

    /** 雲パラメータ1 (xyz=cloud_color, w=wind_speed)。 */
    FVec4 cloud1;

    /** 物理大気切替と視点高度 (km)。 */
    FVec4 physical;

    /** 物理大気へ渡す太陽放射輝度。 */
    FVec4 physical_sun;

    /** 物理大気へ渡す地表アルベド。 */
    FVec4 physical_ground;
};

/**
 * 定数バッファサイズを 256 バイト境界に切り上げる (DX12 制約)。
 *
 * @tparam T 定数バッファのレイアウト型。
 * @return sizeof(T) を 256 の倍数に切り上げたバイト数。
 */
template<typename T>
constexpr usize CBSize() noexcept {
    return (sizeof(T) + 255u) & ~static_cast<usize>(255u);
}

/**
 * ゼロ長を避けてベクトルを正規化する。
 *
 * @param v 正規化するベクトル。
 * @return 正規化したベクトル。長さがほぼ 0 なら (0,1,0) を返す。
 */
ACS_FORCEINLINE FVec3 NormalizeSafe(FVec3 v) noexcept {
    const f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
    if (!std::isfinite(len2) || len2 < 1e-12f) {
        return FVec3{0, 1, 0};
    }
    const f32 inv = 1.0f / Sqrt(len2);
    return { v.x * inv, v.y * inv, v.z * inv };
}

/** fallback 雲の放射色1成分を非負の half-float 安全範囲へ直す。 */
f32 SanitizeFallbackCloudRadiance_Internal(f32 requested, f32 previous) noexcept {
    if (!std::isfinite(requested)) return previous;
    if (requested < 0.0f) return 0.0f;
    return requested > 16384.0f ? 16384.0f : requested;
}

} // namespace

/**
 * 視線と太陽の角度から円盤、光彩、地平線前方散乱の重みを求める。
 *
 * @param one_minus_cosine 視線と太陽方向の内積を1から引いた値。
 * @param view_elevation 視線方向の上下成分。
 * @param disc_radius_one_minus_cosine 円盤の角半径を1-cos形式で表した値。
 * @param halo_radius_one_minus_cosine 光彩外端の角半径を1-cos形式で表した値。
 * @param angular_filter_width 画素が覆う1-cos形式の角度幅。
 */
FSkySunProfile ResolveSkySunProfile(f32 one_minus_cosine, f32 view_elevation, f32 disc_radius_one_minus_cosine, f32 halo_radius_one_minus_cosine, f32 angular_filter_width) noexcept {
    if (!std::isfinite(one_minus_cosine)) one_minus_cosine = 2.0f;
    if (one_minus_cosine < 0.0f) one_minus_cosine = 0.0f;
    if (one_minus_cosine > 2.0f) one_minus_cosine = 2.0f;
    if (!std::isfinite(view_elevation)) view_elevation = 0.0f;
    if (view_elevation < -1.0f) view_elevation = -1.0f;
    if (view_elevation > 1.0f) view_elevation = 1.0f;
    if (!std::isfinite(disc_radius_one_minus_cosine) || disc_radius_one_minus_cosine < 0.0f) disc_radius_one_minus_cosine = 0.0f;
    if (disc_radius_one_minus_cosine > 2.0f) disc_radius_one_minus_cosine = 2.0f;
    if (!std::isfinite(halo_radius_one_minus_cosine) || halo_radius_one_minus_cosine < disc_radius_one_minus_cosine) halo_radius_one_minus_cosine = disc_radius_one_minus_cosine;
    if (halo_radius_one_minus_cosine > 2.0f) halo_radius_one_minus_cosine = 2.0f;
    if (!std::isfinite(angular_filter_width) || angular_filter_width < 0.0f) angular_filter_width = 0.0f;
    if (angular_filter_width > 2.0f) angular_filter_width = 2.0f;

    /** GPUのsmoothstepと同じ三次補間を行う計算。 */
    const auto smoothStep = [](f32 lower, f32 upper, f32 value) noexcept {
        /** 補間範囲へ正規化した位置。 */
        f32 normalized = (value - lower) / (upper - lower);
        if (normalized < 0.0f) normalized = 0.0f;
        if (normalized > 1.0f) normalized = 1.0f;
        return normalized * normalized * (3.0f - 2.0f * normalized);
    };
    /** 円盤輪郭を滑らかにする最小角度幅。 */
    const f32 edgeWidth = angular_filter_width > 1.0e-7f ? angular_filter_width : 1.0e-7f;
    /** 円盤の内側輪郭。 */
    const f32 discStart = disc_radius_one_minus_cosine > edgeWidth ? disc_radius_one_minus_cosine - edgeWidth : 0.0f;
    /** 円盤の外側輪郭。 */
    const f32 discEnd = disc_radius_one_minus_cosine + edgeWidth;
    /** 光彩が始まる角度。 */
    const f32 haloStart = discEnd;
    /** 光彩補間の零除算を防いだ外端。 */
    const f32 haloEnd = halo_radius_one_minus_cosine > haloStart + 1.0e-6f ? halo_radius_one_minus_cosine : haloStart + 1.0e-6f;
    /** 外端へ向かって減衰する光彩の基礎値。 */
    const f32 haloProfile = 1.0f - smoothStep(haloStart, haloEnd, one_minus_cosine);
    /** 前方散乱の角度減衰幅。 */
    const f32 forwardWidthCandidate = halo_radius_one_minus_cosine * kSkySunForwardGlowWidthScale;
    /** 零除算を防いだ前方散乱の角度減衰幅。 */
    const f32 forwardWidth = forwardWidthCandidate > 1.0e-5f ? forwardWidthCandidate : 1.0e-5f;

    /** 算出した太陽方向の放射成分。 */
    FSkySunProfile out{};
    out.disc_weight = 1.0f - smoothStep(discStart, discEnd, one_minus_cosine);
    out.halo_weight = haloProfile * haloProfile * kSkySunHaloStrength * (1.0f - out.disc_weight);
    out.horizon_glow_weight = std::exp(-one_minus_cosine / forwardWidth) * std::exp(-std::fabs(view_elevation) * kSkySunHorizonGlowFalloff) * kSkySunHorizonGlowStrength;
    return out;
}

/** 太陽円盤の角半径を1-cos形式で設定する。
 * @param one_minus_cosine 太陽中心から円盤外端までの1-cos値。
 */
void CSky::SetSunRadius(f32 one_minus_cosine) noexcept {
    if (!std::isfinite(one_minus_cosine)) return;
    if (one_minus_cosine < 0.0f) one_minus_cosine = 0.0f;
    if (one_minus_cosine > 2.0f) one_minus_cosine = 2.0f;
    m_SunRadius = one_minus_cosine;
    if (m_SunGlow < m_SunRadius) m_SunGlow = m_SunRadius;
}

/** 太陽の光彩外端を1-cos形式で設定する。
 * @param one_minus_cosine 太陽中心から光彩外端までの1-cos値。
 */
void CSky::SetSunGlow(f32 one_minus_cosine) noexcept {
    if (!std::isfinite(one_minus_cosine)) return;
    if (one_minus_cosine < m_SunRadius) one_minus_cosine = m_SunRadius;
    if (one_minus_cosine > 2.0f) one_minus_cosine = 2.0f;
    m_SunGlow = one_minus_cosine;
}

/** 太陽方向を正規化して保持する。 */
void CSky::SetSunDirection(FVec3 dir) noexcept { m_SunDir = NormalizeSafe(dir); }

/** 低コスト fallback 雲の入力を GPU で安全な範囲へ直して保持する。 */
void CSky::SetFallbackClouds(f32 coverage, f32 density) noexcept {
    if (!std::isfinite(coverage)) coverage = 0.0f;
    if (!std::isfinite(density)) density = 1.6f;
    m_FallbackCloudCoverage = coverage < 0.0f ? 0.0f
                            : (coverage > 1.0f ? 1.0f : coverage);
    m_FallbackCloudDensity = density < 0.1f ? 0.1f
                           : (density > 8.0f ? 8.0f : density);
}

/** 低コスト fallback 雲の有限な色成分だけを更新する。 */
void CSky::SetFallbackCloudColor(FVec3 color) noexcept {
    m_FallbackCloudColor = FVec3{SanitizeFallbackCloudRadiance_Internal(color.x, m_FallbackCloudColor.x), SanitizeFallbackCloudRadiance_Internal(color.y, m_FallbackCloudColor.y), SanitizeFallbackCloudRadiance_Internal(color.z, m_FallbackCloudColor.z)};
}

/** 低コスト fallback 雲の風速を GPU で安全な範囲へ直して保持する。 */
void CSky::SetFallbackCloudWind(f32 speed) noexcept {
    if (!std::isfinite(speed)) speed = 0.0f;
    m_FallbackCloudWind = speed < -20.0f ? -20.0f
                        : (speed > 20.0f ? 20.0f : speed);
}

/** 低コスト fallback 雲の時刻を決定論的な有限範囲へ直して保持する。 */
void CSky::SetFallbackCloudTime(f32 seconds) noexcept {
    if (!std::isfinite(seconds)) seconds = 0.0f;
    m_FallbackCloudTime = seconds < -10000000.0f ? -10000000.0f
                        : (seconds > 10000000.0f ? 10000000.0f : seconds);
}

EShaderStatus CSky::FCompiledShaders::Status() const noexcept {
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
    return EShaderStatus::Ready;
}

/** Compile raw-DX12 sky bytecode without accessing the render device. */
TResult<CSky::FCompiledShaders> CSky::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CSky.VS";

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CSky.PS";

    auto vertex = MakeUnique<FDx12Shader>();
    if (!vertex)
        return ACS_ERR(Memory, 571, "CSky vertex shader allocation failed");
    const FHrResult vertex_result = vertex->Init(vs_d);
    if (vertex_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 572, "CSky vertex shader CPU compile failed",
            static_cast<u32>(vertex_result.hr));
    }

    auto pixel = MakeUnique<FDx12Shader>();
    if (!pixel)
        return ACS_ERR(Memory, 573, "CSky pixel shader allocation failed");
    const FHrResult pixel_result = pixel->Init(ps_d);
    if (pixel_result.IsErr()) {
        return ACS_ERR_OS(
            Render, 574, "CSky pixel shader CPU compile failed",
            static_cast<u32>(pixel_result.hr));
    }

    FCompiledShaders compiled{};
    compiled.vertex = TUniquePtr<IRhiShader>(
        vertex.Release(), vertex.GetAllocator());
    compiled.pixel = TUniquePtr<IRhiShader>(
        pixel.Release(), pixel.GetAllocator());
    return TResult<FCompiledShaders>(OkInit, Move(compiled));
#else
    return ACS_ERR(
        Render, 575,
        "CSky CPU compilation is available only on the raw DX12 backend");
#endif
}

TResult<CSky::FCompiledShaders> CSky::BeginCompileShadersAsync(
    IRhiDevice& device) noexcept {
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 576,
            "CSky backend-managed asynchronous compilation is unsupported");
    }

    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name = "CSky.VS";
    vs_d.compile_async = true;

    FCompiledShaders compiled{};
    auto vertex = CreateRhiShader(device, vs_d);
    if (vertex.IsErr()) return Err<FCompiledShaders>(vertex.Error());
    compiled.vertex = Move(vertex.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name = "CSky.PS";
    ps_d.compile_async = true;
    auto pixel = CreateRhiShader(device, ps_d);
    if (pixel.IsErr()) return Err<FCompiledShaders>(pixel.Error());
    compiled.pixel = Move(pixel.Value());

    return TResult<FCompiledShaders>(OkInit, Move(compiled));
}

/** VS/PS/定数バッファ/パイプラインを生成する。 */
TResult<void> CSky::Init(IRhiDevice& device, EFormat rt_format, EFormat depth_format) noexcept {
    FShaderDesc vs_d{};
    vs_d.stage = EShaderStage::Vertex;
    vs_d.hlsl_source = kSkyHLSL;
    vs_d.entry_point = "VSMain";
    vs_d.debug_name  = "CSky.VS";
    auto vs_r = CreateRhiShader(device, vs_d);
    if (vs_r.IsErr()) return Err<void>(vs_r.Error());
    FCompiledShaders compiled{};
    compiled.vertex = Move(vs_r.Value());

    FShaderDesc ps_d{};
    ps_d.stage = EShaderStage::Pixel;
    ps_d.hlsl_source = kSkyHLSL;
    ps_d.entry_point = "PSMain";
    ps_d.debug_name  = "CSky.PS";
    auto ps_r = CreateRhiShader(device, ps_d);
    if (ps_r.IsErr()) return Err<void>(ps_r.Error());
    compiled.pixel = Move(ps_r.Value());

    return InitWithCompiledShaders(
        device, Move(compiled), rt_format, depth_format);
}

/** Install compiled bytecode and atomically publish owner-thread resources. */
TResult<void> CSky::InitWithCompiledShaders(
    IRhiDevice& device,
    FCompiledShaders&& shaders,
    EFormat rt_format,
    EFormat depth_format) noexcept {
    if (shaders.vertex.Get() == nullptr || shaders.pixel.Get() == nullptr) {
        return ACS_ERR(Render, 576, "CSky compiled shader set is incomplete");
    }

    FBufferDesc cbd{};
    cbd.size = CBSize<FSkyCb>();
    cbd.usage = EBufferUsage::Uniform;
    cbd.cpu_writable = true;
    auto cb_r = CreateRhiBuffer(device, cbd);
    if (cb_r.IsErr()) return Err<void>(cb_r.Error());
    TUniquePtr<IRhiBuffer> candidate_cb = Move(cb_r.Value());

    FPipelineDesc pd{};
    pd.vs = shaders.vertex.Get();
    pd.ps = shaders.pixel.Get();
    pd.topology      = EPrimitiveTopology::TriangleList;
    pd.rt_format     = rt_format;
    pd.depth_format  = depth_format;
    pd.depth_test    = false;          // sky は最初に塗るだけ。既存深度は維持
    pd.depth_write   = false;
    pd.cull_mode     = ECullMode::None;
    pd.blend_mode    = EBlendMode::Opaque;
    pd.cbuffer_slots = 1;
    pd.texture_slots = 0;
    pd.cbuffer_names[0] = "CSky";
    pd.layout_count  = 0;
    pd.vertex_stride = 0;
    auto pl_r = CreateRhiPipeline(device, pd);
    if (pl_r.IsErr()) return Err<void>(pl_r.Error());
    TUniquePtr<IRhiPipeline> candidate_pipeline = Move(pl_r.Value());

    // HDRでは後段のトーンマッピングが8bit量子化直前にディザを加えるため、空側は無効にする。
    const bool outputDitherEnabled =
        rt_format == EFormat::R8G8B8A8_UNorm ||
        rt_format == EFormat::R8G8B8A8_UNorm_sRGB ||
        rt_format == EFormat::B8G8R8A8_UNorm;

    // Commit cannot fail. Release the old PSO before the resources it refers
    // to, then publish the replacement PSO last.
    m_Pipeline.Reset();
    m_Vs = Move(shaders.vertex);
    m_Ps = Move(shaders.pixel);
    m_Cb = Move(candidate_cb);
    m_OutputDitherEnabled = outputDitherEnabled;
    m_Pipeline = Move(candidate_pipeline);

    return Ok();
}

/** GPU リソースを解放する。 */
void CSky::Shutdown() noexcept {
    m_Pipeline.Reset();
    m_Cb.Reset();
    m_Ps.Reset();
    m_Vs.Reset();
}

/** 昼空プリセット (青空 + 白い太陽) を適用する。 */
void CSky::PresetDay() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.4f, 0.7f, 0.4f});
    m_SunColor   = FVec3{1.0f, 0.95f, 0.85f};
    m_SunRadius  = kSkySolarDiscRadiusOneMinusCosine;
    m_SunGlow    = kSkyDaySunHaloRadiusOneMinusCosine;
    m_Zenith      = FVec3{0.15f, 0.35f, 0.78f};
    m_Horizon     = FVec3{0.70f, 0.83f, 0.95f};
    m_Ground      = FVec3{0.18f, 0.20f, 0.20f};
    m_FallbackCloudColor    = FVec3{1.0f, 1.0f, 1.0f};
    m_FallbackCloudCoverage = 0.60f;
    m_FallbackCloudDensity  = 1.1f;
    m_FallbackCloudWind     = 1.0f;
}

/** 夕焼けプリセット (茜色 + 暖色太陽) を適用する。 */
void CSky::PresetSunset() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.7f, 0.05f, 0.5f});
    m_SunColor   = FVec3{1.0f, 0.55f, 0.25f};
    m_SunRadius  = kSkySolarDiscRadiusOneMinusCosine;
    m_SunGlow    = kSkySunsetSunHaloRadiusOneMinusCosine;
    m_Zenith      = FVec3{0.06f, 0.10f, 0.30f};
    m_Horizon     = FVec3{1.00f, 0.55f, 0.25f};
    m_Ground      = FVec3{0.10f, 0.06f, 0.08f};
    m_FallbackCloudColor    = FVec3{1.0f, 0.72f, 0.50f};   // 茜色に染まった雲
    m_FallbackCloudCoverage = 0.55f;
    m_FallbackCloudDensity  = 1.3f;
    m_FallbackCloudWind     = 0.8f;
}

/** 夜空プリセット (紺青 + 弱い月光) を適用する。 */
void CSky::PresetNight() noexcept {
    m_SunDir     = NormalizeSafe(FVec3{0.3f, 0.6f, 0.2f});
    m_SunColor   = FVec3{0.85f, 0.85f, 0.95f};
    m_SunRadius  = kSkySolarDiscRadiusOneMinusCosine;
    m_SunGlow    = kSkyNightMoonHaloRadiusOneMinusCosine;
    m_Zenith      = FVec3{0.02f, 0.03f, 0.08f};
    m_Horizon     = FVec3{0.05f, 0.07f, 0.15f};
    m_Ground      = FVec3{0.02f, 0.03f, 0.05f};
    m_FallbackCloudColor    = FVec3{0.10f, 0.12f, 0.20f};  // 紺青の薄い雲
    m_FallbackCloudCoverage = 0.38f;
    m_FallbackCloudDensity  = 1.8f;
    m_FallbackCloudWind     = 0.5f;
}

/** 定数バッファを更新し、フルスクリーン三角形でスカイを描画する。 */
void CSky::RenderInternal(IRhiCommandList& cl, const CCamera& camera,
                          bool physical_atmosphere, FVec3 sun_intensity,
                          f32 altitude_meters, FVec3 ground_albedo) noexcept {
    if (!m_Pipeline || !m_Cb) return;
    /** 有限かつ非負の太陽放射輝度へ入力を正規化する。 */
    const auto sanitize_radiance = [](f32 value) noexcept {
        if (!std::isfinite(value) || value < 0.0f) return 0.0f;
        return value > 65504.0f ? 65504.0f : value;
    };
    sun_intensity = FVec3{
        sanitize_radiance(sun_intensity.x),
        sanitize_radiance(sun_intensity.y),
        sanitize_radiance(sun_intensity.z)};
    if (!std::isfinite(altitude_meters) || altitude_meters < 0.0f) {
        altitude_meters = 0.0f;
    }
    if (altitude_meters > kSkyAtmosphereTopAltitudeMeters) {
        altitude_meters = kSkyAtmosphereTopAltitudeMeters;
    }
    /** 地表反射率を物理的な0～1の範囲へ収める。 */
    const auto sanitize_albedo = [](f32 value) noexcept {
        if (!std::isfinite(value) || value < 0.0f) return 0.0f;
        return value > 1.0f ? 1.0f : value;
    };
    ground_albedo = FVec3{
        sanitize_albedo(ground_albedo.x),
        sanitize_albedo(ground_albedo.y),
        sanitize_albedo(ground_albedo.z)};

    FSkyCb cb{};
    cb.inv_view_proj = BuildCameraRelativeInverseViewProjection(camera.View(), camera.Projection());
    const FVec3 eye = camera.Eye();
    cb.camera_pos = FVec4{eye.x, eye.y, eye.z, 1};
    cb.sun_dir    = FVec4{m_SunDir.x, m_SunDir.y, m_SunDir.z, 0};
    cb.sun_color  = FVec4{m_SunColor.x, m_SunColor.y, m_SunColor.z, 1};
    cb.sun_params = FVec4{m_SunRadius, m_SunGlow,
                          m_OutputDitherEnabled ? (1.0f / 255.0f) : 0.0f, 0.0f};
    cb.zenith     = FVec4{m_Zenith.x, m_Zenith.y, m_Zenith.z, 1};
    cb.horizon    = FVec4{m_Horizon.x, m_Horizon.y, m_Horizon.z, 1};
    cb.ground     = FVec4{m_Ground.x, m_Ground.y, m_Ground.z, 1};
    cb.cloud0     = FVec4{m_FallbackCloudCoverage, m_FallbackCloudDensity,
                          m_FallbackCloudTime, m_FallbackCloudsEnabled ? 1.0f : 0.0f};
    cb.cloud1     = FVec4{m_FallbackCloudColor.x, m_FallbackCloudColor.y,
                          m_FallbackCloudColor.z, m_FallbackCloudWind};
    cb.physical   = FVec4{physical_atmosphere ? 1.0f : 0.0f,
                          altitude_meters * 0.001f, 0.0f, 0.0f};
    cb.physical_sun = FVec4{sun_intensity.x, sun_intensity.y,
                            sun_intensity.z, 0.0f};
    cb.physical_ground = FVec4{ground_albedo.x, ground_albedo.y,
                               ground_albedo.z, 0.0f};
    m_Cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点
}

void CSky::Render(IRhiCommandList& cl, const CCamera& camera) noexcept {
    RenderInternal(cl, camera, false, FVec3{}, 0.0f, FVec3{});
}

void CSky::RenderPhysicalAtmosphere(
    IRhiCommandList& cl, const CCamera& camera, FVec3 sun_intensity,
    f32 altitude_meters, FVec3 ground_albedo) noexcept {
    RenderInternal(cl, camera, true, sun_intensity, altitude_meters,
                   ground_albedo);
}

// ===================== CVolumetricClouds (GPU レイマーチ) =====================
namespace {

// 雲レイマーチ compute。視線ごとに雲スラブを march、Worley FBM 密度を coverage/height で remap、
// 太陽へ light-march (Beer) + dual-lobe HG + 有界な内部散乱近似を積分。出力=非premult色+alpha。
// ---- Perlin-Worley 3D noise を init で 1 回焼く ----
// 最初の128^3 RGBA16Fへ、2周期Perlinだけの低周波形状と、境界だけを動かす
// 高周波変位を含む完成形状を分けて保存する。次の軸別計算は探索用最大値だけを作る。
const char* kNoiseGenCS = R"(
RWTexture3D<float4> shapeSourceOut : register(u0);
RWTexture3D<float4> shapeOccupancySourceOut : register(u1);
float3 h33(float3 p){
    // 超越関数を使わない integer-cell hash。初回 128^3 bake を大幅に高速化し、
    // shader compiler 間でも決定的になる（大きな引数の sin hash は決定的でない）。
    p=frac(p*float3(0.1031,0.1030,0.0973));
    p+=dot(p,p.yxz+33.33);
    return frac((p.xxy+p.yxx)*p.zyx);
}
// HLSLのfmodは負値を負のまま返すため、0以上の周期セルへ明示的に折り返す。
// 領域ゆがみで座標が0未満になっても、タイル境界の両側で同じ勾配を使える。
float3 wrapPeriodicCell(float3 cell,float freq){
    return cell-floor(cell/freq)*freq;
}
// tileable gradient (Perlin) noise。freq = 整数セル数。
float gnoise(float3 x, float freq){
    float3 p=floor(x), f=frac(x);
    // C2 連続補間により、強い太陽光で見える cubic grid の折れ目を消す。
    float3 u=f*f*f*(f*(f*6.0-15.0)+10.0);
    float n=0.0;
    [unroll] for(int dz=0;dz<2;dz++) [unroll] for(int dy=0;dy<2;dy++) [unroll] for(int dx=0;dx<2;dx++){
        float3 o=float3(dx,dy,dz);
        float3 g=normalize(h33(wrapPeriodicCell(p+o,freq))*2.0-1.0 + 1e-4);
        float w=lerp(1.0-u.x,u.x,o.x)*lerp(1.0-u.y,u.y,o.y)*lerp(1.0-u.z,u.z,o.z);
        n += w*dot(g, f-o);
    }
    return n*0.5+0.5;
}
// tileable worley (cellular)。1 - 最近接点距離 → セルが明るい «もくもく» 特性。
float worley(float3 x, float freq){
    float3 p=floor(x*freq), f=frac(x*freq);
    float md=1.0;
    [unroll] for(int dz=-1;dz<=1;dz++) [unroll] for(int dy=-1;dy<=1;dy++) [unroll] for(int dx=-1;dx<=1;dx++){
        float3 o=float3(dx,dy,dz);
        float3 fp=h33(wrapPeriodicCell(p+o,freq));
        float3 d=o+fp-f;
        md=min(md, dot(d,d));
    }
    return 1.0-sqrt(saturate(md));
}
// 0～1の雑音を、振幅の上限が明確な-1～1の変位へ変換する。
float signedCloudNoise(float value){
    return saturate(value)*2.0-1.0;
}
// 低周波の3D湿度を符号付き凝結場の所有者とし、高周波Perlinと零平均Worley差は
// その境界だけを動かす。ここでは正値化せず、未凝結側の距離もLOD間で保持する。
float completedHierarchicalCloudPotential(
    float macroPerlin,float middlePerlin,float finePerlin,
    float worleyA,float worleyB){
    float macroPotential=signedCloudNoise(macroPerlin);
    // 周波数が2倍になるごとに振幅を半減する。Worley差へ残りの1/4を割り当て、
    // 境界変位の絶対上限を1に保つ。
    float boundaryDisplacement=
        signedCloudNoise(middlePerlin)*0.50
        +signedCloudNoise(finePerlin)*0.25
        +(saturate(worleyA)-saturate(worleyB))*0.25;
    // |macroPotential|=1の確定した内外では変位を0にし、境界へ近いほど連続的に戻す。
    float boundaryWeight=1.0-abs(macroPotential);
    return clamp(
        macroPotential+boundaryWeight*boundaryDisplacement,-1.0,1.0);
}
// R16_UNORM相当の保存範囲へ符号付き値を写す。0.5が凝結境界の平均0を表す。
float storedSignedCloudPotential(float potential){
    return saturate(potential*0.5+0.5);
}
// 低周波の3Dゆがみを焼き込み領域へ加え、世界軸へ揃った柱状反復を崩す。
// 形状値そのものは平均せず、生の周波数成分と輪郭の勾配を保持する。
float3 cloudWarpedShapeDomain(float3 uvw){
    // 低周波の3Dゆがみで雲塊の側面を上下へずらし、同じX・Zの房が
    // 高さ方向へ一直線に積み重なるのを防ぐ。追加計算は初回焼き込みだけで行う。
    float warpX=gnoise((uvw+float3(0.173,0.417,0.619))*2.0,2.0);
    float warpZ=gnoise((uvw+float3(0.731,0.251,0.847))*2.0,2.0);
    float3 domainWarp=float3(
        warpX-0.5,(warpX+warpZ)*0.5-0.5,warpZ-0.5)
        *float3(0.22,0.28,0.22);
    return uvw+domainWarp;
}
[numthreads(4,4,4)]
void CSNoise(uint3 id : SV_DispatchThreadID){
    if(id.x>=128u||id.y>=128u||id.z>=128u) return;
    float3 uvw=(float3(id)+0.5)/128.0;
    float3 warpedUvw=cloudWarpedShapeDomain(uvw);
    float perlin2=gnoise(warpedUvw*2.0,2.0);
    float perlin4=gnoise(warpedUvw*4.0,4.0);
    float perlin8=gnoise(warpedUvw*8.0,8.0);
    float worley4A=worley(warpedUvw,4.0);
    // 1周期領域の半・四分・四分の三だけ平行移動した同一分布との差を取り、
    // 非線形な領域変形後にもWorley成分の平均偏りを小さく保つ。
    float worley4B=worley(
        warpedUvw+float3(0.50,0.25,0.75),4.0);
    float completedPotential=completedHierarchicalCloudPotential(
        perlin2,perlin4,perlin8,worley4A,worley4B);
    // R/G/Bは周波数2・4・8までを順に含む符号付き場、Aは平均0である。
    // 周波数を一段ずつ除き、最粗帯域も未解像ならAへ連続移行する。
    float coarsePotential=completedHierarchicalCloudPotential(
        perlin2,0.5,0.5,0.5,0.5);
    float middlePotential=completedHierarchicalCloudPotential(
        perlin2,perlin4,0.5,0.5,0.5);
    shapeSourceOut[id]=float4(
        storedSignedCloudPotential(coarsePotential),
        storedSignedCloudPotential(middlePotential),
        storedSignedCloudPotential(completedPotential),0.5);
    // 占有階層は、どのLOD重みでも加重和以上になる三帯域の最大値を所有する。
    // R8 UNormの最近接丸めで真値を下回らないよう、保存前に1コード上へ広げる。
    float maximumPotential=max(
        coarsePotential,max(middlePotential,completedPotential));
    float conservativeStoredPotential=min(
        storedSignedCloudPotential(maximumPotential)+1.0/255.0,1.0);
    shapeOccupancySourceOut[id]=conservativeStoredPotential.xxxx;
}
)";

// 128要素の各行へ、4・16・64標本の前方最大値を並列に作る。
// 読み書き座標を循環転置して三回通すと、RGBはそれぞれ7・19・67角の
// 保守的な周期最大値となり、Aの点支持域と共に元の軸順へ戻る。
const char* kNoiseFilterCS = R"(
Texture3D<float4> shapeFilterSource : register(t0);
RWTexture3D<float4> shapeFilterOut : register(u0);
groupshared float3 shapeMaximumA[128];
groupshared float3 shapeMaximumB[128];

// 128要素固定の周期行へ符号付き添字を戻す。
uint wrapShapeLineIndex(int index){
    return uint(index)&127u;
}
[numthreads(128,1,1)]
void CSNoiseFilter(
    uint3 groupId : SV_GroupID,
    uint3 groupThreadId : SV_GroupThreadID){
    uint lineIndex=groupThreadId.x;
    uint3 sourceCoord=uint3(lineIndex,groupId.x,groupId.y);
    float4 sourceValue=shapeFilterSource.Load(int4(sourceCoord,0));
    shapeMaximumA[lineIndex]=sourceValue.rgb;
    GroupMemoryBarrierWithGroupSync();
    // 二つの共有領域を交互に使い、同じ段の書込みを次の読取りへ混ぜない。
    // Rは4、Gは16、Bは64標本へ到達した時点で延長を止める。
    shapeMaximumB[lineIndex]=max(
        shapeMaximumA[lineIndex],
        shapeMaximumA[wrapShapeLineIndex(int(lineIndex)+1)]);
    GroupMemoryBarrierWithGroupSync();
    shapeMaximumA[lineIndex]=max(
        shapeMaximumB[lineIndex],
        shapeMaximumB[wrapShapeLineIndex(int(lineIndex)+2)]);
    GroupMemoryBarrierWithGroupSync();
    float3 maximum4=shapeMaximumA[lineIndex];
    float3 extension4=shapeMaximumA[
        wrapShapeLineIndex(int(lineIndex)+4)];
    shapeMaximumB[lineIndex]=float3(
        maximum4.r,max(maximum4.g,extension4.g),
        max(maximum4.b,extension4.b));
    GroupMemoryBarrierWithGroupSync();
    float3 maximum8=shapeMaximumB[lineIndex];
    float3 extension8=shapeMaximumB[
        wrapShapeLineIndex(int(lineIndex)+8)];
    shapeMaximumA[lineIndex]=float3(
        maximum8.r,max(maximum8.g,extension8.g),
        max(maximum8.b,extension8.b));
    GroupMemoryBarrierWithGroupSync();
    float3 maximum16=shapeMaximumA[lineIndex];
    float3 extension16=shapeMaximumA[
        wrapShapeLineIndex(int(lineIndex)+16)];
    shapeMaximumB[lineIndex]=float3(
        maximum16.r,maximum16.g,
        max(maximum16.b,extension16.b));
    GroupMemoryBarrierWithGroupSync();
    float3 maximum32=shapeMaximumB[lineIndex];
    float3 extension32=shapeMaximumB[
        wrapShapeLineIndex(int(lineIndex)+32)];
    shapeMaximumA[lineIndex]=float3(
        maximum32.r,maximum32.g,
        max(maximum32.b,extension32.b));
    GroupMemoryBarrierWithGroupSync();
    // 三線形再構成が隣接セルを読む分だけ両側へ一セル広げる。
    // 二つの前方区間の和集合は、中心からR=3、G=9、B=33標本を覆う。
    float3 conservativeMaximum=float3(
        max(shapeMaximumA[
                wrapShapeLineIndex(int(lineIndex)-3)].r,
            shapeMaximumA[lineIndex].r),
        max(shapeMaximumA[
                wrapShapeLineIndex(int(lineIndex)-9)].g,
            shapeMaximumA[
                wrapShapeLineIndex(int(lineIndex)-6)].g),
        max(shapeMaximumA[
                wrapShapeLineIndex(int(lineIndex)-33)].b,
            shapeMaximumA[
                wrapShapeLineIndex(int(lineIndex)-30)].b));
    // RGBは各担当幅の最大ポテンシャル、Aは未加工の点ポテンシャルを循環転置する。
    shapeFilterOut[sourceCoord.yzx]=saturate(float4(
        conservativeMaximum,sourceValue.a));
}
)";

// 広域の天候場は3D形状体積から独立させる。同じ周期を被覆、形状、侵食へ
// 流用すると、地上から地平線まで同じ繰り返しが露出する。
// R=被覆、G=雲種、B=降水、A=低周波の対流ポテンシャル兼ゆがみ。
const char* kWeatherGenCS = R"(
RWTexture2D<float4> weatherOut : register(u0);

float hash21(float2 p) {
    p=frac(p*float2(0.1031,0.1030));
    p+=dot(p,p.yx+33.33);
    return frac((p.x+p.y)*p.x);
}
float periodicValue(float2 p,float period,float2 seed) {
    float2 i=floor(p), f=frac(p);
    float2 u=f*f*(3.0-2.0*f);
    // fmodは負値を負のまま返すため、領域外の座標でも0以上へ戻る床剰余を使う。
    float2 i00=i-floor(i/period)*period;
    float2 i10=i+float2(1,0);
    float2 i01=i+float2(0,1);
    float2 i11=i+float2(1,1);
    i10=i10-floor(i10/period)*period;
    i01=i01-floor(i01/period)*period;
    i11=i11-floor(i11/period)*period;
    float a=hash21(i00+seed), b=hash21(i10+seed);
    float c=hash21(i01+seed), d=hash21(i11+seed);
    return lerp(lerp(a,b,u.x),lerp(c,d,u.x),u.y);
}
float weatherFbm(float2 uv,float2 seed) {
    float n=0.0;
    n+=periodicValue(uv*3.0,3.0,seed)*0.50;
    n+=periodicValue(uv*7.0,7.0,seed+17.0)*0.27;
    n+=periodicValue(uv*13.0,13.0,seed+41.0)*0.15;
    n+=periodicValue(uv*29.0,29.0,seed+83.0)*0.08;
    return saturate(n);
}
// 被覆は雲塊の配置だけを決める。細かな2D模様を含めると、それが雲層全高へ押し出されて柱になる。
float weatherCoverageFbm(float2 uv,float2 seed) {
    float n=0.0;
    // 3・7分割は広域の雲塊配置と適度な縁の揺らぎを両立する。
    // 低周波へ寄せ過ぎると、大きな雲塊の縁へ縦筋が現れるため維持する。
    n+=periodicValue(uv*3.0,3.0,seed)*0.68;
    n+=periodicValue(uv*7.0,7.0,seed+17.0)*0.32;
    return saturate(n);
}
[numthreads(8,8,1)]
void CSWeather(uint3 id : SV_DispatchThreadID) {
    if(id.x>=512u||id.y>=512u) return;
    float2 uv=(float2(id.xy)+0.5)/512.0;
    float coverage=weatherCoverageFbm(uv,float2(11.7,29.3));
    float cloudType=weatherFbm(uv.yx+float2(0.19,0.43),float2(67.1,5.9));
    // 降水域と対流域へ細かな二次元粒を入れると、その輪郭が層全高へ押し出される。
    // 被覆と同じ二帯域だけを使い、数km単位で連続する発達域を作る。
    float storm=weatherCoverageFbm(float2(1.0-uv.y,uv.x)+float2(0.31,0.07),float2(103.7,47.2));
    float warp=weatherCoverageFbm(uv+float2(0.53,0.23),float2(151.9,73.4));
    float precipitation=saturate((storm-0.48)*2.2)*saturate((coverage-0.38)*1.9);
    weatherOut[id.xy]=float4(coverage,cloudType,precipitation,warp);
}
)";

// 基本形状とは独立した縁の侵食体積。同じ細胞模様を雲塊と房の両方へ出さない。
const char* kDetailGenCS = R"(
RWTexture3D<float2> detailOut : register(u0);

float3 detailHash(float3 p,float3 seed) {
    p=frac((p+seed)*float3(0.1031,0.1030,0.0973));
    p+=dot(p,p.yxz+33.33);
    return frac((p.xxy+p.yxx)*p.zyx);
}
float3 wrapDetailPeriodicCell(float3 cell,float frequency) {
    return cell-floor(cell/frequency)*frequency;
}
float detailWorley(float3 x,float frequency,float3 seed) {
    float3 cell=floor(x);
    float3 local=frac(x);
    float minDistance=10.0;
    [unroll] for(int z=-1;z<=1;z++)
    [unroll] for(int y=-1;y<=1;y++)
    [unroll] for(int xOffset=-1;xOffset<=1;xOffset++) {
        float3 offset=float3(xOffset,y,z);
        float3 wrapped=wrapDetailPeriodicCell(cell+offset,frequency);
        float3 feature=detailHash(wrapped,seed);
        float3 delta=offset+feature-local;
        minDistance=min(minDistance,dot(delta,delta));
    }
    return 1.0-sqrt(saturate(minDistance));
}
[numthreads(4,4,4)]
void CSDetail(uint3 id : SV_DispatchThreadID) {
    if(id.x>=64u||id.y>=64u||id.z>=64u) return;
    float3 uvw=(float3(id)+0.5)/64.0;
    float a=detailWorley(uvw*4.0,4.0,float3(17.0,41.0,73.0));
    float b=detailWorley(uvw*8.0,8.0,float3(89.0,29.0,53.0));
    float c=detailWorley(uvw*16.0,16.0,float3(11.0,97.0,37.0));
    float d=saturate(a*0.55+b*0.30+c*0.15);
    // R は最初の Worley 帯域、G は三帯域の合成値。未使用成分を持たない RG16F に収める。
    detailOut[id]=float2(a,d);
}
)";

// A world-space curl field supplies coherent shear without boiling the global
// weather pattern.  It is generated from an independent periodic potential,
// then sampled at two incommensurate orientations in the cloud shader.
const char* kCurlGenCS = R"(
RWTexture2D<float2> curlOut : register(u0);

float curlHash(float2 p,float2 seed) {
    p=frac((p+seed)*float2(0.1031,0.1030));
    p+=dot(p,p.yx+33.33);
    return frac((p.x+p.y)*p.x);
}
float2 wrapCurlPeriodicCell(float2 cell,float frequency) {
    return cell-floor(cell/frequency)*frequency;
}
float curlValue(float2 p,float frequency,float2 seed) {
    float2 cell=floor(p);
    float2 f=frac(p);
    float2 u=f*f*f*(f*(f*6.0-15.0)+10.0);
    float2 i00=wrapCurlPeriodicCell(cell,frequency);
    float2 i10=wrapCurlPeriodicCell(cell+float2(1,0),frequency);
    float2 i01=wrapCurlPeriodicCell(cell+float2(0,1),frequency);
    float2 i11=wrapCurlPeriodicCell(cell+float2(1,1),frequency);
    float a=curlHash(i00,seed), b=curlHash(i10,seed);
    float c=curlHash(i01,seed), d=curlHash(i11,seed);
    return lerp(lerp(a,b,u.x),lerp(c,d,u.x),u.y);
}
float potential(float2 uv) {
    return curlValue(uv*4.0,4.0,float2(19.0,71.0))*0.58
         + curlValue(uv*9.0,9.0,float2(83.0,31.0))*0.28
         + curlValue(uv*17.0,17.0,float2(47.0,107.0))*0.14;
}
[numthreads(8,8,1)]
void CSCurl(uint3 id : SV_DispatchThreadID) {
    if(id.x>=128u||id.y>=128u) return;
    float2 uv=(float2(id.xy)+0.5)/128.0;
    const float e=1.0/128.0;
    float dx=potential(uv+float2(e,0))-potential(uv-float2(e,0));
    float dy=potential(uv+float2(0,e))-potential(uv-float2(0,e));
    float2 curl=float2(dy,-dx);
    // Preserve the derivative magnitude.  Normalizing every texel turns flat
    // regions into full-strength, discontinuous directions and creates another
    // visible repeated motif.
    curlOut[id.xy]=clamp(curl*8.0,-1.0,1.0);
}
)";

const char* kCloudCS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4x4 prevCameraRelativeViewProj;
    float4 camPos;     // xyz
    float4 prevCamPos; // xyz
    float4 sunDir;     // xyz (world)
    float4 sunCol;     // rgb (color*intensity)
    float4 skyCol;     // rgb ambient
    float4 params;     // x=被覆率, y=密度, z=風*時間, w=時間
    float4 dims;       // xy=ray-march 解像度, zw=全解像度の寸法
    float4 temporal;   // x=履歴の有効性, y=前回の風オフセット, z=フレーム番号, w=2x2 interleave
    float4 layer;      // x=world base Y, y=world top Y, z=XZ noise scale, w=1 m当たりの基準消散
    float4 worldOrigin;// xyz=discrete curved-shell tangent origin
    float4 shadowGrid; // xy=material-space min XZ, zw=inverse horizontal extents
    // x=valid, y=太陽の角半径(rad), z=1/width/depth, w=環境光外周の二次係数
    float4 shadowState;
    float4 groundHorizon;// xyz=camera local up, w=ground tangent elevation; <-1 disables
    float4 cloudFrameTerms;// xy=world wind, z=shape scale, w=1/(top-base)
    float4 cloudLightTangent;// xyz=CPU-hoisted Duff/Frisvad tangent
    float4 cloudLightBitangent;// xyz=CPU-hoisted Duff/Frisvad bitangent
    float4 cloudCoverage;// xy=天候しきい値、zw=基本形状雑音の固定正規化範囲
    // xy=inverse weather transition widths, z=view fine step, w=予約
    float4 cloudCoverageReciprocals;
    // CPUで先行計算した曲面雲層の二次方程式項。
    // xyz=惑星中心から見たカメラ, w=下層底面のc
    float4 cloudShellRayOrigin;
    // x=下層上面, yz=上層底面・上面のc
    float4 cloudShellTerms;
    // 雲を «光を散らす媒質» として扱うための係数。これまでは shader 内の即値だった。
    // x=見る側の消散, y=光の側の消散, z=太陽光の散乱率, w=周囲散乱源の確率を混ぜる割合
    float4 cloudLightingExtinction;
    // x=前方散乱の鋭さ, y=後方散乱, z=前方の混ぜ率, w=多重散乱の寄与
    float4 cloudLightingPhase;
    // x=多重散乱の消散の弱め方, y=位相の下限, z=位相の上限, w=地面からの照り返し
    float4 cloudLightingMulti;
    // x=雲底が空から受ける割合, y=雲頂が受ける割合
    float4 cloudLightingAmbient;
    // xyz=地面側から入る放射輝度, w=有限次数で失う太陽散乱輝度の補償倍率
    float4 cloudLightingGround;
    // xyz=太陽光が雲へ届くまでの大気透過率 (低い太陽で赤くなる)
    float4 cloudSunTransmittance;
    // xyz=天頂の空の色, w=1 なら «雲頂は天頂・雲底は地平» で分ける
    float4 cloudSkyZenith;
    // x=多重散乱に使う位相の鋭さ (0 で等方)
    float4 cloudMultiPhase;
    // x=層外の最大距離, y=層外で薄め始める距離, z=遠いレイの刻み拡大, w=現在高度の最大距離
    float4 cloudRange;
    // x=上層の底, y=上層の天井, z=1/(天井-底), w=1 なら上層あり
    float4 cloudUpperLayer;
    // x=上層の被覆, y=上層の濃さ, z=1 m当たりの基準消散, w=予約
    float4 cloudUpperTerms;
    // xy=基本形状の位相ずれ, zw=渦と侵食の位相ずれ
    float4 cloudEvolution;
    // x=目標雲種, y=雲種適用率, z=目標降水成分, w=降水適用率
    float4 cloudWeatherControl;
    // xy=更新する偶奇位置, z=各軸の更新間隔, w=1 なら全更新
    float4 cloudShadowUpdate;
    // xy=基準面上の左下XZ, z=1/範囲, w=基準面のワールドY
    float4 cloudWorldShadowMap;
    // 前フレームの対流位相。時間再投影が形状変化を検出するために使う。
    float4 cloudPreviousEvolution;
    // xy=立体物影で更新する偶奇位置, z=各軸の更新間隔, w=1なら全更新
    float4 cloudWorldShadowUpdate;
    // x=前フレーム照明との差による履歴更新割合
    float4 cloudLightingHistory;
};
RWTexture2D<float4> cloudOut : register(u0);
RWTexture2D<float2> cloudDepthOut : register(u1); // x=不透明度加重ヒット距離, y=アルファ信頼度
// 先頭32高度はxy=上層の空・地面方向、zw=下層の空・地面方向の面積平均透過率。
// 続く各32高度は、同じ始点で評価した太陽円盤4方向の一次・二次・三次透過率。
RWTexture3D<float4> cloudShadowOut : register(u2);
Texture3D<float4> shapeNoise     : register(t0);   // RGB=周波数2・4・8までの密度形状
Texture3D<float4> shapeOccupancy : register(t1);   // RGB=7・19・67角の全帯域最大値、A=点最大値
Texture2D    weatherMap          : register(t2);   // coverage/type/precipitation/warp
Texture3D<float2> detailNoise    : register(t3);   // (低周波房, 三帯域侵食)
Texture2D    curlNoise           : register(t4);   // independent world-space curl field
// CSCloud は、現在フレームに生成した周囲光と三つの散乱次数別太陽透過率をt5から読む。
Texture3D<float4> cloudShadowCache : register(t5);
SamplerState shapeNoise_sampler      : register(s0); // wrap (tileable)
SamplerState shapeOccupancy_sampler  : register(s1); // 保守的最大値を読むpoint-wrap
SamplerState weatherMap_sampler      : register(s2); // world-scale wrap
SamplerState detailNoise_sampler     : register(s3); // wrap (tileable)
SamplerState curlNoise_sampler       : register(s4); // world-scale wrap
SamplerState cloudShadowCache_sampler : register(s5); // clamp (finite cache footprint)

float remapc(float v,float a,float b,float c,float d){ return c + saturate((v-a)/max(b-a,1e-5))*(d-c); }
float hash13(float3 p){ p=frac(p*0.1031); p+=dot(p,p.zyx+31.32); return frac((p.x+p.y)*p.z); }
// CPU側でabs(g)<=0.99を保証する。前方または後方の鋭い頂点でも近い数の差を取らず、桁落ちを防ぐ。
float hg(float c,float g){ float a=abs(g); float oneMinusA=1.0-a; float alignedC=g>=0.0?c:-c; float d=oneMinusA*oneMinusA+2.0*a*max(1.0-alignedC,0.0); return (oneMinusA*(1.0+a))/(12.566370*pow(max(d,1e-6),1.5)); }
// 既に求めた区間透過率から、次数ごとに縮小した均質区間の散乱重みを解析積分する。
// xが0へ近づく場合はphi(x)=(1-exp(-x))/xの級数を使い、重み・重心・透過率を
// 同じ縮小光学的深さへ揃える。
// 均質区間の吸収率を薄い媒質では級数で求め、指数近似が物理上限
// min(光学的深さ,1)を越えても積分へ持ち込まない。
bool cloudValueIsFinite(float value){
    return value==value&&value>=-3.402823466e38
        &&value<=3.402823466e38;
}
float cloudBeerAbsorptionFraction(float opticalDepth){
    float resolvedAbsorption=1.0;
    bool finiteInput=cloudValueIsFinite(opticalDepth);
    if(finiteInput){
        float safeOpticalDepth=max(opticalDepth,0.0);
        float maximumAbsorption=min(safeOpticalDepth,1.0);
        resolvedAbsorption=0.0;
        if(safeOpticalDepth>0.0){
            float absorption=maximumAbsorption;
            if(safeOpticalDepth<=0.125){
                float opticalDepthSquared=
                    safeOpticalDepth*safeOpticalDepth;
                absorption=safeOpticalDepth*(
                    1.0-safeOpticalDepth*0.5
                    +opticalDepthSquared/6.0
                    -opticalDepthSquared*safeOpticalDepth/24.0
                    +opticalDepthSquared*opticalDepthSquared/120.0
                    -opticalDepthSquared*opticalDepthSquared
                        *safeOpticalDepth/720.0);
            }
            else
                absorption=1.0-exp(-safeOpticalDepth);
            resolvedAbsorption=absorption==absorption
                ?clamp(absorption,0.0,maximumAbsorption)
                :maximumAbsorption;
        }
    }
    return resolvedAbsorption;
}
float cloudReducedIntervalScatteringWeight(
    float opticalDepth,float reducedOpticalDepth,
    float intervalTransmittance,float contribution){
    bool finiteInput=cloudValueIsFinite(opticalDepth)
        &&cloudValueIsFinite(reducedOpticalDepth)
        &&cloudValueIsFinite(intervalTransmittance)
        &&cloudValueIsFinite(contribution);
    float resolvedWeight=0.0;
    if(finiteInput&&opticalDepth>0.0&&contribution>0.0){
        float safeOpticalDepth=max(opticalDepth,0.0);
        float safeContribution=max(contribution,0.0);
        float safeReducedOpticalDepth=max(reducedOpticalDepth,0.0);
        float normalizedAbsorption=1.0;
        if(safeReducedOpticalDepth<=0.001){
            float reducedOpticalDepthSquared=
                safeReducedOpticalDepth*safeReducedOpticalDepth;
            normalizedAbsorption=1.0-safeReducedOpticalDepth*0.5
                +reducedOpticalDepthSquared/6.0
                -reducedOpticalDepthSquared*safeReducedOpticalDepth/24.0;
        }
        else
            normalizedAbsorption=(1.0-saturate(intervalTransmittance))
                /safeReducedOpticalDepth;
        float maximumWeight=safeContribution*safeOpticalDepth;
        float weight=maximumWeight*normalizedAbsorption;
        if(!(weight==weight)||weight<0.0) weight=maximumWeight;
        resolvedWeight=min(weight,maximumWeight);
    }
    return resolvedWeight;
}

// 均質区間のBeer-Lambert吸収が起きる平均位置を、区間始点からの比率で返す。
// 既に求めた透過率を再利用し、次数ごとの指数演算を重複させない。
float cloudBeerAbsorptionCentroidFraction(float opticalDepth,float intervalTransmittance){
    float absorptionFraction=0.5;
    bool finiteOpticalDepth=cloudValueIsFinite(opticalDepth);
    if(finiteOpticalDepth){
        float safeOpticalDepth=max(opticalDepth,0.0);
        if(safeOpticalDepth>0.0&&safeOpticalDepth<=1.0){
            float opticalDepthSquared=safeOpticalDepth*safeOpticalDepth;
            float opticalDepthCubed=
                opticalDepthSquared*safeOpticalDepth;
            float opticalDepthFifth=
                opticalDepthCubed*opticalDepthSquared;
            float opticalDepthSeventh=
                opticalDepthFifth*opticalDepthSquared;
            float opticalDepthNinth=
                opticalDepthSeventh*opticalDepthSquared;
            absorptionFraction=0.5-safeOpticalDepth/12.0
                +opticalDepthCubed/720.0
                -opticalDepthFifth/30240.0
                +opticalDepthSeventh/1209600.0
                -opticalDepthNinth/47900160.0;
        }
        else if(safeOpticalDepth>20.0)
            absorptionFraction=1.0/safeOpticalDepth;
        else if(safeOpticalDepth>0.0){
            bool finiteTransmittance=
                cloudValueIsFinite(intervalTransmittance);
            float transmittance=finiteTransmittance
                ?saturate(intervalTransmittance)
                :1.0-cloudBeerAbsorptionFraction(safeOpticalDepth);
            // この分岐は光学的深さが1より大きいため、正しいBeer透過率なら
            // 1-Tは0.632以上になる。入力透過率が壊れていても、物理量を
            // 無限大へ膨らませず有限な重心として扱う。
            absorptionFraction=1.0/safeOpticalDepth
                -transmittance/max(1.0-transmittance,0.5);
        }
    }
    return clamp(absorptionFraction,0.0,0.5);
}

// 横断面4レーンへ同じBeer-Lambert式を適用する。スカラー関数を共有し、
// 薄い媒質の級数と非有限値の扱いをCPU参照式と揃える。
float4 cloudBeerAbsorptionFraction4(float4 opticalDepth){
    return float4(
        cloudBeerAbsorptionFraction(opticalDepth.x),
        cloudBeerAbsorptionFraction(opticalDepth.y),
        cloudBeerAbsorptionFraction(opticalDepth.z),
        cloudBeerAbsorptionFraction(opticalDepth.w));
}
float4 cloudBeerAbsorptionCentroidFraction4(
    float4 opticalDepth,float4 intervalTransmittance){
    return float4(
        cloudBeerAbsorptionCentroidFraction(
            opticalDepth.x,intervalTransmittance.x),
        cloudBeerAbsorptionCentroidFraction(
            opticalDepth.y,intervalTransmittance.y),
        cloudBeerAbsorptionCentroidFraction(
            opticalDepth.z,intervalTransmittance.z),
        cloudBeerAbsorptionCentroidFraction(
            opticalDepth.w,intervalTransmittance.w));
}
// 雲内部で有限・非負と確定済みの4レーンを一つの式で解く。
// 成分ごとの同じ制御フローを旧FXCへ展開させず、スカラー版と同じ級数を使う。
float4 cloudFiniteBeerAbsorptionFraction4(float4 requestedOpticalDepth){
    float4 opticalDepth=float4(
        cloudValueIsFinite(requestedOpticalDepth.x)
            ?max(requestedOpticalDepth.x,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.y)
            ?max(requestedOpticalDepth.y,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.z)
            ?max(requestedOpticalDepth.z,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.w)
            ?max(requestedOpticalDepth.w,0.0):0.0);
    // 級数は0.125以下でだけ有効なため、その範囲へ先に閉じる。
    // lerpは未選択側も評価するので、未使用の4乗・5乗中間値も有限に保つ。
    float4 seriesOpticalDepth=min(opticalDepth,0.125.xxxx);
    float4 opticalDepthSquared=seriesOpticalDepth*seriesOpticalDepth;
    float4 opticalDepthFourth=opticalDepthSquared*opticalDepthSquared;
    float4 seriesAbsorption=seriesOpticalDepth*(
        1.0.xxxx-seriesOpticalDepth*0.5
        +opticalDepthSquared/6.0
        -opticalDepthSquared*seriesOpticalDepth/24.0
        +opticalDepthFourth/120.0
        -opticalDepthFourth*seriesOpticalDepth/720.0);
    // 光学的深さ80を超える透過率は単精度で区別できないため、指数の引数も閉じる。
    float4 exponentialAbsorption=1.0.xxxx-exp(-min(opticalDepth,80.0.xxxx));
    float4 absorption=lerp(
        exponentialAbsorption,seriesAbsorption,
        step(opticalDepth,0.125.xxxx));
    return clamp(absorption,0.0.xxxx,min(opticalDepth,1.0.xxxx));
}
// 有限Beer区間の吸収重心を4レーン同時に解き、薄い区間の桁落ちを避ける。
float4 cloudFiniteBeerAbsorptionCentroidFraction4(
    float4 requestedOpticalDepth,float4 requestedTransmittance){
    float4 opticalDepth=float4(
        cloudValueIsFinite(requestedOpticalDepth.x)
            ?max(requestedOpticalDepth.x,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.y)
            ?max(requestedOpticalDepth.y,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.z)
            ?max(requestedOpticalDepth.z,0.0):0.0,
        cloudValueIsFinite(requestedOpticalDepth.w)
            ?max(requestedOpticalDepth.w,0.0):0.0);
    float4 transmittance=float4(
        cloudValueIsFinite(requestedTransmittance.x)
            ?saturate(requestedTransmittance.x)
            :1.0-cloudBeerAbsorptionFraction(opticalDepth.x),
        cloudValueIsFinite(requestedTransmittance.y)
            ?saturate(requestedTransmittance.y)
            :1.0-cloudBeerAbsorptionFraction(opticalDepth.y),
        cloudValueIsFinite(requestedTransmittance.z)
            ?saturate(requestedTransmittance.z)
            :1.0-cloudBeerAbsorptionFraction(opticalDepth.z),
        cloudValueIsFinite(requestedTransmittance.w)
            ?saturate(requestedTransmittance.w)
            :1.0-cloudBeerAbsorptionFraction(opticalDepth.w));
    // 級数は光学的深さ1以下でだけ使うため、その範囲へ先に閉じて積を有限にする。
    float4 seriesOpticalDepth=min(opticalDepth,1.0.xxxx);
    float4 opticalDepthSquared=seriesOpticalDepth*seriesOpticalDepth;
    float4 opticalDepthCubed=opticalDepthSquared*seriesOpticalDepth;
    float4 opticalDepthFifth=opticalDepthCubed*opticalDepthSquared;
    float4 opticalDepthSeventh=opticalDepthFifth*opticalDepthSquared;
    float4 opticalDepthNinth=opticalDepthSeventh*opticalDepthSquared;
    float4 seriesCentroid=0.5.xxxx-seriesOpticalDepth/12.0
        +opticalDepthCubed/720.0
        -opticalDepthFifth/30240.0
        +opticalDepthSeventh/1209600.0
        -opticalDepthNinth/47900160.0;
    // 1以下では級数を使うため、逆数側の未使用値も巨大数へ膨らませない。
    float4 inverseOpticalDepth=1.0.xxxx/max(opticalDepth,1.0.xxxx);
    // 光学的深さが1より大きい分岐でだけ使うため、物理的な1-Tの下限を
    // 使う。極小番兵値の逆数を作ると、旧FXCが巨大な一時値を生成する。
    float4 regularCentroid=inverseOpticalDepth
        -transmittance/max(1.0.xxxx-transmittance,0.5.xxxx);
    float4 resolvedCentroid=lerp(
        regularCentroid,seriesCentroid,step(opticalDepth,1.0.xxxx));
    resolvedCentroid=lerp(
        resolvedCentroid,inverseOpticalDepth,
        step(20.000001.xxxx,opticalDepth));
    return clamp(resolvedCentroid,0.0.xxxx,0.5.xxxx);
}
// 有限吸収率を薄い媒質用級数から光学的深さへ4レーン同時に戻す。
float4 cloudFiniteOpticalDepthFromAbsorption4(float4 requestedAbsorption){
    // 無効な吸収率は上流の異常を次の対数・級数へ渡さず、無吸収として扱う。
    float4 absorption=float4(
        cloudValueIsFinite(requestedAbsorption.x)
            ?saturate(requestedAbsorption.x):0.0,
        cloudValueIsFinite(requestedAbsorption.y)
            ?saturate(requestedAbsorption.y):0.0,
        cloudValueIsFinite(requestedAbsorption.z)
            ?saturate(requestedAbsorption.z):0.0,
        cloudValueIsFinite(requestedAbsorption.w)
            ?saturate(requestedAbsorption.w):0.0);
    float4 squared=absorption*absorption;
    float4 cubed=squared*absorption;
    float4 fourth=squared*squared;
    float4 fifth=fourth*absorption;
    float4 sixth=cubed*cubed;
    float4 seriesDepth=absorption+squared*0.5+cubed/3.0
        +fourth*0.25+fifth*0.2+sixth/6.0;
    float4 logarithmicDepth=min(
        -log(max(1.0.xxxx-absorption,1e-35.xxxx)),80.0.xxxx);
    return lerp(
        logarithmicDepth,seriesDepth,step(absorption,0.125.xxxx));
}
float4 cloudReducedIntervalScatteringWeight4(
    float4 opticalDepth,float4 reducedOpticalDepth,
    float4 intervalTransmittance,float contribution){
    return float4(
        cloudReducedIntervalScatteringWeight(
            opticalDepth.x,reducedOpticalDepth.x,
            intervalTransmittance.x,contribution),
        cloudReducedIntervalScatteringWeight(
            opticalDepth.y,reducedOpticalDepth.y,
            intervalTransmittance.y,contribution),
        cloudReducedIntervalScatteringWeight(
            opticalDepth.z,reducedOpticalDepth.z,
            intervalTransmittance.z,contribution),
        cloudReducedIntervalScatteringWeight(
            opticalDepth.w,reducedOpticalDepth.w,
            intervalTransmittance.w,contribution));
}

// 光学的深さは補間せず、各光路をBeer-Lambert透過率へ変換してから扱う。
// 不均一なセルで exp(-平均深さ) を作ると、晴天側の太陽光を失うためである。
float4 cloudSunTransmittanceFromDepth(float4 lightDepths,float extinction){
    return exp(-max(lightDepths,0.0.xxxx)*max(extinction,0.0));
}
// 低詳細度キャッシュへ近距離の有効光学的深さ差分を適用する。次数ごとに
// 統計透過率から求めた差を受け取り、平均密度の共通残差を再利用しない。
float4 cloudApplySunOpticalDepthResidual(
    float4 pathTransmittance,float4 opticalDepthResiduals){
    float4 safePathTransmittance=float4(
        cloudValueIsFinite(pathTransmittance.x)
            ?saturate(pathTransmittance.x):0.0,
        cloudValueIsFinite(pathTransmittance.y)
            ?saturate(pathTransmittance.y):0.0,
        cloudValueIsFinite(pathTransmittance.z)
            ?saturate(pathTransmittance.z):0.0,
        cloudValueIsFinite(pathTransmittance.w)
            ?saturate(pathTransmittance.w):0.0);
    float4 safeOpticalDepthResiduals=float4(
        cloudValueIsFinite(opticalDepthResiduals.x)
            ?opticalDepthResiduals.x:0.0,
        cloudValueIsFinite(opticalDepthResiduals.y)
            ?opticalDepthResiduals.y:0.0,
        cloudValueIsFinite(opticalDepthResiduals.z)
            ?opticalDepthResiduals.z:0.0,
        cloudValueIsFinite(opticalDepthResiduals.w)
            ?opticalDepthResiduals.w:0.0);
    float4 residualExponents=clamp(
        -safeOpticalDepthResiduals,-16.0.xxxx,16.0.xxxx);
    return saturate(safePathTransmittance*exp(residualExponents));
}
// 0～1のR16F透過率を丸めたときの最大誤差を返す。非正規化領域では固定の
// 2^-25、正規化領域では指数に対応する半ULPとなる。
float cloudR16TransmittanceHalfUlp(float visibility){
    const float minimumNormal=0.00006103515625;
    const float maximumBelowOne=0.99951171875;
    const float subnormalHalfUlp=0.0000000298023223876953125;
    float magnitude=clamp(
        abs(visibility),minimumNormal,maximumBelowOne);
    return max(
        exp2(floor(log2(magnitude))-11.0),subnormalHalfUlp);
}
// 正の有限値を32単精度表現だけ上へ広げ、積和と指数演算の丸め誤差を
// 残差上限へ含める。最近接丸めの結果だけでは境界を越えない保証にならない。
float cloudInflatePositiveFloatUpper(float value){
    const uint maximumFiniteCode=0x7f7fffffu;
    const uint roundingMargin=32u;
    float result=3.402823466e38;
    bool valid=value==value&&value>=0.0&&value<=3.402823466e38;
    if(valid){
        if(value==0.0)
            result=0.0;
        else{
            uint code=asuint(value);
            if(code<maximumFiniteCode-roundingMargin)
                result=asfloat(code+roundingMargin);
        }
    }
    return result;
}
float3 cloudInflatePositiveFloatUpper3(float3 value){
    return float3(
        cloudInflatePositiveFloatUpper(value.x),
        cloudInflatePositiveFloatUpper(value.y),
        cloudInflatePositiveFloatUpper(value.z));
}
float cloudDeflatePositiveFloatLower(float value){
    const uint roundingMargin=32u;
    float result=0.0;
    bool valid=value==value&&value>0.0&&value<=3.402823466e38;
    if(valid){
        uint code=asuint(value);
        if(code>roundingMargin)
            result=asfloat(code-roundingMargin);
    }
    return result;
}
float3 cloudDeflatePositiveFloatLower3(float3 value){
    return float3(
        cloudDeflatePositiveFloatLower(value.x),
        cloudDeflatePositiveFloatLower(value.y),
        cloudDeflatePositiveFloatLower(value.z));
}
// 1付近の単精度値で8表現分を一輸送区間の演算誤差として予約する。
// 吸収率自体は物理上限へ制限済みなので、指数、減算、乗算、加算を覆う。
static const int CLOUD_BEER_TRANSPORT_SEGMENT_COUNT=8;
static const int CLOUD_SHELL_COMPONENT_COUNT=2;
static const float CLOUD_TRANSPORT_ROUNDING_ERROR_PER_SEGMENT=
    0.00000095367431640625;
float cloudPositiveProductUpper(float left,float right){
    bool valid=left==left&&right==right
        &&left>=0.0&&right>=0.0
        &&left<=3.402823466e38&&right<=3.402823466e38;
    float result=3.402823466e38;
    if(valid){
        result=left==0.0||right==0.0
            ?0.0:cloudInflatePositiveFloatUpper(left*right);
    }
    return result;
}
float3 cloudPositiveProductUpper3(float left,float3 right){
    return cloudInflatePositiveFloatUpper3(left*right);
}
float cloudPositiveSumUpper(float left,float right){
    bool valid=left==left&&right==right
        &&left>=0.0&&right>=0.0
        &&left<=3.402823466e38&&right<=3.402823466e38;
    float result=3.402823466e38;
    if(valid)
        result=cloudInflatePositiveFloatUpper(left+right);
    return result;
}
float3 cloudPositiveSumUpper3(float3 left,float3 right){
    return float3(
        cloudPositiveSumUpper(left.x,right.x),
        cloudPositiveSumUpper(left.y,right.y),
        cloudPositiveSumUpper(left.z,right.z));
}
float cloudPositiveDifferenceUpper(float upperEnd,float currentEnd){
    bool valid=cloudValueIsFinite(upperEnd)
        &&cloudValueIsFinite(currentEnd)
        &&upperEnd>=0.0&&currentEnd>=0.0;
    float result=3.402823466e38;
    if(valid)
        result=cloudInflatePositiveFloatUpper(
            max(upperEnd-currentEnd,0.0));
    return result;
}
uint cloudRemainingTransportSegmentCount(
    int currentFineCellCount,int nextFineCellIndex,
    bool hasNextInterval,int nextFineCellCount){
    uint currentFineCellCountUnsigned=
        uint(max(currentFineCellCount,0));
    uint nextFineCellIndexUnsigned=
        uint(max(nextFineCellIndex,0));
    uint currentRemainingCellCount=
        nextFineCellIndexUnsigned<currentFineCellCountUnsigned
            ?currentFineCellCountUnsigned-nextFineCellIndexUnsigned:0u;
    uint nextRemainingCellCount=hasNextInterval
        ?uint(max(nextFineCellCount,0)):0u;
    uint segmentCountPerCell=
        uint(CLOUD_BEER_TRANSPORT_SEGMENT_COUNT)
        *uint(CLOUD_SHELL_COMPONENT_COUNT);
    uint maximumCellCount=0xffffffffu/segmentCountPerCell;
    uint segmentCount=0xffffffffu;
    uint remainingCellCapacity=0u;
    bool currentCountFits=
        currentRemainingCellCount<=maximumCellCount;
    if(currentCountFits)
        remainingCellCapacity=
            maximumCellCount-currentRemainingCellCount;
    bool nextCountFits=currentCountFits&&
        nextRemainingCellCount<=remainingCellCapacity;
    if(nextCountFits)
        segmentCount=
            (currentRemainingCellCount+nextRemainingCellCount)
            *segmentCountPerCell;
    return segmentCount;
}
// 残り散乱重みは指数差を使わず、1-exp(-x)<=min(x,1)と
// 消散縮小時の飽和値 contribution/occlusion から直接包む。
float cloudTransportWeightUpper(
    float transmittance,float primaryOpticalDepthUpper,
    float contribution,float occlusion,uint remainingSegmentCount){
    bool valid=transmittance==transmittance
        &&primaryOpticalDepthUpper==primaryOpticalDepthUpper
        &&contribution==contribution&&occlusion==occlusion
        &&transmittance>=0.0&&primaryOpticalDepthUpper>=0.0
        &&contribution>=0.0&&occlusion>=0.0
        &&transmittance<=3.402823466e38
        &&primaryOpticalDepthUpper<=3.402823466e38
        &&contribution<=3.402823466e38
        &&occlusion<=3.402823466e38;
    float result=3.402823466e38;
    if(valid){
        float arithmeticMargin=cloudPositiveProductUpper(
            float(remainingSegmentCount),
            CLOUD_TRANSPORT_ROUNDING_ERROR_PER_SEGMENT);
        float effectiveOpticalDepth=cloudPositiveSumUpper(
            primaryOpticalDepthUpper,arithmeticMargin);
        float reducedWeight=cloudPositiveProductUpper(
            contribution,effectiveOpticalDepth);
        if(occlusion>0.0){
            float saturatedWeight=cloudInflatePositiveFloatUpper(
                contribution/occlusion);
            reducedWeight=min(reducedWeight,saturatedWeight);
        }
        float transportedWeight=cloudPositiveProductUpper(
            transmittance,reducedWeight);
        float accumulationScale=cloudPositiveSumUpper(
            1.0,arithmeticMargin);
        result=cloudPositiveProductUpper(
            transportedWeight,accumulationScale);
    }
    return result;
}
// 現在値を挟む下限・上限の全域が、R16Fで同じ値へ丸められるか返す。
// 非乗算済み色は残り不透明度で暗くなる場合と、残り散乱で明るくなる場合の両方を見る。
bool cloudR16ValueRangeKeepsCode(float currentValue,float lowerValue,float upperValue){
    bool finiteInput=currentValue==currentValue
        &&lowerValue==lowerValue&&upperValue==upperValue
        &&currentValue>=0.0&&lowerValue>=0.0
        &&upperValue>=currentValue&&upperValue<=65504.0;
    bool result=false;
    if(finiteInput){
        uint currentCode=f32tof16(currentValue);
        result=currentCode==f32tof16(lowerValue)
            &&currentCode==f32tof16(upperValue);
    }
    return result;
}
// 非負値は単精度のビット順と数値順が一致する。残差後の上限が同じ
// R32F表現なら、保存する不透明度または深度は厳密に変わらない。
bool cloudR32PositiveRangeKeepsCode(float currentValue,float upperValue){
    bool finiteInput=currentValue==currentValue&&upperValue==upperValue
        &&currentValue>=0.0&&upperValue>=currentValue
        &&upperValue<=250000.0;
    return finiteInput&&asuint(currentValue)==asuint(upperValue);
}
// 負の詳細残差で透過率を明るく戻すと、保存時の半ULPも同じ指数で増幅される。
// 補正後の値をR16Fへ直接保存した場合の半ULPと比較し、元の量子化誤差がそれを
// 越える分だけ正確積分へ連続的に移す。残差0では比が厳密に1へ戻る。
float cloudAmplifiedR16VisibilityReliability(
    float visibility,float opticalDepthResidual){
    float reliability=1.0;
    if(visibility<1.0){
        float amplification=exp(clamp(
            -opticalDepthResidual,0.0,16.0));
        float correctedVisibility=saturate(
            max(visibility,0.0)*amplification);
        float amplifiedHalfUlp=
            cloudR16TransmittanceHalfUlp(visibility)*amplification;
        float correctedHalfUlp=
            cloudR16TransmittanceHalfUlp(correctedVisibility);
        reliability=saturate(
            correctedHalfUlp/max(amplifiedHalfUlp,1e-30));
    }
    return reliability;
}
// 12本の次数別太陽光路のうち最も不確かな値へ合わせる。残差の符号だけでは
// 分岐せず、実際の消散とR16Fの量子化誤差から連続信頼度を求める。
float cloudSunDepthResidualCacheReliability(
    float4 firstVisibility,float4 secondVisibility,
    float4 thirdVisibility,
    float4 firstOpticalDepthResiduals,
    float4 secondOpticalDepthResiduals,
    float4 thirdOpticalDepthResiduals){
    bool validInput=!any(
            firstOpticalDepthResiduals!=firstOpticalDepthResiduals)
        &&!any(secondOpticalDepthResiduals!=secondOpticalDepthResiduals)
        &&!any(thirdOpticalDepthResiduals!=thirdOpticalDepthResiduals)
        &&all(abs(firstOpticalDepthResiduals)<=3.0e38.xxxx)
        &&all(abs(secondOpticalDepthResiduals)<=3.0e38.xxxx)
        &&all(abs(thirdOpticalDepthResiduals)<=3.0e38.xxxx)
        &&!any(firstVisibility!=firstVisibility)
        &&!any(secondVisibility!=secondVisibility)
        &&!any(thirdVisibility!=thirdVisibility);
    float reliability=0.0;
    if(validInput){
        reliability=1.0;
        [unroll] for(uint directionIndex=0u;directionIndex<4u;++directionIndex){
            reliability=min(reliability,
                cloudAmplifiedR16VisibilityReliability(
                    firstVisibility[directionIndex],
                    firstOpticalDepthResiduals[directionIndex]));
            reliability=min(reliability,
                cloudAmplifiedR16VisibilityReliability(
                    secondVisibility[directionIndex],
                    secondOpticalDepthResiduals[directionIndex]));
            reliability=min(reliability,
                cloudAmplifiedR16VisibilityReliability(
                    thirdVisibility[directionIndex],
                    thirdOpticalDepthResiduals[directionIndex]));
        }
    }
    return reliability;
}
// 太陽の見かけの半径は地球近傍で約0.00465 rad。円盤の二次モーメントへ
// 合う半径R/sqrt(2)の4方向を用意し、同じ始点から全方向を独立して積分する。
static const uint CLOUD_SUN_DISK_DIRECTION_COUNT=4u;

// CPU側で作った正規直交基底へ太陽面の各積分点を写し、各点を独立した
// 直線光路として扱う。円盤の4点は中心対称なので、光量に片寄りを作らない。
float3 cloudSunDiskDirection(
    float3 sun,float3 lightTangent,float3 lightBitangent,uint sampleIndex){
    float axisOffset=0.5*max(shadowState.y,0.0);
    uint boundedIndex=sampleIndex&(CLOUD_SUN_DISK_DIRECTION_COUNT-1u);
    // 二つの添字ビットから中心対称な四符号を復元する。動的な定数配列添字を
    // 無くすことで、四本の重い光路を式展開せず通常ループとして実行できる。
    float signX=((boundedIndex^(boundedIndex>>1u))&1u)==0u?1.0:-1.0;
    float signY=(boundedIndex&1u)==0u?1.0:-1.0;
    float2 offset=axisOffset*float2(signX,signY);
    float3 lateral=offset.x*lightTangent+offset.y*lightBitangent;
    return (sun+lateral)*rsqrt(1.0+dot(offset,offset));
}

static const float CLOUD_PLANET_RADIUS=6360000.0;
static const float CLOUD_RAY_END_LIMIT=2.0*CLOUD_PLANET_RADIUS;
// b*b-c は接線でほぼ同値の二項を引く。二進32bitの八機械イプシロンを
// 外殻の接線候補だけに使い、丸め誤差で薄い雲を失うことを防ぐ。
// 内殻の負判別式を接線へ丸めると、雲層内の連続区間を途中で切るため適用しない。
static const float CLOUD_SHELL_DISCRIMINANT_RELATIVE_TOLERANCE=
    9.5367431640625e-7;
// CPU側の kVolumetricCloudShadowCacheHeight と一致させる。一つのスレッドが縦列を完結させる。
static const uint CLOUD_SHADOW_CACHE_HEIGHT=32u;
// 一つの所有テクスチャへ、周囲光と一次・二次・三次の太陽透過率を縦に並べる。
static const uint CLOUD_SHADOW_CACHE_TEXTURE_HEIGHT=
    4u*CLOUD_SHADOW_CACHE_HEIGHT;
// 4x1x4スレッドが同じ水平セルの16標本を担当し、各自の32区間を保持する。
// 可視率は16bit整数の成分別総和にして、外周で増やした部分標本も平均できるようにする。
static const uint CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT=16u;
static const uint CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS=4u;
static const uint CLOUD_SUN_CACHE_GROUP_THREAD_COUNT=4u;
groupshared float2 cloudShadowColumnSegmentDepths[
    CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT*CLOUD_SHADOW_CACHE_HEIGHT];
groupshared uint4 cloudAmbientQuantizedVisibilitySums[
    CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT*CLOUD_SHADOW_CACHE_HEIGHT];
groupshared float3 cloudSunVisibilityProfiles[
    CLOUD_SUN_CACHE_GROUP_THREAD_COUNT*CLOUD_SHADOW_CACHE_HEIGHT];
// 線形補間が範囲外を参照しない境界と、完全なキャッシュ値へ到達する境界。
static const float CLOUD_SHADOW_CACHE_FILTER_START_CELLS=1.5;
static const float CLOUD_SHADOW_CACHE_FILTER_FULL_CELLS=2.5;
// 中心から片側16画素は500 m間隔を保ち、CPUの中心追従半径8 kmと一致させる。
static const float CLOUD_AMBIENT_CACHE_UNIFORM_RADIUS_CELLS=16.0;
// 光路の最大標本数。実際の区間数は形状の相関長から毎回求める。
static const int CLOUD_LIGHT_MARCH_SAMPLE_COUNT=16;
static const int CLOUD_LIGHT_DETAIL_SAMPLE_COUNT=3;
// CPU公開値 kVolumetricCloudMinViewSteps と一致させ、各有効帯の最低探索量に使う。
static const int CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT=32;
// 採取数を距離で切り替えず、固定4点Gauss-Legendre求積で密度と一次距離モーメントを得る。
// CPUの負荷計画も4点を使い、文字列契約試験でGPU側だけの値ずれを検出する。
static const int CLOUD_DENSITY_GAUSS_SAMPLE_COUNT=4;
static const float4 CLOUD_DENSITY_GAUSS_FRACTIONS=float4(
    0.0694318442,0.3300094782,0.6699905218,0.9305681558);
static const float4 CLOUD_DENSITY_GAUSS_WEIGHTS=float4(
    0.1739274226,0.3260725774,0.3260725774,0.1739274226);
// 隣接Gauss点の最大間隔。1/4区間では中央二点の0.339981区間を覆えず、
// その間に残る高周波を解像済みと誤判定するため、担当幅の保守的な上限に使う。
static const float CLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION=0.3399810436;
// 旧FXCが動的な定数ベクトル添字を外側視線ループの展開要求へ変換しないよう、
// 四つのGauss点だけを固定成分から選ぶ。
float cloudDensityGaussFractionAt(int sampleIndex){
    float fraction=CLOUD_DENSITY_GAUSS_FRACTIONS.x;
    if(sampleIndex==1) fraction=CLOUD_DENSITY_GAUSS_FRACTIONS.y;
    else if(sampleIndex==2) fraction=CLOUD_DENSITY_GAUSS_FRACTIONS.z;
    else if(sampleIndex>=3) fraction=CLOUD_DENSITY_GAUSS_FRACTIONS.w;
    return fraction;
}
// 八つの解析区間境界をGauss点と重みから直接作る。密度標本ごとの二区間幅が
// 対応するGauss重みへ必ず一致し、点と輸送幅の二重記述を残さない。
float cloudBeerTransportBoundaryAt(int boundaryIndex){
    float boundary=0.0;
    if(boundaryIndex==1) boundary=CLOUD_DENSITY_GAUSS_FRACTIONS.x;
    else if(boundaryIndex==2) boundary=CLOUD_DENSITY_GAUSS_WEIGHTS.x;
    else if(boundaryIndex==3) boundary=CLOUD_DENSITY_GAUSS_FRACTIONS.y;
    else if(boundaryIndex==4) boundary=
        CLOUD_DENSITY_GAUSS_WEIGHTS.x+CLOUD_DENSITY_GAUSS_WEIGHTS.y;
    else if(boundaryIndex==5) boundary=CLOUD_DENSITY_GAUSS_FRACTIONS.z;
    else if(boundaryIndex==6) boundary=1.0-CLOUD_DENSITY_GAUSS_WEIGHTS.w;
    else if(boundaryIndex==7) boundary=CLOUD_DENSITY_GAUSS_FRACTIONS.w;
    else if(boundaryIndex>=8) boundary=1.0;
    return boundary;
}
// 雲殻出口までの低周波光路をキャッシュし、信頼度が不足する場所は同じ固定積分へ戻す。
static const bool CLOUD_MAIN_SHADOW_CACHE_ENABLED=true;
// 視線標本は接平面原点から250 km以内、広域環境光キャッシュの外端標本は最大約426 km以内にある。
// そのため xz^2/(R+y)^2 は0.0045未満となる。4次展開により標本ごとの平方根を省きながら、
// 対応する全領域で雲殻高度の誤差を4 cm未満へ保つ。
float cloudAltitude(float3 p){
    float3 local=p-worldOrigin.xyz;
    float radialY=max(CLOUD_PLANET_RADIUS+local.y,1.0);
    float radialXz2=dot(local.xz,local.xz);
    float inverseRadialY=1.0/radialY;
    float q=radialXz2*inverseRadialY;
    return local.y+q*(0.5-q*inverseRadialY*0.125);
}
// 雲層内のカメラでは、地平線より下の視線にもカメラ直前の雲が存在する。
// 下層・上層のどちらに入っているかを一度だけ判定し、地面の地平線除外を後回しにする。
bool cloudCameraInsideCloudLayer(){
    float cameraAltitude=cloudAltitude(camPos.xyz);
    bool insideLower=cameraAltitude>=layer.x&&cameraAltitude<layer.y;
    bool insideUpper=cloudUpperLayer.w>0.5&&
        cameraAltitude>=cloudUpperLayer.x&&cameraAltitude<cloudUpperLayer.y;
    return insideLower||insideUpper;
}
// 地上視点でだけ地面の地平線より下を除外する。雲層内または雲層より上では、
// 下向きの視線が雲を通過して地面へ向かうため、地平線除外を適用しない。
bool cloudCameraBelowCloudBase(){
    return cloudAltitude(camPos.xyz)<layer.x;
}
// 上層に居るか。高度だけで決まるので、視線側と光側で同じ判定になる。
bool inUpperCloudBandFromAltitude(float altitude){
    return cloudUpperLayer.w>0.5 && altitude>=cloudUpperLayer.x;
}
// 消散係数はm^-1であり、層厚で割らない。同じ密度なら実際に通過した距離に比例して
// 光学的深さが増える。上下層は同じ基準を持つが、定数バッファーの既存配置を保って選ぶ。
float cloudOpticalDepthScaleFromBand(bool upperBand){
    float scale=layer.w;
    if(upperBand) scale=cloudUpperTerms.z;
    return scale;
}
// 層の中での高さ (0=底, 1=天井)。上層に居るなら上層の中で測る。
//
// 2層の間では下層の1.0へ貼り付いてcloudProfileが0を返す。歩進側はこの晴天域を
// 独立した交差区間の間として飛ばし、ここでは密度の安全な0だけを保証する。
float heightFractionFromAltitude(float altitude,bool upperBand){
    // FXC が分岐内の即時 return を未初期化扱いすることがあるため、下層の値で先に初期化し、
    // 上層だけを上書きして一つの経路から返す。高さの式と飽和処理の順序は変えない。
    float height=(altitude-layer.x)*cloudFrameTerms.w;
    if(upperBand)
        height=(altitude-cloudUpperLayer.x)*cloudUpperLayer.z;
    return saturate(height);
}
float heightFraction(float3 p){
    float altitude=cloudAltitude(p);
    return heightFractionFromAltitude(
        altitude,inUpperCloudBandFromAltitude(altitude));
}

// Solve against a shell altitude without subtracting two ~R^2 values. The
// factorized c term and q-form roots remain stable at an Earth-sized radius.
bool sphereRootsFromTerms(float b,float c,bool acceptRoundedOuterTangent,
                          out float nearT,out float farT){
    nearT=0.0;
    farT=0.0;
    float bSquared=b*b;
    float disc=bSquared-c;
    float discriminantTolerance=max(
        abs(bSquared)+abs(c),1.0)
        *CLOUD_SHELL_DISCRIMINANT_RELATIVE_TOLERANCE;
    bool hit=disc>=0.0||(
        acceptRoundedOuterTangent&&disc>=-discriminantTolerance);
    if(hit){
        float s=sqrt(max(disc,0.0));
        float q=-b-(b>=0.0?s:-s);
        float rootA=q;
        float rootB=abs(q)>1e-5?c/q:-b+s;
        nearT=min(rootA,rootB);
        farT=max(rootA,rootB);
    }
    return hit;
}

// 一つの曲面雲層について、正の距離にある最初の連続区間を返す。
// 地面による遮蔽と最大距離は呼び出し側で扱い、層の下・中・上では同じ交差規則を使う。
bool intersectNearestCloudShellTerms(float b,float innerC,float outerC,out float t0,out float t1){
    t0=0.0;
    t1=0.0;
    float outerNear=0.0,outerFar=0.0;
    bool hitsOuter=sphereRootsFromTerms(
        b,outerC,true,outerNear,outerFar);
    if(hitsOuter && outerFar>0.0){
        float innerNear=0.0,innerFar=0.0;
        // 内殻の負判別式は実際のmissとして扱う。ここを接線へ丸めると、雲層内から
        // 内殻をわずかに外すレイを偽の終端にして、その先の雲路を失う。
        bool hitsInner=sphereRootsFromTerms(
            b,innerC,false,innerNear,innerFar);
        if(innerC<0.0){
            if(hitsInner && innerFar>0.0){
                t0=max(innerFar,0.0);
                t1=outerFar;
            }
        } else if(outerC<=0.0){
            bool headingInward=b<0.0;
            // 境界上では内側交差の距離が0になる。内向きなら、雲へ入る前に
            // 内殻へ接しているだけなので、0を有効な終端として扱う。
            t1=(headingInward && hitsInner && innerNear>=0.0)?innerNear:outerFar;
        } else if(outerNear>0.0){
            t0=outerNear;
            t1=(hitsInner && innerNear>t0)?innerNear:outerFar;
        }
    }
    return t1>t0;
}

// 下層と上層の交差区間を視点から近い順へ詰め、物理雲帯IDも同じ順序で保持する。
// 走査順をIDにすると、片方の区間が消えた瞬間に残った雲帯の採取位相が変わる。
int packCloudBandIntervals(bool lowerHit,float2 lowerInterval,bool upperHit,float2 upperInterval,out float4 intervals,out int2 bandIds){
    intervals=float4(0,0,0,0);
    bandIds=int2(0,0);
    int bandCount=0;
    if(lowerHit&&upperHit){
        bool lowerFirst=lowerInterval.x<=upperInterval.x;
        intervals=lowerFirst
            ?float4(lowerInterval,upperInterval)
            :float4(upperInterval,lowerInterval);
        bandIds=lowerFirst?int2(0,1):int2(1,0);
        bandCount=2;
    }else if(lowerHit){
        intervals.xy=lowerInterval;
        bandIds.x=0;
        bandCount=1;
    }else if(upperHit){
        intervals.xy=upperInterval;
        bandIds.x=1;
        bandCount=1;
    }
    return bandCount;
}

// カメラ固定の主描画では、CPUで先行計算した下層・上層の係数を使う。
int intersectCloudBands(float3 rayDir,out float4 intervals,out int2 bandIds){
    float b=dot(cloudShellRayOrigin.xyz,rayDir);
    float2 lowerInterval=float2(0,0);
    bool lowerHit=intersectNearestCloudShellTerms(b,cloudShellRayOrigin.w,cloudShellTerms.x,lowerInterval.x,lowerInterval.y);
    float2 upperInterval=float2(0,0);
    bool upperHit=false;
    if(cloudUpperLayer.w>0.5){
        upperHit=intersectNearestCloudShellTerms(b,cloudShellTerms.y,cloudShellTerms.z,upperInterval.x,upperInterval.y);
    }
    return packCloudBandIntervals(lowerHit,lowerInterval,upperHit,upperInterval,intervals,bandIds);
}

// 正方向と惑星表面終端で切った区間を、近い順の最大二本へ追加する。
void appendCloudShellInterval(
    float2 candidate,float rayEnd,
    inout float4 intervals,inout int intervalCount){
    if(intervalCount>=2||rayEnd<=0.0) return;
    float2 clipped=float2(
        max(candidate.x,0.0),min(candidate.y,rayEnd));
    if(clipped.y<=clipped.x) return;
    if(intervalCount==0) intervals.xy=clipped;
    else intervals.zw=clipped;
    intervalCount++;
}

// 始点から最初の惑星表面までを、全サブレイと全雲帯に共通する物理終端とする。
float cloudPlanetRayEnd(float b,float groundC){
    // 惑星半径の二倍は、惑星近傍から外へ向かう光線が取り得る最大弦長。
    // 無限大の番兵値は後段の加算で丸め警告と未定義な最適化経路を生むため使わない。
    float rayEnd=CLOUD_RAY_END_LIMIT;
    float groundNear=0.0,groundFar=0.0;
    if(groundC<0.0){
        rayEnd=0.0;
    }else{
        bool hitsGround=sphereRootsFromTerms(
            b,groundC,false,groundNear,groundFar);
        if(hitsGround&&groundNear>0.0)
            rayEnd=groundNear;
        else if(hitsGround&&groundC<=0.0&&b<0.0)
            rayEnd=0.0;
    }
    return rayEnd;
}

// 一つの球殻が正方向に作る近側・遠側の両区間を保持する。
int intersectCloudShellTermsAll(
    float b,float innerC,float outerC,float rayEnd,
    out float4 intervals){
    intervals=0.0.xxxx;
    int intervalCount=0;
    float outerNear=0.0,outerFar=0.0;
    bool hitsOuter=sphereRootsFromTerms(
        b,outerC,true,outerNear,outerFar);
    if(!hitsOuter||outerFar<=0.0||rayEnd<=0.0)
        return intervalCount;
    float innerNear=0.0,innerFar=0.0;
    bool hitsInner=sphereRootsFromTerms(
        b,innerC,false,innerNear,innerFar);
    if(hitsInner){
        appendCloudShellInterval(
            float2(outerNear,innerNear),rayEnd,
            intervals,intervalCount);
        appendCloudShellInterval(
            float2(innerFar,outerFar),rayEnd,
            intervals,intervalCount);
    }else{
        appendCloudShellInterval(
            float2(outerNear,outerFar),rayEnd,
            intervals,intervalCount);
    }
    return intervalCount;
}

// 2×2 Gauss-Legendre位置を通る、画素中心とは独立した4本の実視線方向。
struct CloudPhysicalSubrayDirections {
    float3 lane0;
    float3 lane1;
    float3 lane2;
    float3 lane3;
};

// xyに近側、zwに遠側の区間を、物理雲帯ごとに保持する。
struct CloudBandIntervalSet {
    float4 lower;
    float4 upper;
};

struct CloudSubrayBandIntervals {
    CloudBandIntervalSet lane0;
    CloudBandIntervalSet lane1;
    CloudBandIntervalSet lane2;
    CloudBandIntervalSet lane3;
};

// 動的添字で構造体配列を作らず、現在処理する実サブレイ成分だけを選ぶ。
float4 cloudPhysicalSubraySelector(int laneIndex){
    return float4(
        laneIndex==0?1.0:0.0,
        laneIndex==1?1.0:0.0,
        laneIndex==2?1.0:0.0,
        laneIndex==3?1.0:0.0);
}

float3 cloudPhysicalSubrayDirectionAt(
    CloudPhysicalSubrayDirections directions,int laneIndex){
    float3 direction=directions.lane0;
    if(laneIndex==1) direction=directions.lane1;
    else if(laneIndex==2) direction=directions.lane2;
    else if(laneIndex==3) direction=directions.lane3;
    return direction;
}

// 歩進対象となる最大四区間を距離順に保持する。
struct CloudPackedBandIntervals {
    float4 starts;
    float4 ends;
    int4 bandIds;
    int count;
};

// 一方向について、地表遮蔽後の全区間を物理雲帯ごとに返す。
CloudBandIntervalSet intersectCloudBandsUnpacked(
    float3 rayDir,float maximumDistance){
    CloudBandIntervalSet result;
    result.lower=0.0.xxxx;
    result.upper=0.0.xxxx;
    float b=dot(cloudShellRayOrigin.xyz,rayDir);
    float groundC=cloudShellRayOrigin.w
        +layer.x*(2.0*CLOUD_PLANET_RADIUS+layer.x);
    float rayEnd=min(
        cloudPlanetRayEnd(b,groundC),max(maximumDistance,0.0));
    intersectCloudShellTermsAll(
        b,cloudShellRayOrigin.w,cloudShellTerms.x,
        rayEnd,result.lower);
    if(cloudUpperLayer.w>0.5){
        intersectCloudShellTermsAll(
            b,cloudShellTerms.y,cloudShellTerms.z,
            rayEnd,result.upper);
    }
    return result;
}

// 無効区間を末尾へ送り、有効区間は始点と終点の順で決定論的に並べる。
void sortCloudBandIntervalCandidates(
    inout float2 left,inout float2 right){
    float leftKey=left.y>left.x?left.x:CLOUD_RAY_END_LIMIT;
    float rightKey=right.y>right.x?right.x:CLOUD_RAY_END_LIMIT;
    bool exchange=rightKey<leftKey
        ||(rightKey==leftKey&&right.y<left.y);
    if(exchange){
        float2 temporary=left;
        left=right;
        right=temporary;
    }
}

// 始点順の候補を包絡へ足し、最大の晴天間隔だけを二区間の境界として残す。
void includeCloudSortedBandInterval(
    float2 candidate,inout float currentEnd,
    inout float largestGap,inout float splitEnd,inout float splitStart){
    if(candidate.y<=candidate.x) return;
    float gap=max(candidate.x-currentEnd,0.0);
    if(gap>largestGap){
        largestGap=gap;
        splitEnd=currentEnd;
        splitStart=candidate.x;
    }
    currentEnd=max(currentEnd,candidate.y);
}

// 4実サブレイの最大8区間を固定ソートし、埋める晴天距離が最小となる
// 最大二区間へ縮約する。同じ最大間隔では近い側を残して結果を一意にする。
float4 cloudBandIntervalPairFromCandidates(
    float4 lane0,float4 lane1,float4 lane2,float4 lane3){
    float4 result=0.0.xxxx;
    float2 interval0=lane0.xy,interval1=lane0.zw;
    float2 interval2=lane1.xy,interval3=lane1.zw;
    float2 interval4=lane2.xy,interval5=lane2.zw;
    float2 interval6=lane3.xy,interval7=lane3.zw;
    sortCloudBandIntervalCandidates(interval0,interval1);
    sortCloudBandIntervalCandidates(interval2,interval3);
    sortCloudBandIntervalCandidates(interval4,interval5);
    sortCloudBandIntervalCandidates(interval6,interval7);
    sortCloudBandIntervalCandidates(interval0,interval2);
    sortCloudBandIntervalCandidates(interval1,interval3);
    sortCloudBandIntervalCandidates(interval4,interval6);
    sortCloudBandIntervalCandidates(interval5,interval7);
    sortCloudBandIntervalCandidates(interval1,interval2);
    sortCloudBandIntervalCandidates(interval5,interval6);
    sortCloudBandIntervalCandidates(interval0,interval4);
    sortCloudBandIntervalCandidates(interval3,interval7);
    sortCloudBandIntervalCandidates(interval1,interval5);
    sortCloudBandIntervalCandidates(interval2,interval6);
    sortCloudBandIntervalCandidates(interval1,interval4);
    sortCloudBandIntervalCandidates(interval3,interval6);
    sortCloudBandIntervalCandidates(interval2,interval4);
    sortCloudBandIntervalCandidates(interval3,interval5);
    sortCloudBandIntervalCandidates(interval3,interval4);
    if(interval0.y>interval0.x){
        float currentEnd=interval0.y;
        float largestGap=0.0;
        float splitEnd=0.0,splitStart=0.0;
        includeCloudSortedBandInterval(
            interval1,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval2,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval3,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval4,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval5,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval6,currentEnd,largestGap,splitEnd,splitStart);
        includeCloudSortedBandInterval(
            interval7,currentEnd,largestGap,splitEnd,splitStart);
        result=largestGap>0.0
            ?float4(interval0.x,splitEnd,splitStart,currentEnd)
            :float4(interval0.x,currentEnd,0.0,0.0);
    }
    return result;
}

void sortCloudBandIntervalPair(
    inout float2 left,inout int leftBandId,
    inout float2 right,inout int rightBandId){
    float leftKey=left.y>left.x?left.x:CLOUD_RAY_END_LIMIT;
    float rightKey=right.y>right.x?right.x:CLOUD_RAY_END_LIMIT;
    if(rightKey<leftKey){
        float2 temporaryInterval=left;
        left=right;
        right=temporaryInterval;
        int temporaryBandId=leftBandId;
        leftBandId=rightBandId;
        rightBandId=temporaryBandId;
    }
}

CloudPackedBandIntervals packCloudBandIntervalPairs(
    float4 lowerPair,float4 upperPair){
    float2 interval0=lowerPair.xy;
    float2 interval1=lowerPair.zw;
    float2 interval2=upperPair.xy;
    float2 interval3=upperPair.zw;
    int bandId0=0,bandId1=0,bandId2=1,bandId3=1;
    sortCloudBandIntervalPair(
        interval0,bandId0,interval1,bandId1);
    sortCloudBandIntervalPair(
        interval2,bandId2,interval3,bandId3);
    sortCloudBandIntervalPair(
        interval0,bandId0,interval2,bandId2);
    sortCloudBandIntervalPair(
        interval1,bandId1,interval3,bandId3);
    sortCloudBandIntervalPair(
        interval1,bandId1,interval2,bandId2);
    CloudPackedBandIntervals packed;
    packed.starts=float4(
        interval0.x,interval1.x,interval2.x,interval3.x);
    packed.ends=float4(
        interval0.y,interval1.y,interval2.y,interval3.y);
    packed.bandIds=int4(bandId0,bandId1,bandId2,bandId3);
    packed.count=0;
    if(interval0.y>interval0.x) packed.count++;
    if(interval1.y>interval1.x) packed.count++;
    if(interval2.y>interval2.x) packed.count++;
    if(interval3.y>interval3.x) packed.count++;
    return packed;
}

// 4実サブレイの交差を保持する。歩進包絡も同じ実光路だけから作り、
// 近側・遠側の両区間を距離順へ並べる。
CloudPackedBandIntervals intersectCloudSubrayBandUnion(
    CloudPhysicalSubrayDirections subrayDirections,
    float maximumDistance,
    out CloudSubrayBandIntervals subrayIntervals){
    subrayIntervals.lane0=intersectCloudBandsUnpacked(
        subrayDirections.lane0,maximumDistance);
    subrayIntervals.lane1=intersectCloudBandsUnpacked(
        subrayDirections.lane1,maximumDistance);
    subrayIntervals.lane2=intersectCloudBandsUnpacked(
        subrayDirections.lane2,maximumDistance);
    subrayIntervals.lane3=intersectCloudBandsUnpacked(
        subrayDirections.lane3,maximumDistance);
    float4 lowerPair=cloudBandIntervalPairFromCandidates(
        subrayIntervals.lane0.lower,subrayIntervals.lane1.lower,
        subrayIntervals.lane2.lower,subrayIntervals.lane3.lower);
    float4 upperPair=cloudBandIntervalPairFromCandidates(
        subrayIntervals.lane0.upper,subrayIntervals.lane1.upper,
        subrayIntervals.lane2.upper,subrayIntervals.lane3.upper);
    return packCloudBandIntervalPairs(lowerPair,upperPair);
}

float4 cloudBandIntervalSetById(
    CloudBandIntervalSet intervals,int physicalBandId){
    return physicalBandId>0?intervals.upper:intervals.lower;
}

// 近側または遠側の一連結成分だけを切り出し、実始点と実終点を保つ。
float2 cloudBandIntervalComponentOverlap(
    float4 intervals,int componentIndex,
    float segmentStart,float segmentEnd){
    float2 component=componentIndex>0?intervals.zw:intervals.xy;
    float overlapStart=max(segmentStart,component.x);
    float overlapEnd=min(segmentEnd,component.y);
    bool valid=component.y>component.x&&overlapEnd>overlapStart;
    return valid?float2(overlapStart,overlapEnd):0.0.xx;
}

// 遠側成分の入口を現在のセルが跨ぐレーンだけを返す。
float cloudBandSecondComponentEntryMask(
    float4 intervals,float segmentStart,float segmentEnd){
    bool secondValid=intervals.w>intervals.z;
    bool crossesEntry=segmentStart<=intervals.z&&segmentEnd>intervals.z;
    return secondValid&&crossesEntry?1.0:0.0;
}

struct CloudPhysicalSubraySegmentOverlaps {
    float4 starts;
    float4 ends;
};

CloudPhysicalSubraySegmentOverlaps cloudPhysicalSubrayBandOverlaps(
    CloudSubrayBandIntervals intervals,int physicalBandId,
    int componentIndex,float segmentStart,float segmentEnd){
    float2 lane0=cloudBandIntervalComponentOverlap(
        cloudBandIntervalSetById(intervals.lane0,physicalBandId),
        componentIndex,segmentStart,segmentEnd);
    float2 lane1=cloudBandIntervalComponentOverlap(
        cloudBandIntervalSetById(intervals.lane1,physicalBandId),
        componentIndex,segmentStart,segmentEnd);
    float2 lane2=cloudBandIntervalComponentOverlap(
        cloudBandIntervalSetById(intervals.lane2,physicalBandId),
        componentIndex,segmentStart,segmentEnd);
    float2 lane3=cloudBandIntervalComponentOverlap(
        cloudBandIntervalSetById(intervals.lane3,physicalBandId),
        componentIndex,segmentStart,segmentEnd);
    CloudPhysicalSubraySegmentOverlaps result;
    result.starts=float4(lane0.x,lane1.x,lane2.x,lane3.x);
    result.ends=float4(lane0.y,lane1.y,lane2.y,lane3.y);
    return result;
}

// 遠距離の極薄区間でも、重心から始点を逆算せず閉区間内へ写像する。
float cloudIntervalDistanceAtFraction(
    float intervalStart,float intervalEnd,float fraction){
    float distance=intervalStart+saturate(fraction)
        *(intervalEnd-intervalStart);
    return clamp(distance,intervalStart,intervalEnd);
}

// 遠側成分へ入るセルで、各実サブレイの有限相関状態を個別に切る。
float4 cloudPhysicalSubraySecondComponentEntryMasks(
    CloudSubrayBandIntervals intervals,int physicalBandId,
    float segmentStart,float segmentEnd){
    return float4(
        cloudBandSecondComponentEntryMask(
            cloudBandIntervalSetById(intervals.lane0,physicalBandId),
            segmentStart,segmentEnd),
        cloudBandSecondComponentEntryMask(
            cloudBandIntervalSetById(intervals.lane1,physicalBandId),
            segmentStart,segmentEnd),
        cloudBandSecondComponentEntryMask(
            cloudBandIntervalSetById(intervals.lane2,physicalBandId),
            segmentStart,segmentEnd),
        cloudBandSecondComponentEntryMask(
            cloudBandIntervalSetById(intervals.lane3,physicalBandId),
            segmentStart,segmentEnd));
}

// 任意の始点に対する曲面高度の二次方程式c項を、巨大な半径の二乗差を避けて求める。
float cloudShellCFromLocalPosition(float3 local,float altitude){
    return dot(local.xz,local.xz)
        +(local.y-altitude)
         *(2.0*CLOUD_PLANET_RADIUS+local.y+altitude);
}

// 任意の始点から近側・遠側の全雲殻区間を求め、惑星の裏側は地表で切る。
// 太陽光と立体物用雲影も、主視線と同じ曲面区間の定義を使う。
CloudPackedBandIntervals intersectCloudBandsFromPosition(
    float3 rayOrigin,float3 rayDir){
    float3 local=rayOrigin-worldOrigin.xyz;
    float3 centreOffset=float3(local.x,CLOUD_PLANET_RADIUS+local.y,local.z);
    float b=dot(centreOffset,rayDir);
    float groundC=cloudShellCFromLocalPosition(local,0.0);
    float rayEnd=cloudPlanetRayEnd(b,groundC);
    float4 lowerIntervals=0.0.xxxx;
    intersectCloudShellTermsAll(
        b,cloudShellCFromLocalPosition(local,layer.x),
        cloudShellCFromLocalPosition(local,layer.y),
        rayEnd,lowerIntervals);
    float4 upperIntervals=0.0.xxxx;
    if(cloudUpperLayer.w>0.5){
        intersectCloudShellTermsAll(
            b,cloudShellCFromLocalPosition(local,cloudUpperLayer.x),
            cloudShellCFromLocalPosition(local,cloudUpperLayer.y),
            rayEnd,upperIntervals);
    }
    return packCloudBandIntervalPairs(lowerIntervals,upperIntervals);
}

// 物理的な惑星直径を越えない有限区間長へ変換し、壊れた入力を空区間にする。
float cloudFiniteIntervalLength(float start,float end){
    float result=0.0;
    bool finiteEndpoints=cloudValueIsFinite(start)
        &&cloudValueIsFinite(end);
    if(finiteEndpoints&&end>start){
        float length=end-start;
        if(cloudValueIsFinite(length))
            result=min(length,CLOUD_RAY_END_LIMIT);
    }
    return result;
}

// 固定標本を最大四区間の占有距離比で分ける。標本数が区間数以上なら、
// 薄い区間にも必ず一標本を予約し、残りだけを累積距離の丸めで配分する。
int4 cloudPackedIntervalSampleCounts(
    CloudPackedBandIntervals intervals,int requestedSampleCount){
    int4 sampleCounts=int4(0,0,0,0);
    int intervalCount=clamp(intervals.count,0,4);
    int safeSampleCount=max(requestedSampleCount,0);
    if(intervalCount>0&&safeSampleCount>0){
        float4 intervalLengths=float4(
            cloudFiniteIntervalLength(
                intervals.starts.x,intervals.ends.x),
            cloudFiniteIntervalLength(
                intervals.starts.y,intervals.ends.y),
            cloudFiniteIntervalLength(
                intervals.starts.z,intervals.ends.z),
            cloudFiniteIntervalLength(
                intervals.starts.w,intervals.ends.w));
        if(intervalCount<4) intervalLengths.w=0.0;
        if(intervalCount<3) intervalLengths.z=0.0;
        if(intervalCount<2) intervalLengths.y=0.0;
        float occupiedLength=max(dot(intervalLengths,1.0.xxxx),1e-5);
        int reservedPerInterval=safeSampleCount>=intervalCount?1:0;
        int remainingSampleCount=
            safeSampleCount-reservedPerInterval*intervalCount;
        int previousTarget=0;
        float cumulativeLength=0.0;
        [unroll] for(int intervalIndex=0;intervalIndex<4;++intervalIndex){
            if(intervalIndex<intervalCount){
                cumulativeLength+=intervalLengths[intervalIndex];
                int target=intervalIndex==intervalCount-1
                    ?remainingSampleCount
                    :clamp(
                        (int)round(float(remainingSampleCount)
                            *cumulativeLength/occupiedLength),
                        previousTarget,remainingSampleCount);
                sampleCounts[intervalIndex]=reservedPerInterval
                    +max(target-previousTarget,0);
                previousTarget=target;
            }
        }
    }
    return sampleCounts;
}

// 固定標本の番号を担当区間の中央標本と物理帯IDへ写し、その区間だけの積分幅を返す。
// 晴天の層間は積分幅へ含めず、実光線上の開始距離としてだけ反映する。
bool cloudLightSampleTerms(
    CloudPackedBandIntervals intervals,int requestedSampleCount,int sampleIndex,
    out float rayDistance,out float sampleSpacing,out int sampleBandId){
    rayDistance=0.0;
    sampleSpacing=0.0;
    sampleBandId=-1;
    int4 sampleCounts=cloudPackedIntervalSampleCounts(
        intervals,requestedSampleCount);
    bool validSample=false;
    int sampleStart=0;
    [unroll] for(int intervalIndex=0;intervalIndex<4;++intervalIndex){
        int intervalSampleCount=sampleCounts[intervalIndex];
        int sampleEnd=sampleStart+intervalSampleCount;
        if(!validSample&&sampleIndex>=sampleStart&&sampleIndex<sampleEnd){
            int intervalSampleIndex=sampleIndex-sampleStart;
            float intervalLength=cloudFiniteIntervalLength(
                intervals.starts[intervalIndex],
                intervals.ends[intervalIndex]);
            sampleSpacing=intervalLength
                /float(max(intervalSampleCount,1));
            rayDistance=intervals.starts[intervalIndex]
                +(float(intervalSampleIndex)+0.5)*sampleSpacing;
            sampleBandId=intervals.bandIds[intervalIndex];
            validSample=cloudValueIsFinite(intervalLength)
                &&cloudValueIsFinite(sampleSpacing)
                &&cloudValueIsFinite(rayDistance)
                &&sampleSpacing>1e-4;
        }
        sampleStart=sampleEnd;
    }
    return validSample;
}
// 短い8点の光円すい内では雲種と降水量を共有できる。二つの雲種補間を明示して、
// 視線標本で一度だけ求めた同じ重みを各光標本へ再利用する。
float2 cloudProfileTypeWeights(float cloudType){
    return float2(
        smoothstep(0.18,0.52,cloudType),
        smoothstep(0.50,0.84,cloudType));
}
// 積雲の塔状成長を有効にする強さ。雲種だけでなく降水成分も見るため、
// 積乱雲は雲量と独立して選べる。通常の層積雲にはほぼ影響しない。
float cloudToweringStrength(float cloudType,float precipitation){
    // 通常の積雲まで塔へ変換すると、画面全体が縦長の柱で埋まる。
    // 雲種または降水が十分に高い場合だけ積乱雲の縦分布へ移行する。
    float typeTower=smoothstep(0.84,0.99,saturate(cloudType));
    float precipitationTower=smoothstep(0.25,0.85,saturate(precipitation));
    return max(typeTower,precipitationTower);
}
// 作者が指定した積乱雲の可能性を、低周波天候場と被覆中心から局所的な発達域へ分ける。
// 雲種や降水を100%上書きしても全ての雲塊を塔にせず、広い連続領域だけを成熟させる。
float cloudLocalToweringStrength(float4 weather,float cloudInterior){
    float authoredTower=cloudToweringStrength(weather.g,weather.b);
    // 広域の対流候補だけでは塔にせず、天候域の中心へ入った場所だけを成熟させる。
    // 外周まで同じ縦分布へすると一枚の壁になり、中心だけへ絞り過ぎると細柱になるため、
    // 両方の平滑遷移を残して発達域の幅を確保する。
    // 雲種・降水を明示した場合は、手続き天候場が低くても発達域の下限を残す。
    // これを持たないと全域を積乱雲へ設定しても通常雲の低い雲頂へ戻り、高さが失われる。
    float authoredFloor=0.45*authoredTower;
    float broadPotential=max(
        smoothstep(0.66,0.92,saturate(weather.a)),
        authoredFloor);
    float interiorPotential=smoothstep(0.50,0.96,saturate(cloudInterior));
    float localPotential=broadPotential*interiorPotential;
    // 対流域の外側では通常雲の高さへ戻し、全域指定時にも縁へ塔を残さない。
    // 局所候補を二乗すると中間の発達域がさらに狭まり、光路上で細い柱へ分断される。
    // 下限を保った平滑補間で、成熟域の上限を変えずに隣接する雲塊を連続させる。
    float coherentPotential=smoothstep(0.12,0.82,localPotential);
    return authoredTower*coherentPotential;
}
// 通常雲から積乱雲の縦分布へ移る割合を、同じ局所発達強度から求める。
float cloudStormProfileMix(float toweringStrength){
    return saturate(toweringStrength*0.92);
}
// 塔の上部だけ被覆しきい値を下げ、中心から横へ張り出すかなとこを作る。
// 追加の天候採取は行わず、局所発達強度を全描画経路で共有する。
float cloudAnvilCoverageExpansion(float layerHeight,float toweringStrength){
    float anvilBand=smoothstep(0.50,0.66,saturate(layerHeight))
                   *(1.0-smoothstep(0.80,0.97,saturate(layerHeight)));
    return 0.07*saturate(toweringStrength)*anvilBand;
}
// 積乱雲の中層だけ被覆しきい値を上げ、雲底から雲頂まで同じ輪郭が続く柱を避ける。
// 完成マスクへの乗算では正の領域が閉じないため、天候場の等値線そのものを内側へ移す。
float cloudConvectiveWaistThresholdOffset(float layerHeight,float toweringStrength){
    float waist=smoothstep(0.28,0.44,saturate(layerHeight))
               *(1.0-smoothstep(0.58,0.74,saturate(layerHeight)));
    return 0.018*saturate(toweringStrength)*waist;
}
// 柔らかく密な底面と列ごとにずれた上面により、切断された水平な棚を避ける。
// 呼び出し元は詳細侵食でも使う正規化高度を保持する。
// 各雲種の雲底は層厚の比率ではなく、基準層で意図した物理距離で本体密度へ立ち上げる。
// xyzは層雲・層積雲・積雲の約140・220・320 m、wは積乱雲の約180 mを表す。
// 雲底から数百mにわたって密度を増やすと3D雑音の山が逆円すいへ広がるため、
// 凝結境界だけを短く立ち上げ、雲体内部の厚みは基本形状へ任せる。
float4 cloudBaseRiseEnds(bool upperBand,float columnSpan){
    float inverseThickness=upperBand
        ?cloudUpperLayer.z:cloudFrameTerms.w;
    float4 layerRiseEnds=clamp(
        float4(140.0,220.0,320.0,180.0)*inverseThickness,
        float4(0.012,0.019,0.027,0.016),
        float4(0.070,0.110,0.160,0.090));
    // cloudProfile の高さは局所雲柱で0～1へ再配置済みなので、全球層に対する比率を
    // 局所雲柱の幅で割り、意図した物理距離を保つ。
    return min(
        layerRiseEnds/max(columnSpan,0.001),
        float4(0.95,0.95,0.95,0.95));
}
// 現在の雲種に対応する雲底立ち上がり幅を、物理距離から求めた各雲種の値で補間する。
float cloudProfileBaseRiseEnd(float cloudType,float toweringStrength,float columnSpan,bool upperBand){
    float4 riseEnds=cloudBaseRiseEnds(upperBand,columnSpan);
    float2 typeWeights=cloudProfileTypeWeights(cloudType);
    float lowCloudRise=lerp(riseEnds.x,riseEnds.y,typeWeights.x);
    lowCloudRise=lerp(lowCloudRise,riseEnds.z,typeWeights.y);
    return lerp(lowCloudRise,riseEnds.w,cloudStormProfileMix(toweringStrength));
}
float cloudProfileFromTypeWeights(
    float h,float2 typeWeights,float cloudType,float toweringStrength,
    float columnSpan,bool upperBand){
    float4 riseEnds=cloudBaseRiseEnds(upperBand,columnSpan);
    float4 rise=smoothstep(
        float4(0.0,0.0,0.0,0.0),
        riseEnds,h.xxxx);
    // 雲柱を局所高さへ再配置した後は、雲頂付近まで密度支持を残す。
    // 固定層座標の終端を使うと通常雲の上部だけが先に消え、薄い板や粒の列に見える。
    float4 fall=1.0-smoothstep(
        float4(0.38,0.72,0.62,0.94),
        float4(0.50,0.98,0.995,0.999),h.xxxx);
    float stratus=rise.x*fall.x;
    // 層積雲と積雲は凝結直後を少し薄くし、中層で本体密度へ連続的に達する。
    // 下端を最初から一様な密度にすると、3D形状の縦節が灰色の柱として残る。
    float stratocumulus=rise.y*fall.y
        *lerp(0.78,1.0,smoothstep(0.08,0.42,h));
    float cumulus=rise.z*fall.z
        *lerp(0.64,1.0,smoothstep(0.12,0.52,h));
    float profile=lerp(stratus,stratocumulus,typeWeights.x);
    profile=lerp(profile,cumulus,typeWeights.y);
    // 積乱雲本体は下部の胴体を厚く、中層から上部へ緩やかに細める。
    // 全高をほぼ一定密度で埋めると、3D形状を積分しても一枚の雲膜に見えるため、
    // 本体の立体的な減衰を先に作り、その上へかなとこを連続して重ねる。
    float stormRiseEnd=riseEnds.w;
    float stormRiseBegin=stormRiseEnd*0.20;
    float stormBody=smoothstep(stormRiseBegin,stormRiseEnd,h)
                   *(1.0-0.38*smoothstep(0.30,0.78,h))
                   *(1.0-smoothstep(0.78,0.995,h));
    // 胴体とかなとこの間は小さな肩でつなぎ、密度0の棚を作らない。
    float stormShoulder=smoothstep(0.42,0.56,h)
                       *(1.0-smoothstep(0.66,0.82,h))*0.08;
    float anvil=smoothstep(0.56,0.70,h)
               *(1.0-smoothstep(0.80,0.995,h))*0.24;
    // 本体が雲頂側で連続しているため、かなとこは別の棚にならず不足部分だけを補う。
    float storm=saturate(
        stormBody+(stormShoulder+anvil)*(1.0-stormBody));
    float stormMix=cloudStormProfileMix(toweringStrength);
    return saturate(lerp(profile,storm,stormMix));
}
float cloudProfile(
    float h,float cloudType,float toweringStrength,float columnSpan,
    bool upperBand){
    return cloudProfileFromTypeWeights(
        h,cloudProfileTypeWeights(cloudType),cloudType,toweringStrength,
        columnSpan,upperBand);
}
// 既に採取した二つの天候模様で時間位相の向きと強さを場所ごとに変える。
// 全地点へ同じ位相を足したときの一様な上下動を避け、追加のテクスチャ採取は行わない。
float cloudLocalConvectionPhase(float4 weather){
    float warpPattern=smoothstep(0.36,0.64,weather.a)*2.0-1.0;
    float typePattern=weather.g*2.0-1.0;
    return dot(cloudEvolution.xy,float2(warpPattern,typePattern));
}
// 空と濃い中心を固定し、雲縁の被覆だけを場所ごとに成長または消散させる。
// 既存の時間位相と天候模様だけを使うため、テクスチャ採取は増えない。
float cloudWeatherCoverageEvolution(float4 weather){
    float2 localPattern=float2(weather.a,weather.g)*2.0-1.0;
    float slowPhase=cloudLocalConvectionPhase(weather);
    float finePhase=dot(cloudEvolution.zw,localPattern.yx);
    float edgeBase=weather.r*(1.0-weather.r);
    float edgeResponse=16.0*edgeBase*edgeBase;
    return clamp(
        slowPhase*0.38+finePhase*0.32,-0.14,0.14)*edgeResponse;
}
// bake 済み volume は tile あたり 4..32 cells を既に含む。world frequency を下げ、
// 小さな blob の反復ではなく連続した cloud bank を作る。
float cloudShapeScale(){
    // CPU側で layer.z * 0.0030 を 0.00004～0.00020 に収め、1フレームに一度だけ求めた倍率を使う。
    return cloudFrameTerms.z;
}
// 基本形状が雲帯の底から上端までに進む、物理距離基準の3D領域幅を返す。
// 焼き込み体積の周波数をここで数え直さず、横と高さへ同じメートル尺度を使う。
float cloudShapeVerticalSpan(bool upperBand){
    float inverseThickness=upperBand
        ?cloudUpperLayer.z:cloudFrameTerms.w;
    return cloudShapeScale()/max(inverseThickness,1e-6);
}
float2 cloudWindWorld(){
    // CPU側で風の移動距離を同じ方向へ射影し、フレームごとに一度だけ求める。
    return cloudFrameTerms.xy;
}
// 2Dの天候包絡は柱ごとに固定したまま、3D密度形状だけを高度に応じて風下へ傾ける。
// 層の下端から上端までに850 mずらし、上下断面を同じ風向へ連続的に接続する。
float2 cloudHeightShapeShear(float layerHeight,bool upperBand){
    float bandScale=upperBand?0.25:1.0;
    return float2(0.9284767,0.3713907)
          *(850.0*saturate(layerHeight)*bandScale);
}
// 一つの物質座標で、広域と地域の天候領域を混ぜる。地域領域は担当面積で
// 解像できるときだけ使い、未解像の細かな天候模様を別の低周波へ折り返さない。
float4 cloudWeatherDataAtMaterialXz(float2 xz,float regionalWeight){
    // 二つの独立した2D回転をまとめ、四つの内積ではなく一つの積和として計算する。
    float4 rotated=
        xz.x*float4(0.8660254,0.5,0.9563048,-0.2923717)
       +xz.y*float4(-0.5,0.8660254,0.2923717,0.9563048);
    float4 weatherUv=
        rotated/float4(65536.0,65536.0,9127.0,9127.0)
       +float4(0.173,0.417,0.619,0.281);
    // 飛行規模の総観領域と回転した地域領域を混ぜ、短い繰り返し周期を隠す。
    float4 a=weatherMap.SampleLevel(
        weatherMap_sampler,weatherUv.xy,0);
    float4 b=weatherMap.SampleLevel(
        weatherMap_sampler,weatherUv.zw,0);
    float4 weather=lerp(a,b,0.45*saturate(regionalWeight));
    weather.b=max(a.b,b.b*0.72*saturate(regionalWeight));
    return weather;
}
// 担当幅で解像できない地域天候だけを総観領域へ戻す。値の面積平均は行わず、
// 非線形なしきい値より前に晴天列と雲列を混ぜない。
float cloudRegionalWeatherWeight(float2 safeFootprint){
    float maximumHorizontalFootprint=max(
        safeFootprint.x,safeFootprint.y);
    const float regionalShortestPeriod=9127.0/29.0;
    return 1.0-smoothstep(
        0.25,0.50,maximumHorizontalFootprint/regionalShortestPeriod);
}
// 生の天候標本へ被覆分布補正と作者指定を一度だけ適用する。
float4 cloudFinalizeWeatherData(float4 weather){
    weather.r=saturate((weather.r-0.045)*1.095);
    // 二領域を混ぜた雲種は主に 0.42～0.66 に収まる。この実分布を全範囲へ広げ、
    // 中央値付近を積雲へ飽和させず、層雲、層積雲、積雲の高さ形状を使い分ける。
    weather.g=smoothstep(0.42,0.66,weather.g);
    // 手続き模様を消さずに目的の天候へ寄せる。被覆の時間変化より先に適用し、
    // 視線密度、自己影、立体物用雲影が同じ雲種と降水成分を共有する。
    weather.g=lerp(weather.g,cloudWeatherControl.x,cloudWeatherControl.y);
    weather.b=lerp(weather.b,cloudWeatherControl.z,cloudWeatherControl.w);
    // 成長後の被覆をこの天候値に収め、占有判定、詳細密度、自己影で共有する。
    weather.r=saturate(
        weather.r+cloudWeatherCoverageEvolution(weather));
    return weather;
}
// 主視線は各Gauss点の完成密度を積分するため、画素・区間の入力値を先に平均しない。
// 担当幅は地域天候の周波数選択だけへ使い、総観領域の一点値を保つ。
float4 cloudWeatherDataBandLimitedPoint(
    float3 p,float2 resolutionFootprint){
    float2 safeFootprint=max(resolutionFootprint,0.0.xx);
    float regionalWeight=cloudRegionalWeatherWeight(safeFootprint);
    float2 xz=p.xz-cloudWindWorld();
    return cloudFinalizeWeatherData(
        cloudWeatherDataAtMaterialXz(xz,regionalWeight));
}
float3 rotateNoise(float3 q){
    return float3(
        dot(q,float3(0.0000000,0.8000000,0.6000000)),
        dot(q,float3(-0.7071068,-0.4242641,0.5656854)),
        dot(q,float3(0.7071068,-0.4242641,0.5656854)));
}
float2 cloudCurlOffset(float3 p,float2 horizontalFootprint){
    float2 xz=p.xz-cloudWindWorld();
    // Pack both curl domains for the same reason as the weather rotations.
    float4 rotated=
        xz.x*float4(0.9063078,0.4226183,0.0,1.0)
       +xz.y*float4(-0.4226183,0.9063078,-1.0,0.0);
    float4 curlUv=
        rotated/float4(1536.0,1536.0,947.0,947.0)
       +float4(0.137,0.619,0.743,0.281);
    // 基準領域を固定し、第 2 領域だけを動かして渦の形を時間変化させる。
    curlUv+=float4(0.0,0.0,cloudEvolution.z,cloudEvolution.w);
    float2 a=curlNoise.SampleLevel(
        curlNoise_sampler,curlUv.xy,0).rg;
    float2 b=curlNoise.SampleLevel(
        curlNoise_sampler,curlUv.zw,0).rg;
    // 地域領域の17周期は最短約56 m。担当幅で解像できない渦は平均0へ戻し、
    // 雲全体を未解像の横ずれでちらつかせない。
    const float shortestCurlPeriod=947.0/17.0;
    float frequencyVisibility=1.0-smoothstep(
        0.25,0.50,max(max(horizontalFootprint.x,0.0),
                      max(horizontalFootprint.y,0.0))/shortestCurlPeriod);
    return (a*0.68+b.yx*0.32)*frequencyVisibility;
}
float3 cloudUVW(
    float3 p,float normalizedLayerHeight,bool upperBand){
    float shapeScale=cloudShapeScale();
    float2 xz=p.xz-cloudWindWorld()
             +cloudHeightShapeShear(normalizedLayerHeight,upperBand);
    // 低周波形状は、天候しきい値や局所雲頂で座標自体を曲げない。
    // 同じ物理点を視線・自己影・環境光が同じ3D物質座標として読むことで、
    // 空間探索の担当幅を解析可能にし、高さごとの縦筋と移流時の位相飛びを防ぐ。
    float canonicalY=saturate(normalizedLayerHeight)
                    *cloudShapeVerticalSpan(upperBand)+0.07;
    float3 canonicalPosition=float3(
        xz.x*shapeScale,
        canonicalY,
        xz.y*shapeScale);
    // 直交回転で物理尺度を保ったまま世界軸との整列を外す。同じXZの上下で
    // tile周期が一致して断面が積み重なる、煙柱状の反復を防ぐ。
    return rotateNoise(canonicalPosition);
}
static const float CLOUD_CONDENSATION_BASE_SUPPORT_SCALE=0.20;
static const float CLOUD_CONDENSATION_MAXIMUM_BASE_SUPPORT=0.42;
static const float CLOUD_CONDENSATION_MAXIMUM_BILLOW_OFFSET=0.13;
static const float CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET=0.12;
static const float CLOUD_CONDENSATION_TRANSITION_WIDTH=0.06;
// 周期Perlinの勾配分布と五次補間を全セル位置で積分した分散を、
// 標準正規分布の4点Gauss-Hermite求積へ写した最粗ポテンシャルと確率。
// exp(-x*x)の節点をそのまま使わずsqrt(2)を掛け、実分散を半減させない。
static const float CLOUD_UNRESOLVED_COARSE_INNER_POTENTIAL=0.141673264;
static const float CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL=0.445741543;
static const float CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT=0.454124145;
static const float CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT=0.0458758548;
// 128角へ焼いた最粗形状を軸方向へ周期移動し、正の自己相関を積分した距離。
// 周波数2のセル幅ではなく、領域ゆがみと半精度保存を含む実データから測定する。
static const float3 CLOUD_COARSE_CORRELATION_DOMAIN_LENGTHS=float3(
    0.251507194,0.188147797,0.197809963);
// 包絡が0なら、形状・雲底補助・詳細変位が全て最大でも凝結しない片側障壁。
// 1では0となるため、十分に湿った内部の3D形状を一様な正値へ押し上げない。
static const float CLOUD_CONDENSATION_ENVELOPE_REJECTION=
    1.0+CLOUD_CONDENSATION_MAXIMUM_BASE_SUPPORT
        *CLOUD_CONDENSATION_BASE_SUPPORT_SCALE
    +CLOUD_CONDENSATION_MAXIMUM_BILLOW_OFFSET
    +CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET;
// 占有判定が後段の最大正方向変位を含めても空セルを捨てない上限。
static const float CLOUD_CONDENSATION_MAXIMUM_POSITIVE_OFFSET=
    CLOUD_CONDENSATION_MAXIMUM_BASE_SUPPORT
        *CLOUD_CONDENSATION_BASE_SUPPORT_SCALE
    +CLOUD_CONDENSATION_MAXIMUM_BILLOW_OFFSET
    +CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET;
// CPU参照と同じ有限判定を使い、NaNと無限大を密度へ通さない。
bool cloudCondensationValueIsFinite(float value){
    return value==value
        &&value>=-3.402823466e+38
        &&value<=3.402823466e+38;
}
float cloudCondensationFiniteSaturate(float value){
    return cloudCondensationValueIsFinite(value)?saturate(value):0.0;
}
// 保存した0～1値を、負=未凝結、正=凝結のポテンシャルへ戻す。
float cloudSignedPotentialFromStored(float storedPotential){
    float signedPotential=-1.0;
    if(cloudCondensationValueIsFinite(storedPotential))
        signedPotential=saturate(storedPotential)*2.0-1.0;
    return signedPotential;
}
// 2D天候と高さ分布は不足分だけを3D凝結場から引く。
// 不足率を二乗して湿潤中心の勾配を0にし、包絡模様を雲芯へ焼き付けない。
float cloudCondensationEnvelopeGate(float envelope){
    float safeEnvelope=cloudCondensationValueIsFinite(envelope)
        ?saturate(envelope):0.0;
    float missingEnvelope=1.0-safeEnvelope;
    return -missingEnvelope*missingEnvelope
        *CLOUD_CONDENSATION_ENVELOPE_REJECTION;
}
// 積雲の凝結高度では同じ天候塊の底面が概ね揃う。3D雑音の高い点だけを先に
// 可視化すると逆円すい状の尾になるため、雲底近傍だけに低い密度下限を置く。
// 高さ0.16より上では完全に0となり、2D天候場を雲頂まで柱状に押し出さない。
float cloudCondensationBaseSupport(
    float height,float weatherMask,float toweringStrength){
    float localBaseEntry=smoothstep(0.0,0.035,saturate(height));
    float baseBand=1.0-smoothstep(0.035,0.16,saturate(height));
    float weatherCore=smoothstep(0.12,0.72,saturate(weatherMask));
    float support=lerp(
        CLOUD_CONDENSATION_MAXIMUM_BASE_SUPPORT,0.36,
        saturate(toweringStrength));
    return localBaseEntry*baseBand*weatherCore*support;
}
// 周波数帯に共通する2D天候、高さ包絡、雲底補助を一度だけ求める。
float cloudCondensationCommonPotential(
    float verticalProfile,float weatherMask,
    float height,float toweringStrength){
    float baseSupport=cloudCondensationBaseSupport(
        height,weatherMask,toweringStrength);
    if(!cloudCondensationValueIsFinite(baseSupport)) baseSupport=0.0;
    return cloudCondensationEnvelopeGate(weatherMask)
        +cloudCondensationEnvelopeGate(verticalProfile)
        +baseSupport*CLOUD_CONDENSATION_BASE_SUPPORT_SCALE;
}
// 3D湿度と共通条件を加算し、最終的な0等値面を表す符号付き場を作る。
float cloudCondensationPotential(
    float shapePotential,float verticalProfile,float weatherMask,
    float height,float toweringStrength){
    float safeShapePotential=cloudCondensationValueIsFinite(shapePotential)
        ?clamp(shapePotential,-1.0,1.0):-1.0;
    return safeShapePotential+cloudCondensationCommonPotential(
        verticalProfile,weatherMask,height,toweringStrength);
}
// 符号付き場の0以下を厳密に空へ保ち、正側だけをC1連続に立ち上げる。
// 平均0へ移る未解像LODが一様な灰色媒質へ変わらず、深い内部は飽和させない。
float cloudCondensationDensity(float potential){
    float densityResult=0.0;
    if(cloudCondensationValueIsFinite(potential)){
        if(potential>=CLOUD_CONDENSATION_TRANSITION_WIDTH){
            densityResult=potential;
        }else if(potential>0.0){
            float transition=potential/CLOUD_CONDENSATION_TRANSITION_WIDTH;
            densityResult=potential*transition*(2.0-transition);
        }
    }
    return densityResult;
}
// 最粗形状が未解像な場合だけ、密度の期待値を求める4点Gauss-Hermite求積重み。
// この順位を光路へ持ち越すと無限相関になるため、実空間サブレイの重みには使わない。
static const float4 CLOUD_UNRESOLVED_QUADRATURE_WEIGHTS=float4(
    CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT,
    CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT,
    CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT,
    CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT);
// 一画素を2×2 Gauss-Legendre求積する4本の実空間サブレイ。
// 各レーンは同じ面積を所有し、視線区間を跨いでも画面内の位置を入れ替えない。
static const float4 CLOUD_SUBRAY_AREA_WEIGHTS=0.25.xxxx;
static const float CLOUD_SUBRAY_GAUSS_OFFSET=0.2886751346;
float cloudDensityLaneAverage(float4 densityLanes){
    float4 safeLanes=float4(
        cloudValueIsFinite(densityLanes.x)&&densityLanes.x>0.0
            ?densityLanes.x:0.0,
        cloudValueIsFinite(densityLanes.y)&&densityLanes.y>0.0
            ?densityLanes.y:0.0,
        cloudValueIsFinite(densityLanes.z)&&densityLanes.z>0.0
            ?densityLanes.z:0.0,
        cloudValueIsFinite(densityLanes.w)&&densityLanes.w>0.0
            ?densityLanes.w:0.0);
    return dot(safeLanes,CLOUD_SUBRAY_AREA_WEIGHTS);
}
float4 cloudScaleDensityLanes(float4 densityLanes,float densityScale){
    float safeScale=cloudValueIsFinite(densityScale)&&densityScale>0.0
        ?densityScale:0.0;
    if(safeScale<=0.0) return 0.0.xxxx;
    return densityLanes*safeScale;
}
// 最粗周期より小さい構造を、低湿度側から高湿度側へ4点求積した密度分布へ移す。
// これは実空間サブレイではなく、後段で透過率期待値を求める同一位置の状態である。
float4 cloudUnresolvedCoarseDensityDistribution(float sharedOffset){
    return float4(
        cloudCondensationDensity(
            sharedOffset-CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL),
        cloudCondensationDensity(
            sharedOffset-CLOUD_UNRESOLVED_COARSE_INNER_POTENTIAL),
        cloudCondensationDensity(
            sharedOffset+CLOUD_UNRESOLVED_COARSE_INNER_POTENTIAL),
        cloudCondensationDensity(
            sharedOffset+CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL));
}
float cloudDensityDistributionMean(float4 requestedDistribution){
    float4 safeDistribution=float4(
        cloudValueIsFinite(requestedDistribution.x)
            &&requestedDistribution.x>0.0?requestedDistribution.x:0.0,
        cloudValueIsFinite(requestedDistribution.y)
            &&requestedDistribution.y>0.0?requestedDistribution.y:0.0,
        cloudValueIsFinite(requestedDistribution.z)
            &&requestedDistribution.z>0.0?requestedDistribution.z:0.0,
        cloudValueIsFinite(requestedDistribution.w)
            &&requestedDistribution.w>0.0?requestedDistribution.w:0.0);
    return dot(
        safeDistribution,CLOUD_UNRESOLVED_QUADRATURE_WEIGHTS);
}
float4 cloudScaleDensityDistribution(
    float4 densityDistribution,float densityScale){
    float4 result=0.0.xxxx;
    float safeScale=cloudValueIsFinite(densityScale)&&densityScale>0.0
        ?densityScale:0.0;
    if(safeScale>0.0)
        result=densityDistribution*safeScale;
    return result;
}
// 互換表示や局所媒質量だけが使う平均密度。Beer-Lambert輸送は分布を直接閉じる。
float cloudUnresolvedCoarseMeanDensity(float sharedOffset){
    return cloudDensityDistributionMean(
        cloudUnresolvedCoarseDensityDistribution(sharedOffset));
}
// xyzの点形状は同じ物質点のLOD表現として密度を補間し、wの未解像終端は
// 4状態の密度分布へ移す。LOD重みを雲の面積割合や実空間レーンとして扱わない。
float4 cloudBandLimitedCondensationDistribution(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float detailPotential){
    float sharedOffset=commonPotential+detailPotential;
    float4 safeWeights=float4(
        cloudValueIsFinite(frequencyWeights.x)&&frequencyWeights.x>0.0
            ?frequencyWeights.x:0.0,
        cloudValueIsFinite(frequencyWeights.y)&&frequencyWeights.y>0.0
            ?frequencyWeights.y:0.0,
        cloudValueIsFinite(frequencyWeights.z)&&frequencyWeights.z>0.0
            ?frequencyWeights.z:0.0,
        cloudValueIsFinite(frequencyWeights.w)&&frequencyWeights.w>0.0
            ?frequencyWeights.w:0.0);
    float pointDensity=
        safeWeights.x*cloudCondensationDensity(
            shapePotentials.x+sharedOffset)
        +safeWeights.y*cloudCondensationDensity(
            shapePotentials.y+sharedOffset)
        +safeWeights.z*cloudCondensationDensity(
            shapePotentials.z+sharedOffset);
    float4 unresolvedDensityDistribution=
        cloudUnresolvedCoarseDensityDistribution(sharedOffset);
    return pointDensity.xxxx
        +safeWeights.w*unresolvedDensityDistribution;
}
float4 cloudBandLimitedCondensationLanes(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float detailPotential){
    return cloudDensityDistributionMean(
        cloudBandLimitedCondensationDistribution(
            shapePotentials,frequencyWeights,
            commonPotential,detailPotential)).xxxx;
}
float cloudBandLimitedCondensationDensity(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float detailPotential){
    return cloudDensityDistributionMean(
        cloudBandLimitedCondensationDistribution(
            shapePotentials,frequencyWeights,
            commonPotential,detailPotential));
}
// 房なし・粗房・解像済み房と、侵食なし・ありを各密度状態で正値化してから混ぜる。
float4 cloudDetailFilteredCondensationDistribution(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float2 billowPotentials,
    float billowVisibility,float middleBillowVisibility,
    float erosionPotential,float erosionVisibility){
    float safeBillowVisibility=
        cloudCondensationFiniteSaturate(billowVisibility);
    float safeMiddleBillowVisibility=
        cloudCondensationFiniteSaturate(middleBillowVisibility);
    float safeErosionVisibility=
        cloudCondensationFiniteSaturate(erosionVisibility);
    float4 baseDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,0.0);
    float4 coarseBillowDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,billowPotentials.x);
    float4 resolvedBillowDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,billowPotentials.y);
    float4 billowDensity=lerp(
        coarseBillowDensity,resolvedBillowDensity,
        safeMiddleBillowVisibility);
    float4 withoutErosionDensity=lerp(
        baseDensity,billowDensity,safeBillowVisibility);
    float4 erosionDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,erosionPotential);
    float4 coarseBillowErosionDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,
        billowPotentials.x+erosionPotential);
    float4 resolvedBillowErosionDensity=cloudBandLimitedCondensationDistribution(
        shapePotentials,frequencyWeights,commonPotential,
        billowPotentials.y+erosionPotential);
    float4 billowErosionDensity=lerp(
        coarseBillowErosionDensity,resolvedBillowErosionDensity,
        safeMiddleBillowVisibility);
    float4 withErosionDensity=lerp(
        erosionDensity,billowErosionDensity,safeBillowVisibility);
    return lerp(
        withoutErosionDensity,withErosionDensity,
        safeErosionVisibility);
}
float4 cloudDetailFilteredCondensationLanes(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float2 billowPotentials,
    float billowVisibility,float middleBillowVisibility,
    float erosionPotential,float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDetailFilteredCondensationDistribution(
            shapePotentials,frequencyWeights,commonPotential,billowPotentials,
            billowVisibility,middleBillowVisibility,
            erosionPotential,erosionVisibility)).xxxx;
}
float cloudDetailFilteredCondensationDensity(
    float4 shapePotentials,float4 frequencyWeights,
    float commonPotential,float2 billowPotentials,
    float billowVisibility,float middleBillowVisibility,
    float erosionPotential,float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDetailFilteredCondensationDistribution(
            shapePotentials,frequencyWeights,commonPotential,billowPotentials,
            billowVisibility,middleBillowVisibility,
            erosionPotential,erosionVisibility));
}
// 形状座標の回転、球殻高度、高度せん断と、焼き込み形状の三軸自己相関から
// この光路方向に沿った物理相関距離を求める。
float cloudUnresolvedDensityCorrelationLengthAtDirection(
    float3 p,float3 rayDirection,bool upperBand){
    float3 local=p-worldOrigin.xyz;
    float3 radialOffset=float3(
        local.x,CLOUD_PLANET_RADIUS+local.y,local.z);
    float3 radialUp=radialOffset/max(length(radialOffset),1.0);
    float altitudeRate=dot(radialUp,rayDirection);
    float inverseThickness=upperBand
        ?cloudUpperLayer.z:cloudFrameTerms.w;
    float bandScale=upperBand?0.25:1.0;
    float2 shearRate=float2(0.9284767,0.3713907)
        *(850.0*bandScale*max(inverseThickness,0.0)*altitudeRate);
    float3 materialDirection=float3(
        rayDirection.x+shearRate.x,
        altitudeRate,
        rayDirection.z+shearRate.y);
    float3 domainDirection=rotateNoise(materialDirection);
    float inverseDomainDistance=length(
        domainDirection/CLOUD_COARSE_CORRELATION_DOMAIN_LENGTHS);
    float inversePhysicalDistance=
        max(cloudShapeScale(),0.0)*inverseDomainDistance;
    return inversePhysicalDistance>1e-8
        ?1.0/inversePhysicalDistance:0.0;
}

// 位置と方向が有限値かを確認し、異常な定数バッファを光路計算へ通さない。
bool cloudFiniteFloat3(float3 value){
    return cloudValueIsFinite(value.x)&&cloudValueIsFinite(value.y)
        &&cloudValueIsFinite(value.z);
}

// 有効区間だけを確認し、未使用の区間スロットにある値は読まない。
bool cloudPackedBandIntervalsAreFinite(CloudPackedBandIntervals intervals){
    int intervalCount=intervals.count;
    bool valid=intervalCount>=0&&intervalCount<=4;
    if(intervalCount>0)
        valid=valid&&cloudFiniteIntervalLength(
            intervals.starts.x,intervals.ends.x)>0.0;
    if(intervalCount>1)
        valid=valid&&cloudFiniteIntervalLength(
            intervals.starts.y,intervals.ends.y)>0.0;
    if(intervalCount>2)
        valid=valid&&cloudFiniteIntervalLength(
            intervals.starts.z,intervals.ends.z)>0.0;
    if(intervalCount>3)
        valid=valid&&cloudFiniteIntervalLength(
            intervals.starts.w,intervals.ends.w)>0.0;
    return valid;
}

// 光路区間の両端と中央で最短相関長を求め、曲率や高させん断で区間端だけが
// 細かくなる場合も見落とさない。相関長の半分以下へ標本間隔を制限するため、
// 長さだけで予算を配るより高周波の自己影を安定して積分できる。
float cloudLightIntervalSampleDemand(
    CloudPackedBandIntervals intervals,float3 rayOrigin,
    float3 rayDirection,int intervalIndex){
    float intervalStart=intervalIndex==0?intervals.starts.x:
        (intervalIndex==1?intervals.starts.y:
        (intervalIndex==2?intervals.starts.z:intervals.starts.w));
    float intervalEnd=intervalIndex==0?intervals.ends.x:
        (intervalIndex==1?intervals.ends.y:
        (intervalIndex==2?intervals.ends.z:intervals.ends.w));
    bool validInput=cloudFiniteFloat3(rayOrigin)
        &&cloudFiniteFloat3(rayDirection)
        &&cloudValueIsFinite(intervalStart)
        &&cloudValueIsFinite(intervalEnd)
        &&intervalEnd>intervalStart;
    float intervalLength=cloudFiniteIntervalLength(
        intervalStart,intervalEnd);
    float evaluationEnd=intervalStart+intervalLength;
    validInput=validInput&&intervalLength>0.0;
    validInput=validInput&&cloudValueIsFinite(evaluationEnd);
    float result=0.0;
    if(intervalLength>0.0){
        float midpoint=intervalStart+0.5*intervalLength;
        float3 startPosition=rayOrigin+rayDirection*intervalStart;
        float3 midpointPosition=rayOrigin+rayDirection*midpoint;
        float3 endPosition=rayOrigin+rayDirection*evaluationEnd;
        bool finitePositions=cloudFiniteFloat3(startPosition)
            &&cloudFiniteFloat3(midpointPosition)
            &&cloudFiniteFloat3(endPosition);
        int bandId=intervalIndex==0?intervals.bandIds.x:
            (intervalIndex==1?intervals.bandIds.y:
            (intervalIndex==2?intervals.bandIds.z:intervals.bandIds.w));
        float startCorrelationLength=0.0;
        float midpointCorrelationLength=0.0;
        float endCorrelationLength=0.0;
        if(finitePositions){
            startCorrelationLength=
                cloudUnresolvedDensityCorrelationLengthAtDirection(
                    startPosition,rayDirection,bandId>0);
            midpointCorrelationLength=
                cloudUnresolvedDensityCorrelationLengthAtDirection(
                    midpointPosition,rayDirection,bandId>0);
            endCorrelationLength=
                cloudUnresolvedDensityCorrelationLengthAtDirection(
                    endPosition,rayDirection,bandId>0);
        }
        bool finiteCorrelation=finitePositions
            &&cloudValueIsFinite(startCorrelationLength)
            &&cloudValueIsFinite(midpointCorrelationLength)
            &&cloudValueIsFinite(endCorrelationLength);
        float shortestCorrelationLength=3.402823466e+38;
        bool hasPositiveCorrelation=false;
        if(startCorrelationLength>0.0){
            shortestCorrelationLength=min(
                shortestCorrelationLength,startCorrelationLength);
            hasPositiveCorrelation=true;
        }
        if(midpointCorrelationLength>0.0){
            shortestCorrelationLength=min(
                shortestCorrelationLength,midpointCorrelationLength);
            hasPositiveCorrelation=true;
        }
        if(endCorrelationLength>0.0){
            shortestCorrelationLength=min(
                shortestCorrelationLength,endCorrelationLength);
            hasPositiveCorrelation=true;
        }
        if(finiteCorrelation){
            result=float(CLOUD_LIGHT_MARCH_SAMPLE_COUNT);
            if(hasPositiveCorrelation){
                float requestedDemand=
                    2.0*intervalLength/shortestCorrelationLength;
                if(cloudValueIsFinite(requestedDemand))
                    result=min(
                        float(CLOUD_LIGHT_MARCH_SAMPLE_COUNT),
                        max(1.0,requestedDemand));
            }else{
                result=1.0;
            }
        }
    }
    return result;
}

// 相関長に基づく要求量を、最大標本数を越えない整数予算へ変換する。
// 有効区間ごとに一標本を先に予約し、残りは要求量の不足が大きい区間へ優先する。
int4 cloudAdaptiveLightSampleCounts(
    CloudPackedBandIntervals intervals,int requestedSampleCount,
    float3 rayOrigin,float3 rayDirection){
    int4 sampleCounts=int4(0,0,0,0);
    int intervalCount=clamp(intervals.count,0,4);
    int safeSampleCount=clamp(
        requestedSampleCount,0,CLOUD_LIGHT_MARCH_SAMPLE_COUNT);
    bool validInput=cloudPackedBandIntervalsAreFinite(intervals)
        &&cloudFiniteFloat3(rayOrigin)&&cloudFiniteFloat3(rayDirection);
    if(intervalCount>0&&safeSampleCount>0&&validInput){
        float4 demands=float4(
            cloudLightIntervalSampleDemand(
                intervals,rayOrigin,rayDirection,0),
            cloudLightIntervalSampleDemand(
                intervals,rayOrigin,rayDirection,1),
            cloudLightIntervalSampleDemand(
                intervals,rayOrigin,rayDirection,2),
            cloudLightIntervalSampleDemand(
                intervals,rayOrigin,rayDirection,3));
        if(intervalCount<4) demands.w=0.0;
        if(intervalCount<3) demands.z=0.0;
        if(intervalCount<2) demands.y=0.0;
        int reservedPerInterval=safeSampleCount>=intervalCount?1:0;
        if(reservedPerInterval>0){
            sampleCounts.x=intervalCount>0?1:0;
            sampleCounts.y=intervalCount>1?1:0;
            sampleCounts.z=intervalCount>2?1:0;
            sampleCounts.w=intervalCount>3?1:0;
        }
        int remainingSampleCount=
            safeSampleCount-reservedPerInterval*intervalCount;
        // 予算配分の反復を展開すると、区間ごとの相関評価と同時に
        // 一時値が生存して本番CSのレジスタを圧迫する。反復上限は固定でも、
        // 実際の残数は入力で変わるため、動的ループとして実行する。
        [loop] for(int allocation=0;
                     allocation<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;
                     ++allocation){
            if(allocation<remainingSampleCount){
                float4 residual=max(
                    demands-float4(sampleCounts),0.0.xxxx);
                float4 intervalLengths=float4(
                    cloudFiniteIntervalLength(
                        intervals.starts.x,intervals.ends.x),
                    cloudFiniteIntervalLength(
                        intervals.starts.y,intervals.ends.y),
                    cloudFiniteIntervalLength(
                        intervals.starts.z,intervals.ends.z),
                    cloudFiniteIntervalLength(
                        intervals.starts.w,intervals.ends.w));
                if(intervalCount<4) intervalLengths.w=0.0;
                if(intervalCount<3) intervalLengths.z=0.0;
                if(intervalCount<2) intervalLengths.y=0.0;
                bool hasResidual=dot(residual,1.0.xxxx)>0.0;
                float4 priority=hasResidual
                    ?residual
                    :intervalLengths/float4(sampleCounts+1);
                int selectedIndex=0;
                if(priority.y>priority.x) selectedIndex=1;
                if(priority.z>priority[selectedIndex]) selectedIndex=2;
                if(priority.w>priority[selectedIndex]) selectedIndex=3;
                if(selectedIndex==0) sampleCounts.x++;
                if(selectedIndex==1) sampleCounts.y++;
                if(selectedIndex==2) sampleCounts.z++;
                if(selectedIndex==3) sampleCounts.w++;
            }
        }
    }
    return sampleCounts;
}

// 相関長から決めた区間予算を、区間中央の標本位置と物理帯IDへ写す。
bool cloudAdaptiveLightSampleTerms(
    CloudPackedBandIntervals intervals,int4 sampleCounts,int sampleIndex,
    out float rayDistance,out float sampleSpacing,out int sampleBandId){
    rayDistance=0.0;
    sampleSpacing=0.0;
    sampleBandId=-1;
    bool validSample=false;
    int sampleStart=0;
    [unroll] for(int intervalIndex=0;intervalIndex<4;++intervalIndex){
        int intervalSampleCount=sampleCounts[intervalIndex];
        int sampleEnd=sampleStart+intervalSampleCount;
        if(!validSample&&sampleIndex>=sampleStart&&sampleIndex<sampleEnd){
            int intervalSampleIndex=sampleIndex-sampleStart;
            float intervalStart=intervalIndex==0?intervals.starts.x:
                (intervalIndex==1?intervals.starts.y:
                (intervalIndex==2?intervals.starts.z:intervals.starts.w));
            float intervalEnd=intervalIndex==0?intervals.ends.x:
                (intervalIndex==1?intervals.ends.y:
                (intervalIndex==2?intervals.ends.z:intervals.ends.w));
            float intervalLength=cloudFiniteIntervalLength(
                intervalStart,intervalEnd);
            sampleSpacing=intervalLength
                /float(max(intervalSampleCount,1));
            rayDistance=intervalStart
                +(float(intervalSampleIndex)+0.5)*sampleSpacing;
            sampleBandId=intervals.bandIds[intervalIndex];
            validSample=cloudValueIsFinite(intervalStart)
                &&cloudValueIsFinite(intervalLength)
                &&cloudValueIsFinite(sampleSpacing)
                &&cloudValueIsFinite(rayDistance)
                &&sampleSpacing>1e-4;
        }
        sampleStart=sampleEnd;
    }
    return validSample;
}
float3 cloudBeerAbsorptionFraction3(float3 opticalDepth){
    return float3(
        cloudBeerAbsorptionFraction(opticalDepth.x),
        cloudBeerAbsorptionFraction(opticalDepth.y),
        cloudBeerAbsorptionFraction(opticalDepth.z));
}
// 1からの減少量を、薄い媒質でも桁落ちさせず光学的深さへ戻す。
float cloudOpticalDepthFromAbsorption(float requestedAbsorption){
    float absorption=cloudValueIsFinite(requestedAbsorption)
        ?saturate(requestedAbsorption):0.0;
    float result=0.0;
    if(absorption>=1.0){
        result=80.0;
    }else if(absorption>0.0){
        if(absorption<=0.125){
            float squared=absorption*absorption;
            float cubed=squared*absorption;
            float fourth=squared*squared;
            float fifth=fourth*absorption;
            float sixth=cubed*cubed;
            result=absorption+squared*0.5+cubed/3.0
                +fourth*0.25+fifth*0.2+sixth/6.0;
        }else{
            result=min(-log(max(1.0-absorption,1e-35)),80.0);
        }
    }
    return result;
}
float3 cloudOpticalDepthFromAbsorption3(float3 absorption){
    return float3(
        cloudOpticalDepthFromAbsorption(absorption.x),
        cloudOpticalDepthFromAbsorption(absorption.y),
        cloudOpticalDepthFromAbsorption(absorption.z));
}
float4 cloudOpticalDepthFromAbsorption4(float4 absorption){
    return float4(
        cloudOpticalDepthFromAbsorption(absorption.x),
        cloudOpticalDepthFromAbsorption(absorption.y),
        cloudOpticalDepthFromAbsorption(absorption.z),
        cloudOpticalDepthFromAbsorption(absorption.w));
}
float4 cloudFiniteNonnegative4(float4 requestedValues){
    return float4(
        cloudValueIsFinite(requestedValues.x)
            ?max(requestedValues.x,0.0):0.0,
        cloudValueIsFinite(requestedValues.y)
            ?max(requestedValues.y,0.0):0.0,
        cloudValueIsFinite(requestedValues.z)
            ?max(requestedValues.z,0.0):0.0,
        cloudValueIsFinite(requestedValues.w)
            ?max(requestedValues.w,0.0):0.0);
}
// 有限な正値だけを有効にする。物理量へ恣意的なしきい値を設けず、
// 正の極薄区間と微小消散もCPU側と同じBeer-Lambert積分へ渡す。
float4 cloudPositiveMask4(float4 requestedValues){
    return float4(
        cloudValueIsFinite(requestedValues.x)&&requestedValues.x>0.0?1.0:0.0,
        cloudValueIsFinite(requestedValues.y)&&requestedValues.y>0.0?1.0:0.0,
        cloudValueIsFinite(requestedValues.z)&&requestedValues.z>0.0?1.0:0.0,
        cloudValueIsFinite(requestedValues.w)&&requestedValues.w>0.0?1.0:0.0);
}
// CPU参照式と同じく、しきい値と等しい値は級数側へ残す。
// stepは等値を大きい側へ含めるため、厳密な大小比較を成分ごとに明示する。
float4 cloudStrictGreaterMask4(float4 requestedValues,float threshold){
    return float4(
        requestedValues.x>threshold?1.0:0.0,
        requestedValues.y>threshold?1.0:0.0,
        requestedValues.z>threshold?1.0:0.0,
        requestedValues.w>threshold?1.0:0.0);
}
float4 cloudFiniteOpticalDepth4(float4 requestedDepths){
    return float4(
        cloudValueIsFinite(requestedDepths.x)
            ?max(requestedDepths.x,0.0):3.402823466e+38,
        cloudValueIsFinite(requestedDepths.y)
            ?max(requestedDepths.y,0.0):3.402823466e+38,
        cloudValueIsFinite(requestedDepths.z)
            ?max(requestedDepths.z,0.0):3.402823466e+38,
        cloudValueIsFinite(requestedDepths.w)
            ?max(requestedDepths.w,0.0):3.402823466e+38);
}
// 各float4成分は画素内の実サブレイ、state0～3は同一点の未解像密度状態を表す。
// 条件付き生存確率を標本境界で捨てず、物理的な相関セル境界だけで定常確率へ戻す。
struct CloudFourStateTransportLanes {
    float4 state0;
    float4 state1;
    float4 state2;
    float4 state3;
    float4 boundaryDistances;
    float4 cellLengths;
    float4 boundaryPending;
    float4 active;
};
CloudFourStateTransportLanes cloudInitialFourStateTransportLanes(){
    CloudFourStateTransportLanes state;
    state.state0=CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx;
    state.state1=CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx;
    state.state2=CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx;
    state.state3=CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx;
    state.boundaryDistances=0.0.xxxx;
    state.cellLengths=0.0.xxxx;
    state.boundaryPending=0.0.xxxx;
    state.active=0.0.xxxx;
    return state;
}
void cloudResetFourStateTransportLanes(
    inout CloudFourStateTransportLanes state,float4 requestedMask){
    float4 mask=saturate(requestedMask);
    state.state0=lerp(
        state.state0,CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,mask);
    state.state1=lerp(
        state.state1,CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,mask);
    state.state2=lerp(
        state.state2,CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,mask);
    state.state3=lerp(
        state.state3,CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,mask);
    state.boundaryDistances=lerp(
        state.boundaryDistances,0.0.xxxx,mask);
    state.cellLengths=lerp(state.cellLengths,0.0.xxxx,mask);
    state.boundaryPending=lerp(
        state.boundaryPending,0.0.xxxx,mask);
}
struct CloudFourStateChunkLanes {
    float4 transmittances;
    float4 absorptions;
    float4 absorptionMoments;
};
// 一相関セルを跨がない区間では、四状態それぞれをBeer-Lambert積分する。
// 平均・分散への縮約を行わないため、濃い光路でも指数平均を失わない。
CloudFourStateChunkLanes cloudFourStateChunkLanes(
    float4 densityState0Lanes,float4 densityState1Lanes,
    float4 densityState2Lanes,float4 densityState3Lanes,
    float extinction,float4 segmentLengths,
    inout CloudFourStateTransportLanes state){
    CloudFourStateChunkLanes result;
    // 呼び出し元の交差区間、設定値、密度生成は有限値を保証する。ここでは
    // 物理下限だけを適用し、同じ有限判定を状態数だけ繰り返さない。
    float4 safeLengths=cloudFiniteNonnegative4(segmentLengths);
    float activeExtinction=
        cloudValueIsFinite(extinction)&&extinction>0.0?1.0:0.0;
    float4 activeMask=cloudPositiveMask4(safeLengths)
        *activeExtinction.xxxx;
    float4 originalState0=state.state0;
    float4 originalState1=state.state1;
    float4 originalState2=state.state2;
    float4 originalState3=state.state3;
    float4 weightSum=max(
        state.state0+state.state1+state.state2+state.state3,0.0.xxxx);
    float4 invalidWeightMask=1.0.xxxx-step(1e-30.xxxx,weightSum);
    float4 inverseWeightSum=1.0.xxxx/max(weightSum,1e-30.xxxx);
    float4 conditional0=lerp(
        max(state.state0,0.0.xxxx)*inverseWeightSum,
        CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,invalidWeightMask);
    float4 conditional1=lerp(
        max(state.state1,0.0.xxxx)*inverseWeightSum,
        CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,invalidWeightMask);
    float4 conditional2=lerp(
        max(state.state2,0.0.xxxx)*inverseWeightSum,
        CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,invalidWeightMask);
    float4 conditional3=lerp(
        max(state.state3,0.0.xxxx)*inverseWeightSum,
        CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,invalidWeightMask);
    float safeExtinction=max(extinction,0.0);
    float4 opticalDepth0=max(densityState0Lanes,0.0.xxxx)
        *safeExtinction*safeLengths;
    float4 opticalDepth1=max(densityState1Lanes,0.0.xxxx)
        *safeExtinction*safeLengths;
    float4 opticalDepth2=max(densityState2Lanes,0.0.xxxx)
        *safeExtinction*safeLengths;
    float4 opticalDepth3=max(densityState3Lanes,0.0.xxxx)
        *safeExtinction*safeLengths;
    float4 absorption0=cloudFiniteBeerAbsorptionFraction4(opticalDepth0);
    float4 absorption1=cloudFiniteBeerAbsorptionFraction4(opticalDepth1);
    float4 absorption2=cloudFiniteBeerAbsorptionFraction4(opticalDepth2);
    float4 absorption3=cloudFiniteBeerAbsorptionFraction4(opticalDepth3);
    float4 transmittance0=lerp(
        1.0.xxxx-absorption0,exp(-opticalDepth0),
        cloudStrictGreaterMask4(opticalDepth0,0.125));
    float4 transmittance1=lerp(
        1.0.xxxx-absorption1,exp(-opticalDepth1),
        cloudStrictGreaterMask4(opticalDepth1,0.125));
    float4 transmittance2=lerp(
        1.0.xxxx-absorption2,exp(-opticalDepth2),
        cloudStrictGreaterMask4(opticalDepth2,0.125));
    float4 transmittance3=lerp(
        1.0.xxxx-absorption3,exp(-opticalDepth3),
        cloudStrictGreaterMask4(opticalDepth3,0.125));
    float4 unnormalized0=conditional0*transmittance0;
    float4 unnormalized1=conditional1*transmittance1;
    float4 unnormalized2=conditional2*transmittance2;
    float4 unnormalized3=conditional3*transmittance3;
    float4 chunkTransmittance=saturate(
        unnormalized0+unnormalized1+unnormalized2+unnormalized3);
    float4 chunkAbsorption=saturate(
        conditional0*absorption0+conditional1*absorption1
        +conditional2*absorption2+conditional3*absorption3);
    float4 inverseChunkTransmittance=
        1.0.xxxx/max(chunkTransmittance,1e-30.xxxx);
    float4 extinguishedMask=
        1.0.xxxx-step(1e-30.xxxx,chunkTransmittance);
    float4 ending0=lerp(
        unnormalized0*inverseChunkTransmittance,
        CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,extinguishedMask);
    float4 ending1=lerp(
        unnormalized1*inverseChunkTransmittance,
        CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,extinguishedMask);
    float4 ending2=lerp(
        unnormalized2*inverseChunkTransmittance,
        CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT.xxxx,extinguishedMask);
    float4 ending3=lerp(
        unnormalized3*inverseChunkTransmittance,
        CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT.xxxx,extinguishedMask);
    state.state0=lerp(originalState0,ending0,activeMask);
    state.state1=lerp(originalState1,ending1,activeMask);
    state.state2=lerp(originalState2,ending2,activeMask);
    state.state3=lerp(originalState3,ending3,activeMask);
    result.transmittances=lerp(
        1.0.xxxx,chunkTransmittance,activeMask);
    result.absorptions=chunkAbsorption*activeMask;
    result.absorptionMoments=activeMask*safeLengths*(
        conditional0*absorption0*
            cloudFiniteBeerAbsorptionCentroidFraction4(
                opticalDepth0,transmittance0)
        +conditional1*absorption1*
            cloudFiniteBeerAbsorptionCentroidFraction4(
                opticalDepth1,transmittance1)
        +conditional2*absorption2*
            cloudFiniteBeerAbsorptionCentroidFraction4(
                opticalDepth2,transmittance2)
        +conditional3*absorption3*
            cloudFiniteBeerAbsorptionCentroidFraction4(
                opticalDepth3,transmittance3));
    return result;
}
struct CloudFourStateTransportResultLanes {
    float4 transmittances;
    float4 absorptions;
    float4 centroidDistances;
};
// 一セル幅を実測相関積分距離の二倍にすると、無作為位相の区分一定場が持つ
// 三角自己相関の積分値に一致する。完全セルは指数平均の等比列でまとめる。
CloudFourStateTransportResultLanes cloudFourStateTransportLanes(
    float4 densityState0Lanes,float4 densityState1Lanes,
    float4 densityState2Lanes,float4 densityState3Lanes,
    float extinction,float4 correlationLengths,float4 segmentLengths,
    inout CloudFourStateTransportLanes state){
    CloudFourStateTransportResultLanes result;
    float4 safeLengths=cloudFiniteNonnegative4(segmentLengths);
    float activeExtinction=
        cloudValueIsFinite(extinction)&&extinction>0.0?1.0:0.0;
    float4 activeMask=cloudPositiveMask4(safeLengths)
        *activeExtinction.xxxx;
    float4 entryMask=activeMask*(1.0.xxxx-saturate(state.active));
    float4 safeCorrelations=cloudFiniteNonnegative4(correlationLengths);
    float4 heterogeneousMask=
        activeMask*cloudPositiveMask4(safeCorrelations);
    float4 meanDensities=
        max(densityState0Lanes,0.0.xxxx)
            *CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT
        +max(densityState1Lanes,0.0.xxxx)
            *CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT
        +max(densityState2Lanes,0.0.xxxx)
            *CLOUD_UNRESOLVED_COARSE_INNER_WEIGHT
        +max(densityState3Lanes,0.0.xxxx)
            *CLOUD_UNRESOLVED_COARSE_OUTER_WEIGHT;
    float4 homogeneousDepths=
        meanDensities*max(extinction,0.0)*safeLengths;
    float4 homogeneousAbsorptions=
        cloudFiniteBeerAbsorptionFraction4(homogeneousDepths);
    float4 homogeneousTransmittances=lerp(
        1.0.xxxx-homogeneousAbsorptions,exp(-homogeneousDepths),
        cloudStrictGreaterMask4(homogeneousDepths,0.125));
    float4 homogeneousCentroids=safeLengths*
        cloudFiniteBeerAbsorptionCentroidFraction4(
            homogeneousDepths,homogeneousTransmittances);

    // 非相関レーンだけ除算用の1 mを置き、正の相関長は微小でも変更しない。
    // 二倍で単精度上限を越える場合はCPU側と同じ最大有限値へ飽和する。
    float4 finiteCellLengths=
        2.0*min(safeCorrelations,1.701411733e38.xxxx);
    float4 cellLengths=lerp(
        1.0.xxxx,finiteCellLengths,heterogeneousMask);
    float4 storedBoundaryDistances=
        cloudFiniteNonnegative4(state.boundaryDistances);
    float4 storedCellLengths=cloudFiniteNonnegative4(state.cellLengths);
    float4 validBoundaryStateMask=
        cloudPositiveMask4(storedBoundaryDistances)
        *cloudPositiveMask4(storedCellLengths)
        *step(storedBoundaryDistances,storedCellLengths);
    float4 pendingBoundaryMask=heterogeneousMask
        *saturate(cloudFiniteNonnegative4(state.boundaryPending))
        *saturate(state.active);
    float4 entryBoundaryMask=entryMask*heterogeneousMask;
    float4 invalidBoundaryStateMask=heterogeneousMask
        *(1.0.xxxx-entryBoundaryMask)
        *(1.0.xxxx-pendingBoundaryMask)
        *(1.0.xxxx-validBoundaryStateMask);
    cloudResetFourStateTransportLanes(state,entryBoundaryMask);
    float4 initializeHalfCellMask=saturate(
        entryBoundaryMask+invalidBoundaryStateMask);
    state.cellLengths=lerp(
        state.cellLengths,cellLengths,initializeHalfCellMask);
    state.boundaryDistances=lerp(
        state.boundaryDistances,0.5*cellLengths,initializeHalfCellMask);
    state.boundaryPending=lerp(
        state.boundaryPending,0.0.xxxx,initializeHalfCellMask);
    state.cellLengths=lerp(
        state.cellLengths,cellLengths,pendingBoundaryMask);
    state.boundaryDistances=lerp(
        state.boundaryDistances,cellLengths,pendingBoundaryMask);
    state.boundaryPending=lerp(
        state.boundaryPending,0.0.xxxx,pendingBoundaryMask);
    float4 distanceToBoundary=
        cloudFiniteNonnegative4(state.boundaryDistances);
    float4 firstLengths=min(safeLengths,distanceToBoundary)
        *heterogeneousMask;
    CloudFourStateChunkLanes firstChunk=cloudFourStateChunkLanes(
        densityState0Lanes,densityState1Lanes,
        densityState2Lanes,densityState3Lanes,
        extinction,firstLengths,state);
    float4 pathTransmittances=firstChunk.transmittances;
    float4 pathAbsorptions=firstChunk.absorptions;
    float4 pathAbsorptionMoments=firstChunk.absorptionMoments;
    float4 traversedLengths=firstLengths;
    float4 remainingLengths=max(safeLengths-firstLengths,0.0.xxxx)
        *heterogeneousMask;
    float4 reachedBoundaryMask=heterogeneousMask*step(
        distanceToBoundary,safeLengths);
    float4 advancedBoundaryDistances=max(
        distanceToBoundary-firstLengths,0.0.xxxx);
    state.boundaryDistances=lerp(
        state.boundaryDistances,advancedBoundaryDistances,
        heterogeneousMask*(1.0.xxxx-reachedBoundaryMask));
    float4 remainingAfterFirstMask=
        cloudPositiveMask4(remainingLengths)*reachedBoundaryMask;
    float4 exactFirstBoundaryMask=
        reachedBoundaryMask*(1.0.xxxx-remainingAfterFirstMask);
    cloudResetFourStateTransportLanes(state,reachedBoundaryMask);
    state.cellLengths=lerp(
        state.cellLengths,cellLengths,remainingAfterFirstMask);
    state.boundaryDistances=lerp(
        state.boundaryDistances,cellLengths,remainingAfterFirstMask);
    state.boundaryPending=lerp(
        state.boundaryPending,1.0.xxxx,exactFirstBoundaryMask);

    float4 fullCellCounts=floor(remainingLengths/cellLengths)
        *reachedBoundaryMask;
    float4 fullCellMask=step(1.0.xxxx,fullCellCounts);
    float4 fullCellsLengths=fullCellCounts*cellLengths;
    CloudFourStateTransportLanes cellState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateChunkLanes cellChunk=cloudFourStateChunkLanes(
        densityState0Lanes,densityState1Lanes,
        densityState2Lanes,densityState3Lanes,
        extinction,cellLengths*fullCellMask,cellState);
    float4 cellAbsorptions=cellChunk.absorptions;
    float4 cellDepths=cloudFiniteOpticalDepthFromAbsorption4(cellAbsorptions);
    float4 fullCellsDepths=cellDepths*fullCellCounts;
    float4 fullCellsAbsorptions=
        cloudFiniteBeerAbsorptionFraction4(fullCellsDepths);
    float4 fullCellsTransmittances=lerp(
        1.0.xxxx-fullCellsAbsorptions,exp(-fullCellsDepths),
        cloudStrictGreaterMask4(fullCellsDepths,0.125));
    fullCellsTransmittances=lerp(
        1.0.xxxx,fullCellsTransmittances,fullCellMask);
    fullCellsAbsorptions*=fullCellMask;
    float4 inverseCounts=1.0.xxxx/max(fullCellCounts,1.0.xxxx);
    float4 inverseCountsSquared=inverseCounts*inverseCounts;
    float4 inverseCountsFourth=
        inverseCountsSquared*inverseCountsSquared;
    float4 inverseCountsSixth=
        inverseCountsFourth*inverseCountsSquared;
    float4 inverseCountsEighth=
        inverseCountsFourth*inverseCountsFourth;
    float4 depthSquared=fullCellsDepths*fullCellsDepths;
    float4 depthCubed=depthSquared*fullCellsDepths;
    float4 depthFifth=depthCubed*depthSquared;
    float4 depthSeventh=depthFifth*depthSquared;
    float4 seriesMeanCellIndices=fullCellCounts*(
        0.5*(1.0.xxxx-inverseCounts)
        -fullCellsDepths*(1.0.xxxx-inverseCountsSquared)/12.0
        +depthCubed*(1.0.xxxx-inverseCountsFourth)/720.0
        -depthFifth*(1.0.xxxx-inverseCountsSixth)/30240.0
        +depthSeventh*(1.0.xxxx-inverseCountsEighth)/1209600.0);
    float4 regularMeanCellIndices=
        cellChunk.transmittances/max(cellAbsorptions,1e-30.xxxx)
        -fullCellCounts*fullCellsTransmittances
            /max(fullCellsAbsorptions,1e-30.xxxx);
    float4 meanCellIndices=lerp(
        seriesMeanCellIndices,regularMeanCellIndices,
        cloudStrictGreaterMask4(fullCellsDepths,1.0));
    meanCellIndices=clamp(
        meanCellIndices,0.0.xxxx,max(fullCellCounts-1.0.xxxx,0.0.xxxx));
    float4 withinCellCentroids=clamp(
        cellChunk.absorptionMoments/max(cellAbsorptions,1e-30.xxxx),
        0.0.xxxx,cellLengths);
    withinCellCentroids=lerp(
        0.5*cellLengths,withinCellCentroids,
        step(1e-20.xxxx,cellAbsorptions));
    float4 fullCellsMoments=fullCellsAbsorptions*(
        cellLengths*meanCellIndices+withinCellCentroids);
    pathAbsorptionMoments+=pathTransmittances*(
        traversedLengths*fullCellsAbsorptions+fullCellsMoments);
    pathAbsorptions+=pathTransmittances*fullCellsAbsorptions;
    pathTransmittances*=fullCellsTransmittances;
    traversedLengths+=fullCellsLengths;
    remainingLengths=max(
        remainingLengths-fullCellsLengths,0.0.xxxx);
    float4 tailLengths=remainingLengths*reachedBoundaryMask;
    float4 tailMask=cloudPositiveMask4(tailLengths);
    float4 fullCellContinuesMask=fullCellMask*tailMask;
    float4 exactFullCellBoundaryMask=
        fullCellMask*(1.0.xxxx-tailMask);
    cloudResetFourStateTransportLanes(state,fullCellMask);
    state.cellLengths=lerp(
        state.cellLengths,cellLengths,fullCellContinuesMask);
    state.boundaryDistances=lerp(
        state.boundaryDistances,cellLengths,fullCellContinuesMask);
    state.boundaryPending=lerp(
        state.boundaryPending,1.0.xxxx,exactFullCellBoundaryMask);

    CloudFourStateChunkLanes tailChunk=cloudFourStateChunkLanes(
        densityState0Lanes,densityState1Lanes,
        densityState2Lanes,densityState3Lanes,
        extinction,tailLengths,state);
    float4 tailAbsorptions=tailChunk.absorptions;
    pathAbsorptionMoments+=pathTransmittances*(
        traversedLengths*tailAbsorptions+tailChunk.absorptionMoments);
    pathAbsorptions+=pathTransmittances*tailAbsorptions;
    pathTransmittances*=tailChunk.transmittances;
    state.cellLengths=lerp(state.cellLengths,cellLengths,tailMask);
    float4 advancedTailBoundaryDistances=max(
        cellLengths-tailLengths,0.0.xxxx);
    state.boundaryDistances=lerp(
        state.boundaryDistances,advancedTailBoundaryDistances,tailMask);
    state.boundaryPending=lerp(
        state.boundaryPending,0.0.xxxx,tailMask);

    pathAbsorptions=saturate(pathAbsorptions);
    float4 pathCentroids=lerp(
        0.5*safeLengths,
        pathAbsorptionMoments/max(pathAbsorptions,1e-30.xxxx),
        step(1e-30.xxxx,pathAbsorptions));
    pathCentroids=clamp(pathCentroids,0.0.xxxx,safeLengths);
    float4 homogeneousMask=activeMask-heterogeneousMask;
    cloudResetFourStateTransportLanes(state,homogeneousMask);
    result.transmittances=lerp(
        1.0.xxxx,
        lerp(homogeneousTransmittances,pathTransmittances,
             heterogeneousMask),activeMask);
    result.absorptions=lerp(
        0.0.xxxx,
        lerp(homogeneousAbsorptions,pathAbsorptions,
             heterogeneousMask),activeMask);
    result.centroidDistances=lerp(
        0.0.xxxx,
        lerp(homogeneousCentroids,pathCentroids,
             heterogeneousMask),activeMask);
    // 長さ0はCPU版と同じ恒等輸送であり、直前の条件付き状態と位相を保持する。
    // 物理的な雲帯間隔は呼び出し側が明示的にactiveを解除する。
    state.active=max(state.active,activeMask);
    return result;
}
// 一つの横断面を通る全光路の終端でだけ、各レーンを指数変換して面積平均する。
float cloudDensityLaneTransmittance(
    float4 requestedDensityLanes,float opticalScale){
    float result=1.0;
    if(opticalScale==opticalScale&&opticalScale>0.0){
        bool infiniteScale=opticalScale>3.402823466e+38;
        float4 safeDensity=float4(
            cloudValueIsFinite(requestedDensityLanes.x)
                &&requestedDensityLanes.x>0.0?requestedDensityLanes.x:0.0,
            cloudValueIsFinite(requestedDensityLanes.y)
                &&requestedDensityLanes.y>0.0?requestedDensityLanes.y:0.0,
            cloudValueIsFinite(requestedDensityLanes.z)
                &&requestedDensityLanes.z>0.0?requestedDensityLanes.z:0.0,
            cloudValueIsFinite(requestedDensityLanes.w)
                &&requestedDensityLanes.w>0.0?requestedDensityLanes.w:0.0);
        float4 laneTransmittance=1.0.xxxx;
        if(infiniteScale){
            laneTransmittance=float4(
                safeDensity.x>0.0?0.0:1.0,
                safeDensity.y>0.0?0.0:1.0,
                safeDensity.z>0.0?0.0:1.0,
                safeDensity.w>0.0?0.0:1.0);
        }else{
            laneTransmittance=exp(-min(
                safeDensity*opticalScale,80.0.xxxx));
        }
        result=saturate(dot(
            laneTransmittance,CLOUD_SUBRAY_AREA_WEIGHTS));
    }
    return result;
}
float cloudDensityLaneOpticalDepth(
    float4 densityLanes,float opticalScale){
    return -log(max(
        cloudDensityLaneTransmittance(densityLanes,opticalScale),
        1.0e-8));
}
// 詳細テクスチャを読む前に、密度側で混合した場合の保守的な上限を求める。
float cloudDetailFilteredDensityUpperBound(
    float maximumBasePotential,
    float billowVisibility,float erosionVisibility){
    float baseDensity=cloudCondensationDensity(maximumBasePotential);
    float billowDensity=cloudCondensationDensity(
        maximumBasePotential+CLOUD_CONDENSATION_MAXIMUM_BILLOW_OFFSET);
    float erosionDensity=cloudCondensationDensity(
        maximumBasePotential+CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET);
    float completedDensity=cloudCondensationDensity(
        maximumBasePotential
        +CLOUD_CONDENSATION_MAXIMUM_BILLOW_OFFSET
        +CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET);
    float withoutErosionDensity=lerp(
        baseDensity,billowDensity,saturate(billowVisibility));
    float withErosionDensity=lerp(
        erosionDensity,completedDensity,saturate(billowVisibility));
    return lerp(
        withoutErosionDensity,withErosionDensity,
        saturate(erosionVisibility));
}
// 詳細体積の二領域差が基本形状を動かせる最大量。
// 雲頂ほど房状の盛り上がりを強くし、雲底は輪郭が沸騰しない範囲へ抑える。
float cloudBillowMaximumOffset(float height){
    // 低周波房の周期は維持し、雲頂側だけ変位幅を広げて大きな塊の中に
    // 膨らみと谷を作る。高周波の振幅を増やすより、粒状化とちらつきを抑えられる。
    return lerp(0.024,0.130,smoothstep(0.18,0.92,saturate(height)));
}
// 合成値 G から 2・3 段目だけを復元する。生成式は G=0.55R+0.30B+0.15C なので、
// R を除いた値は (G-0.55R)/0.45 で求められ、追加の体積採取を必要としない。
float cloudDetailMiddleBand(float2 detailBands){
    return saturate((detailBands.g-detailBands.r*0.55)*(1.0/0.45));
}
// xへ粗い房、yへ中間帯域まで解像した房を返す。LOD可視率はポテンシャルへ掛けず、
// 後段で各状態の密度を面積割合として混ぜる。
float2 cloudBillowPotentialStates(
    float2 detailA,float2 detailB,float height){
    float topMiddleWeight=0.48*smoothstep(0.38,0.90,saturate(height));
    float coarseDifference=detailA.r-detailB.r;
    float middleDifference=cloudDetailMiddleBand(detailA)-cloudDetailMiddleBand(detailB);
    float maximumOffset=cloudBillowMaximumOffset(height);
    return float2(
        coarseDifference,
        lerp(coarseDifference,middleDifference,topMiddleWeight))
        *maximumOffset;
}
// 二つの独立領域の同じ高周波帯を引き、LODで消しても平均ポテンシャルを変えない侵食を作る。
float cloudErosionPotentialOffset(
    float2 detailA,float2 detailB,float height){
    float signedDetail=detailA.g-detailB.g;
    float amplitude=lerp(
        0.045,CLOUD_CONDENSATION_MAXIMUM_EROSION_OFFSET,
        smoothstep(0.18,0.92,saturate(height)));
    return signedDetail*amplitude;
}
// 採取間隔が各帯域の半周期へ近づく前に細部を消し、別の低周波模様への折り返しを防ぐ。
float cloudShapeMaximumDomainFootprint(
    float3 physicalFootprint,bool upperBand){
    float3 physicalWidth=max(physicalFootprint,0.0.xxx);
    // 球殻高度の勾配は長さ1なので、軸別箱の対角長を高度方向の最大幅として使う。
    // これにより接平面原点から離れた場所でも、世界Yだけを高度幅とみなさない。
    float altitudeWidth=length(physicalWidth);
    float inverseThickness=upperBand
        ?cloudUpperLayer.z:cloudFrameTerms.w;
    float bandScale=upperBand?0.25:1.0;
    float2 shearDerivative=float2(0.9284767,0.3713907)
        *(850.0*bandScale*max(inverseThickness,0.0));
    // 高度せん断は物理高さに対して線形なので、そのヤコビアンをXZ幅へ明示的に足す。
    // 天候・渦による非線形変形を低周波座標から除いたため、これが全変換の上限となる。
    float3 materialWidth=float3(
        physicalWidth.x+abs(shearDerivative.x)*altitudeWidth,
        altitudeWidth,
        physicalWidth.z+abs(shearDerivative.y)*altitudeWidth);
    float3 canonicalWidth=materialWidth*cloudShapeScale();
    // rotateNoiseの直交基底へ軸別の箱幅を射影する。X・Z・高さを一つの
    // 物理幅へ潰さず、各軸の担当範囲から最悪の領域幅だけを求める。
    float3 rotatedWidth=float3(
        0.8*canonicalWidth.y+0.6*canonicalWidth.z,
        0.7071068*canonicalWidth.x+0.4242641*canonicalWidth.y
            +0.5656854*canonicalWidth.z,
        0.7071068*canonicalWidth.x+0.4242641*canonicalWidth.y
            +0.5656854*canonicalWidth.z);
    return max(rotatedWidth.x,max(rotatedWidth.y,rotatedWidth.z));
}
// 完成形状の最高周波数から中間、最粗形状、解析的な未解像分布へ順に移る。
// 最粗周期より広い担当幅で点標本を残さず、遠景の折り返しと移動時のちらつきを防ぐ。
float4 cloudShapeFrequencyWeights(float maximumDomainFootprint){
    float4 result=float4(0.0,0.0,0.0,1.0);
    if(cloudCondensationValueIsFinite(maximumDomainFootprint)){
        float safeFootprint=max(maximumDomainFootprint,0.0);
        float fineVisibility=1.0-smoothstep(
            0.25,0.50,safeFootprint*8.0);
        float middleVisibility=1.0-smoothstep(
            0.25,0.50,safeFootprint*4.0);
        float coarseVisibility=1.0-smoothstep(
            0.25,0.50,safeFootprint*2.0);
        float coarseOwner=(1.0-fineVisibility)*(1.0-middleVisibility);
        result=float4(
            coarseOwner*coarseVisibility,
            (1.0-fineVisibility)*middleVisibility,
            fineVisibility,coarseOwner*(1.0-coarseVisibility));
    }
    return result;
}
// xyzへ最粗・中間・完成形状の符号付きポテンシャルを返す。
// 正値化前には混ぜず、各帯域の密度へ変換した後で担当幅の重みを適用する。
float4 cloudShapePotentialBands(float3 uvw){
    float4 densityBands=shapeNoise.SampleLevel(
        shapeNoise_sampler,uvw,0);
    return float4(
        cloudSignedPotentialFromStored(densityBands.r),
        cloudSignedPotentialFromStored(densityBands.g),
        cloudSignedPotentialFromStored(densityBands.b),-1.0);
}
// 空セル探索用の最大ポテンシャルだけを読み、実密度標本の形状採取と分離する。
float cloudShapeOccupancyMaximum(
    float3 uvw,float maximumDomainFootprint){
    float4 occupancyBands=shapeOccupancy.SampleLevel(
        shapeOccupancy_sampler,uvw,0);
    float result=1.0;
    if(cloudCondensationValueIsFinite(maximumDomainFootprint)){
        float footprintVoxels=max(maximumDomainFootprint,0.0)*128.0;
        float occupancyStoredPotential=occupancyBands.a;
        if(footprintVoxels>0.0) occupancyStoredPotential=occupancyBands.r;
        if(footprintVoxels>4.0) occupancyStoredPotential=occupancyBands.g;
        if(footprintVoxels>16.0) occupancyStoredPotential=occupancyBands.b;
        // 最大包絡より広い区間は誤って捨てず、密度側の低周波形状で細密探索する。
        if(footprintVoxels>64.0) occupancyStoredPotential=1.0;
        result=cloudSignedPotentialFromStored(occupancyStoredPotential);
    }
    return result;
}
// 二領域を混ぜた被覆値の実測百分位へ合わせ、入力0.1/0.5/0.9が
// およそ10%/52%/90%の正の被覆領域になるようにする。
float cloudWeatherThreshold(float coverage){
    return lerp(0.72,0.36,saturate(coverage));
}
float cloudWeatherMaskFromTerms(
    float4 weather,float threshold,float inverseTransitionWidth){
    float t=saturate((weather.r-threshold)*inverseTransitionWidth);
    return t*t*(3.0-2.0*t);
}
// 積乱雲の上部では被覆の下側だけを横へ広げ、雲底と雲頂の境界は動かさない。
// smoothstep の幅は元の被覆と同じにして、かなとこだけを急な板へしない。
float cloudWeatherMaskFromTermsForLayer(float4 weather,float threshold,float inverseTransitionWidth,float layerHeight,float toweringStrength){
    float waistThreshold=threshold+cloudConvectiveWaistThresholdOffset(layerHeight,toweringStrength);
    float narrowedBaseMask=cloudWeatherMaskFromTerms(weather,waistThreshold,inverseTransitionWidth);
    float expansion=cloudAnvilCoverageExpansion(layerHeight,toweringStrength);
    float anvilThreshold=threshold-expansion;
    float anvilMask=cloudWeatherMaskFromTerms(weather,anvilThreshold,inverseTransitionWidth);
    // かなとこ拡張量が0ならanvilMaskは未補正の被覆と等しい。
    // そこでmaxを取ると中層のくびれが常に打ち消されるため、拡張量を
    // かなとこの混合率として使い、本体から上部の張り出しへ連続的に戻す。
    float anvilBlend=saturate(expansion*14.285714);
    return lerp(narrowedBaseMask,anvilMask,anvilBlend);
}
// rayセル中央の3D物質座標から、セル全体を包む最大密度と層番号を返す。
// 天候と高さの包絡は正方向へ増幅しないため、形状・雲底・詳細の最大だけで上限になる。
float2 cloudShapeOccupancyAtInterval(
    float3 p,float3 physicalFootprint){
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(altitude,upperBand);
    float maximumDomainFootprint=cloudShapeMaximumDomainFootprint(
        physicalFootprint,upperBand);
    float4 frequencyWeights=cloudShapeFrequencyWeights(
        maximumDomainFootprint);
    float maximumShapePotential=cloudShapeOccupancyMaximum(
        cloudUVW(p,layerHeight,upperBand),maximumDomainFootprint);
    float maximumPositiveOffset=
        CLOUD_CONDENSATION_MAXIMUM_POSITIVE_OFFSET;
    float pointOwner=max(
        frequencyWeights.x+frequencyWeights.y+frequencyWeights.z,0.0);
    float pointDensityUpper=cloudCondensationDensity(
        maximumShapePotential+maximumPositiveOffset);
    float unresolvedDensityUpper=cloudCondensationDensity(
        CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL
        +maximumPositiveOffset);
    float densityUpper=pointOwner*pointDensityUpper
        +max(frequencyWeights.w,0.0)*unresolvedDensityUpper;
    return float2(
        densityUpper,
        upperBand?1.0:0.0);
}

// 一つの実密度標本で再利用する天候、渦、3D凝結場と高さ条件を保持する。
// 担当区間を包む占有判定は別の中央標本で完結させ、この点標本へ最大値を混ぜない。
struct CloudMacroSample {
    float4 weather;
    float2 curl;
    // xyzは最粗・中間・完成形状の符号付き3D湿度。正値化前には混ぜない。
    float4 shapePotential;
    // xyzは各点形状、wは最粗周期も未解像な解析分布の非負重み。合計は1。
    float4 shapeFrequencyWeights;
    // 高さ別のかなとこ・くびれを加える前の天候域内部位置。
    float columnInterior;
    // 作者指定の積乱雲強度を低周波天候場で局所化した値。高さ、くびれ、かなとこで共有する。
    float toweringStrength;
    // 最終密度の被覆境界から雲柱内部までを表す補間値。
    float densityWeatherMask;
    // 凝結場へ偏りとして加える縦分布。
    float heightProfile;
    // 全球の雲殻内での物理高さ。3D基本形状の等方な座標にだけ使う。
    float layerHeight;
    // 球殻基準の高度。世界Yや原点再配置に依存しない詳細3D座標に使う。
    float altitude;
    // 全球雲帯内で正規化した高さ。縦分布と形状制御に使う。
    float height;
    // 凝結場が使う全球雲帯の幅。環境光の経路長と互換に保つ。
    float columnSpan;
    // 高度計算で確定した層。後段の密度と積分尺度で再利用し、同じ高度を再計算しない。
    float upperBand;
};
// キャッシュ外でも、局所横断面密度を雲柱境界までの有効光学的深さへ変換する。
// xyは空方向と地面方向で、各レーンを全経路の終端まで保持して評価する。
float2 cloudAmbientFallbackOpticalDepth(
    CloudMacroSample macro,float4 densityLanes,
    float densityScale,float extinction){
    bool upperBand=macro.upperBand>0.5;
    float bandThickness=upperBand
        ?cloudUpperLayer.y-cloudUpperLayer.x:layer.y-layer.x;
    float columnThickness=max(bandThickness,0.0)
                         *max(macro.columnSpan,0.0);
    float h=saturate(macro.height);
    float commonScale=columnThickness
        *cloudOpticalDepthScaleFromBand(upperBand)
        *max(densityScale,0.0)*max(extinction,0.0);
    return float2(
        cloudDensityLaneOpticalDepth(
            densityLanes,commonScale*(1.0-h)),
        cloudDensityLaneOpticalDepth(
            densityLanes,commonScale*h));
}
// 天候しきい値と縦・横の物理担当幅から、主視線・光路・環境光で共通する低周波状態を作る。
// 被覆率からしきい値を作る経路と、CPUで先に作ったしきい値を使う経路の処理順を揃える。
CloudMacroSample sampleCloudMacroFromThreshold(
    float3 p,float coverageThreshold,float inverseTransitionWidth,
    float3 longitudinalFootprint,float3 transverseFootprint){
    CloudMacroSample macro;
    macro.weather=float4(0,0,0,0);
    macro.curl=float2(0,0);
    macro.shapePotential=-1.0.xxxx;
    macro.shapeFrequencyWeights=float4(0.0,0.0,0.0,1.0);
    macro.columnInterior=0.0;
    macro.toweringStrength=0.0;
    macro.densityWeatherMask=0.0;
    macro.heightProfile=0.0;
    macro.layerHeight=0.0;
    macro.altitude=0.0;
    macro.height=0.0;
    macro.columnSpan=1.0;
    macro.upperBand=0.0;
    float altitude=cloudAltitude(p);
    macro.altitude=altitude;
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(
        altitude,upperBand);
    macro.layerHeight=layerHeight;
    // 担当幅は未解像周波数の除去だけへ使う。非線形密度より前に面積平均すると、
    // 晴天列と濃い雲列が一様な灰色媒質になるため、値そのものは一点で評価する。
    float3 safeLongitudinalFootprint=max(
        longitudinalFootprint,0.0.xxx);
    float3 safeTransverseFootprint=max(
        transverseFootprint,0.0.xxx);
    float3 safeFootprint=
        safeLongitudinalFootprint+safeTransverseFootprint;
    macro.weather=cloudWeatherDataBandLimitedPoint(
        p,safeFootprint.xz);
    macro.upperBand=upperBand?1.0:0.0;
    // 上層の被覆割合は密度倍率ではなく面積へ適用し、下層と相関した薄膜を作らない。
    float layerCoverageThreshold=coverageThreshold;
    float layerInverseTransitionWidth=inverseTransitionWidth;
    if(upperBand){
        float lowerCoverage=saturate(
            (0.72-coverageThreshold)*(1.0/0.36));
        layerCoverageThreshold=cloudWeatherThreshold(
            lowerCoverage*cloudUpperTerms.x);
        float upperTransition=min(
            layerCoverageThreshold+0.14,0.98);
        layerInverseTransitionWidth=1.0/max(
            upperTransition-layerCoverageThreshold,0.001);
    }
    // 天候は高さへ押し出した密度ではなく、3D凝結場を偏らせる広域条件として使う。
    macro.columnInterior=cloudWeatherMaskFromTerms(
        macro.weather,layerCoverageThreshold,
        layerInverseTransitionWidth);
    macro.toweringStrength=cloudLocalToweringStrength(macro.weather,macro.columnInterior);
    // 小さな2D天候島ごとに層厚全体を伸縮すると柱になるため、物理雲帯の高さを直接使う。
    macro.height=layerHeight;
    macro.columnSpan=1.0;
    macro.densityWeatherMask=cloudWeatherMaskFromTermsForLayer(
        macro.weather,layerCoverageThreshold,
        layerInverseTransitionWidth,
        macro.height,macro.toweringStrength);
    // 対流位相を雲縁だけでなく高さ分布へも伝え、移流だけでは表現できない
    // 雲の局所的な発達・衰退を作る。積雲ほど応答を大きくし、層雲は安定させる。
    float convectionResponse=lerp(
        0.018,0.06,saturate(macro.toweringStrength));
    float convectionHeight=saturate(
        macro.height+cloudLocalConvectionPhase(macro.weather)
            *convectionResponse);
    macro.heightProfile=saturate(cloudProfile(
        convectionHeight,macro.weather.g,macro.toweringStrength,
        macro.columnSpan,upperBand));
    macro.curl=cloudCurlOffset(p,safeFootprint.xz);
    float maximumDomainFootprint=cloudShapeMaximumDomainFootprint(
        safeFootprint,upperBand);
    macro.shapePotential=cloudShapePotentialBands(
        cloudUVW(p,macro.layerHeight,upperBand));
    macro.shapeFrequencyWeights=cloudShapeFrequencyWeights(
        maximumDomainFootprint);
    return macro;
}
CloudMacroSample sampleCloudMacro(
    float3 p,float4 coverageTerms,
    float3 longitudinalFootprint,float3 transverseFootprint){
    return sampleCloudMacroFromThreshold(
        p,coverageTerms.y,cloudCoverageReciprocals.y,
        longitudinalFootprint,transverseFootprint);
}
CloudMacroSample sampleCloudMacroLightingBandLimited(
    float3 p,float weatherCoverage,
    float3 longitudinalFootprint,float3 transverseFootprint){
    float coverageThreshold=cloudWeatherThreshold(weatherCoverage);
    float upperThreshold=min(coverageThreshold+0.14,0.98);
    float inverseTransitionWidth=
        1.0/max(upperThreshold-coverageThreshold,0.001);
    return sampleCloudMacroFromThreshold(
        p,coverageThreshold,inverseTransitionWidth,
        longitudinalFootprint,transverseFootprint);
}
CloudMacroSample sampleCloudMacroLighting(
    float3 p,float weatherCoverage){
    return sampleCloudMacroLightingBandLimited(
        p,weatherCoverage,0.0.xxx,0.0.xxx);
}
// 光路標本が担当する線分を軸別の物理幅へ変換する。方向を一つの最大幅へ
// 潰さないことで、斜め光路でも実際に横切らない軸の低周波化を避ける。
CloudMacroSample sampleCloudMacroLightingSegment(
    float3 p,float weatherCoverage,float3 rayDirection,float sampleSpacing){
    float3 physicalFootprint=
        abs(rayDirection)*max(sampleSpacing,0.0);
    return sampleCloudMacroLightingBandLimited(
        p,weatherCoverage,physicalFootprint,0.0.xxx);
}
// 高度による雲底側の増密と降水域の増密を一つにまとめ、全ての密度経路で共有する。
float cloudHeightPrecipitationDensityScale(float height,float precipitation){
    return lerp(1.10,0.92,height)*lerp(1.0,1.28,precipitation);
}
// 二つの詳細領域で共通する回転成分を一度だけ求め、密度標本ごとの行列積を減らす。
// R の房形状は約 0.8～1.8 km、G の侵食形状は約 0.2～0.45 km を担当する。
// 旧尺度は最小形状が約 30 mしかなく、地平線の採取間隔で別の模様へ化けて粒状になっていた。
void cloudDetailDomains(
    float2 detailXz,float altitude,
    out float3 detailDomainA,out float3 detailDomainB){
    float3 horizontal=float3(
        detailXz.y*0.6000000,
        -detailXz.x*0.7071068+detailXz.y*0.5656854,
         detailXz.x*0.7071068+detailXz.y*0.5656854);
    float3 vertical=altitude*float3(
        0.8000000,-0.4242641,-0.4242641);
    // 球殻高度と水平距離へ同じ周波数を掛け、原点位置や方向で房の寸法を変えない。
    float3 rotatedPosition=horizontal+vertical;
    detailDomainA=rotatedPosition*0.00018;
    detailDomainB=rotatedPosition*0.00031;
}
// 最も細かい詳細領域 B のワールド尺度から、一標本が横切る模様の周期比を求める。
// 手入力した距離ではなく、焼き込み周波数と領域尺度を使って LOD 境界を固定する。
float cloudDetailFrequencyVisibility(float sampleSpacing,float frequency,float fadeBegin,float fadeEnd){
    float footprint=max(sampleSpacing,0.0)*0.00031*frequency;
    return 1.0-smoothstep(fadeBegin,fadeEnd,footprint);
}
// 低周波の房は画素輪郭を決めるため、半周期へ達する前まで残す。
float cloudBillowVisibilityFromSampleSpacing(float sampleSpacing){
    return cloudDetailFrequencyVisibility(sampleSpacing,4.0,0.15,0.52);
}
// 復元した中間房は8周期が主体で16周期を3分の1含む。8周期の足跡0.20で消せば、
// 16周期側も一標本あたり0.40周期に収まり、折り返さず雲頂の中規模な房を残せる。
float cloudMiddleBillowVisibilityFromSampleSpacing(float sampleSpacing){
    return cloudDetailFrequencyVisibility(sampleSpacing,8.0,0.05,0.20);
}
// 高周波侵食は一標本積分で揺れやすいため、最小周期の約4分の1までに消す。
float cloudErosionVisibilityFromSampleSpacing(float sampleSpacing){
    return cloudDetailFrequencyVisibility(sampleSpacing,16.0,0.05,0.24);
}
// 画面横方向の担当幅だけを求める。視線方向の積分間隔は別の区間として積算し、
// 3軸の箱幅へ流用しない。
float cloudProjectedPixelWidth(
    float sampleDistance,float angularPixelFootprint){
    return max(sampleDistance,0.0)*max(angularPixelFootprint,0.0);
}
// 詳細帯域の折り返し判定だけは、視線積分間隔と投影画素幅の広い方を使う。
float cloudDetailSampleSpacing(
    float integrationSpacing,float projectedPixelWidth){
    return max(max(integrationSpacing,0.0),max(projectedPixelWidth,0.0));
}
// 距離減衰は完成済みのレイへ一括適用せず、各密度標本へ適用する。
// 減衰区間が 0 の場合は smoothstep の同一端点による 0 除算を避ける。
float cloudDistanceFade(float sampleDistance,float fadeStart,float maxDistance){
    // 返り値を先に確定し、旧HLSLコンパイラーが複数returnを未初期化経路と誤認しない形にする。
    // NaNは見えない値へ倒し、有限な通常経路はCPU側と同じ三次補間で評価する。
    float fadeResult=0.0;
    float fadeLength=maxDistance-fadeStart;
    if(sampleDistance==sampleDistance){
        if(fadeLength>0.001){
            float blend=saturate(
                (sampleDistance-fadeStart)/fadeLength);
            blend=blend*blend*(3.0-2.0*blend);
            fadeResult=1.0-blend;
        }else if(sampleDistance<maxDistance){
            fadeResult=1.0;
        }
    }
    return fadeResult;
}
// 大きな絶対視線距離を引き算せず、区間始点からの相対距離でセル幅を求める。
// 最後の境界は区間幅へ固定し、二進32bitの絶対座標丸めでセルが0幅になることを防ぐ。
float cloudRayCellOffset(
    float intervalSpan,int cellIndex,int cellCount,
    float nominalCellWidth){
    float safeSpan=max(intervalSpan,0.0);
    float offset=0.0;
    if(cellIndex>=cellCount){
        offset=safeSpan;
    }else if(cellIndex>0){
        // セル数が一つ変わっても全境界を等間隔へ引き直さず、終端側の最後の
        // セルだけで端数を吸収する。別の雲帯が出入りした際の全域位相飛びを防ぐ。
        offset=min(
            float(cellIndex)*max(nominalCellWidth,1e-4),safeSpan);
    }
    return offset;
}

// 物理雲帯ごとに、表示状態では変わらない標本予算を予約する。
// 球殻の最大接線光路は層厚の平方根に比例するため、その比で下層・上層へ配る。
// 一方が画面外へ出ても他方の格子を再分配しないことが、時間方向の安定性を保つ。
int2 cloudPhysicalBandSampleBudgets(int maximumSamples){
    int safeMaximum=max(maximumSamples,1);
    int2 budgets=int2(safeMaximum,0);
    if(cloudUpperLayer.w>0.5&&safeMaximum>1){
        // 各帯へ通常描画の最低探索量を先に予約する。極端な厚さ比でも薄い帯を
        // 1標本へ縮めず、残りだけを接線光路の平方根比で分配する。
        int minimumBandBudget=min(
            CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT,safeMaximum>>1);
        int weightedBudget=max(
            safeMaximum-2*minimumBandBudget,0);
        float lowerWeight=sqrt(max(layer.y-layer.x,1e-4));
        float upperWeight=sqrt(max(
            cloudUpperLayer.y-cloudUpperLayer.x,1e-4));
        float weightSum=max(lowerWeight+upperWeight,1e-4);
        int lowerBudget=minimumBandBudget+clamp(
            (int)floor(float(weightedBudget)*lowerWeight/weightSum+0.5),
            0,weightedBudget);
        budgets=int2(lowerBudget,safeMaximum-lowerBudget);
    }
    return budgets;
}

// 同じ物理雲帯が近側・遠側へ分かれる場合、その帯へ予約済みの標本数を
// 区間長に比例して分ける。各可視区間へ最低一標本を残し、合計は変えない。
int cloudPackedIntervalSampleBudgetAt(
    CloudPackedBandIntervals intervals,int2 physicalBandBudgets,
    int targetIndex){
    float4 spans=max(intervals.ends-intervals.starts,0.0.xxxx);
    bool valid0=intervals.count>0&&spans.x>0.0;
    bool valid1=intervals.count>1&&spans.y>0.0;
    bool valid2=intervals.count>2&&spans.z>0.0;
    bool valid3=intervals.count>3&&spans.w>0.0;
    bool targetValid=targetIndex==0?valid0:
        (targetIndex==1?valid1:(targetIndex==2?valid2:valid3));
    int targetBandId=targetIndex==0?intervals.bandIds.x:
        (targetIndex==1?intervals.bandIds.y:
        (targetIndex==2?intervals.bandIds.z:intervals.bandIds.w));
    bool same0=valid0&&intervals.bandIds.x==targetBandId;
    bool same1=valid1&&intervals.bandIds.y==targetBandId;
    bool same2=valid2&&intervals.bandIds.z==targetBandId;
    bool same3=valid3&&intervals.bandIds.w==targetBandId;
    int sameCount=(same0?1:0)+(same1?1:0)
        +(same2?1:0)+(same3?1:0);
    int bandBudget=max(
        targetBandId>0?physicalBandBudgets.y:physicalBandBudgets.x,1);
    int result=0;
    if(targetValid){
        if(sameCount<=1){
            result=bandBudget;
        }else{
            bandBudget=max(bandBudget,2);
            int firstIndex=same0?0:(same1?1:(same2?2:3));
            float firstSpan=firstIndex==0?spans.x:
                (firstIndex==1?spans.y:(firstIndex==2?spans.z:spans.w));
            float bandSpan=(same0?spans.x:0.0)+(same1?spans.y:0.0)
                +(same2?spans.z:0.0)+(same3?spans.w:0.0);
            int distributableBudget=max(bandBudget-2,0);
            int firstExtra=clamp(
                (int)floor(
                    float(distributableBudget)*firstSpan/max(bandSpan,1e-4)+0.5),
                0,distributableBudget);
            int firstBudget=1+firstExtra;
            result=targetIndex==firstIndex
                ?firstBudget:bandBudget-firstBudget;
        }
    }
    return result;
}

int4 cloudPackedIntervalSampleBudgets(
    CloudPackedBandIntervals intervals,int2 physicalBandBudgets){
    return int4(
        cloudPackedIntervalSampleBudgetAt(
            intervals,physicalBandBudgets,0),
        cloudPackedIntervalSampleBudgetAt(
            intervals,physicalBandBudgets,1),
        cloudPackedIntervalSampleBudgetAt(
            intervals,physicalBandBudgets,2),
        cloudPackedIntervalSampleBudgetAt(
            intervals,physicalBandBudgets,3));
}

// 一区間の標本予算と距離LODから、終端だけが端数を吸収する細密セル幅を返す。
float cloudPackedIntervalFineStep(
    float intervalStart,float intervalEnd,int intervalBudget,
    float baseFineStep,float maximumDistance){
    float intervalSpan=max(intervalEnd-intervalStart,0.0);
    float distanceLod=1.0+cloudRange.z
        *saturate(intervalStart/max(maximumDistance,1.0));
    return max(
        max(baseFineStep,
            intervalSpan/float(max(intervalBudget,1)))*distanceLod,
        1e-4);
}

int cloudPackedIntervalFineCellCount(
    float intervalStart,float intervalEnd,int intervalBudget,
    float fineStep){
    float intervalSpan=max(intervalEnd-intervalStart,0.0);
    return clamp(
        (int)ceil(intervalSpan/max(fineStep,1e-4)),
        1,max(intervalBudget,1));
}
// 高次散乱が周囲の媒質量を判定する低LOD密度を、未解像4状態のまま返す。
float4 cloudLowLodDensityDistributionFromPositiveWeatherMacro(
    CloudMacroSample macro,float weatherMask){
    float h=saturate(macro.height);
    float commonPotential=cloudCondensationCommonPotential(
        macro.heightProfile,weatherMask,
        h,macro.toweringStrength);
    float4 condensationDistribution=
        cloudBandLimitedCondensationDistribution(
        macro.shapePotential,macro.shapeFrequencyWeights,
        commonPotential,0.0);
    return cloudScaleDensityDistribution(
        condensationDistribution,
        cloudHeightPrecipitationDensityScale(h,macro.weather.b));
}
float4 cloudLowLodDensityLanesFromPositiveWeatherMacro(
    CloudMacroSample macro,float weatherMask){
    return cloudDensityDistributionMean(
        cloudLowLodDensityDistributionFromPositiveWeatherMacro(
            macro,weatherMask)).xxxx;
}
float cloudLowLodDensityFromPositiveWeatherMacro(
    CloudMacroSample macro,float weatherMask){
    return cloudDensityDistributionMean(
        cloudLowLodDensityDistributionFromPositiveWeatherMacro(
            macro,weatherMask));
}
// 詳細表示用密度。低周波房、中間房、高周波侵食を別々の採取限界で減衰させる。
float4 cloudDensityDistributionFromPositiveWeatherMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility){
    float4 densityDistribution=0.0.xxxx;
    float h=saturate(macro.height);
    float commonPotential=cloudCondensationCommonPotential(
        macro.heightProfile,weatherMask,
        h,macro.toweringStrength);
    billowVisibility=cloudCondensationFiniteSaturate(billowVisibility);
    middleBillowVisibility=
        cloudCondensationFiniteSaturate(middleBillowVisibility);
    erosionVisibility=cloudCondensationFiniteSaturate(erosionVisibility);
    float maximumShapePotential=max(
        macro.shapePotential.x,
        max(macro.shapePotential.y,macro.shapePotential.z));
    if(macro.shapeFrequencyWeights.w>0.0)
        maximumShapePotential=max(
            maximumShapePotential,
            CLOUD_UNRESOLVED_COARSE_OUTER_POTENTIAL);
    float maximumBasePotential=maximumShapePotential+commonPotential;
    float maximumDetailedDensity=cloudDetailFilteredDensityUpperBound(
        maximumBasePotential,billowVisibility,erosionVisibility);
    float densityScale=cloudHeightPrecipitationDensityScale(
        h,macro.weather.b);
    // 局所密度しきい値では切らない。薄い密度でも長い接線経路では有限な不透明度になる。
    if(maximumDetailedDensity*densityScale>0.0){
        float2 billowPotentials=0.0.xx;
        float erosionPotential=0.0;
        float detailVisibility=max(
            billowVisibility,erosionVisibility);
        [branch] if(detailVisibility>0.001){
            // 基本凝結場とは別のメートル基準領域を使い、雲塊と細部に同じ模様を出さない。
            float2 detailXz=p.xz-cloudWindWorld()
                           +cloudHeightShapeShear(macro.layerHeight,macro.upperBand>0.5)
                           +macro.curl*35.0;
            float3 detailDomainA,detailDomainB;
            cloudDetailDomains(
                detailXz,macro.altitude,detailDomainA,detailDomainB);
            float2 ndA=detailNoise.SampleLevel(detailNoise_sampler,detailDomainA+float3(0.19,0.67,0.41)+float3(cloudEvolution.z,cloudEvolution.w,-cloudEvolution.z),0);
            float2 ndB=detailNoise.SampleLevel(detailNoise_sampler,detailDomainB+float3(0.73,0.23,0.59)+float3(-cloudEvolution.w,cloudEvolution.z,cloudEvolution.w),0);
            // 各状態を後段で密度化するため、LOD可視率を掛けない符号付き変位を保持する。
            billowPotentials=cloudBillowPotentialStates(ndA,ndB,h);
            erosionPotential=cloudErosionPotentialOffset(ndA,ndB,h);
        }
        densityDistribution=cloudScaleDensityDistribution(
            cloudDetailFilteredCondensationDistribution(
                macro.shapePotential,macro.shapeFrequencyWeights,
                commonPotential,billowPotentials,
                billowVisibility,middleBillowVisibility,
                erosionPotential,erosionVisibility),
            densityScale);
    }
    return densityDistribution;
}
float4 cloudDensityLanesFromPositiveWeatherMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDensityDistributionFromPositiveWeatherMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,
            erosionVisibility)).xxxx;
}
float cloudDensityFromPositiveWeatherMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDensityDistributionFromPositiveWeatherMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,
            erosionVisibility));
}
float4 cloudDensityDistributionFromMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility){
    float4 densityDistribution=
        cloudDensityDistributionFromPositiveWeatherMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,erosionVisibility);
    if(macro.upperBand>0.5)
        densityDistribution=cloudScaleDensityDistribution(
            densityDistribution,cloudUpperTerms.y);
    return densityDistribution;
}
float4 cloudDensityLanesFromMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDensityDistributionFromMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,
            erosionVisibility)).xxxx;
}
float cloudDensityFromMacro(float3 p,CloudMacroSample macro,float weatherMask,float billowVisibility,float middleBillowVisibility,float erosionVisibility){
    return cloudDensityDistributionMean(
        cloudDensityDistributionFromMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,
            erosionVisibility));
}
// 高次散乱の周囲媒質判定用密度を求める。
// 空間スキップ用の広い占有しきい値ではなく、詳細密度と同じ被覆・高さしきい値を使う。
float4 cloudLowLodDensityDistributionFromMacro(
    CloudMacroSample macro,float weatherMask){
    float4 densityDistribution=
        cloudLowLodDensityDistributionFromPositiveWeatherMacro(
            macro,weatherMask);
    if(macro.upperBand>0.5)
        densityDistribution=cloudScaleDensityDistribution(
            densityDistribution,cloudUpperTerms.y);
    return densityDistribution;
}
float cloudLowLodDensityFromMacro(CloudMacroSample macro,float weatherMask){
    return cloudDensityDistributionMean(
        cloudLowLodDensityDistributionFromMacro(macro,weatherMask));
}
float4 cloudLowLodDensityLanesFromMacro(
    CloudMacroSample macro,float weatherMask){
    return cloudLowLodDensityFromMacro(macro,weatherMask).xxxx;
}
float3 cloudDensityOpticalDepthByOrderFromMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float billowVisibility,float middleBillowVisibility,
    float erosionVisibility,float3 extinctionByOrder,
    float3 rayDirection,float segmentLength,
    inout CloudFourStateTransportLanes firstOrderState,
    inout CloudFourStateTransportLanes secondOrderState,
    inout CloudFourStateTransportLanes thirdOrderState){
    float4 distribution=cloudDensityDistributionFromMacro(
        p,macro,weatherMask,
        billowVisibility,middleBillowVisibility,erosionVisibility);
    distribution*=cloudOpticalDepthScaleFromBand(macro.upperBand>0.5);
    float correlationLength=
        cloudUnresolvedDensityCorrelationLengthAtDirection(
            p,rayDirection,macro.upperBand>0.5);
    float4 correlationLengths=correlationLength.xxxx;
    float4 segmentLengths=max(segmentLength,0.0).xxxx;
    CloudFourStateTransportResultLanes first=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.x,correlationLengths,segmentLengths,
            firstOrderState);
    CloudFourStateTransportResultLanes second=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.y,correlationLengths,segmentLengths,
            secondOrderState);
    CloudFourStateTransportResultLanes third=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.z,correlationLengths,segmentLengths,
            thirdOrderState);
    return cloudOpticalDepthFromAbsorption3(
        float3(
            first.absorptions.x,
            second.absorptions.x,
            third.absorptions.x));
}
float3 cloudLowLodOpticalDepthByOrderFromMacro(
    float3 p,CloudMacroSample macro,float weatherMask,
    float3 extinctionByOrder,float3 rayDirection,float segmentLength,
    inout CloudFourStateTransportLanes firstOrderState,
    inout CloudFourStateTransportLanes secondOrderState,
    inout CloudFourStateTransportLanes thirdOrderState){
    float4 distribution=cloudLowLodDensityDistributionFromMacro(
        macro,weatherMask);
    distribution*=cloudOpticalDepthScaleFromBand(macro.upperBand>0.5);
    float correlationLength=
        cloudUnresolvedDensityCorrelationLengthAtDirection(
            p,rayDirection,macro.upperBand>0.5);
    float4 correlationLengths=correlationLength.xxxx;
    float4 segmentLengths=max(segmentLength,0.0).xxxx;
    CloudFourStateTransportResultLanes first=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.x,correlationLengths,segmentLengths,
            firstOrderState);
    CloudFourStateTransportResultLanes second=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.y,correlationLengths,segmentLengths,
            secondOrderState);
    CloudFourStateTransportResultLanes third=
        cloudFourStateTransportLanes(
            distribution.x.xxxx,distribution.y.xxxx,
            distribution.z.xxxx,distribution.w.xxxx,
            extinctionByOrder.z,correlationLengths,segmentLengths,
            thirdOrderState);
    return cloudOpticalDepthFromAbsorption3(
        float3(
            first.absorptions.x,
            second.absorptions.x,
            third.absorptions.x));
}
float cloudDensity(float3 p, float coverage, float detailWeight){
    float densityResult=0.0;
    CloudMacroSample macro=sampleCloudMacroLighting(
        p,coverage);
    densityResult=cloudDensityFromMacro(
        p,macro,macro.densityWeatherMask,
        detailWeight,detailWeight,detailWeight);
    return densityResult;
}

// 低解像度影キャッシュへ足す近距離の高周波差分を、散乱次数別の有効光学的深さで求める。
float3 cloudNearLightOpticalDepthResiduals(
    float3 rayOrigin,float coverage,float3 lightDirection,
    float3 extinctionByOrder){
    float3 residuals=0.0.xxx;
    CloudPackedBandIntervals intervals=
        intersectCloudBandsFromPosition(rayOrigin,lightDirection);
    CloudFourStateTransportLanes lowFirstState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes lowSecondState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes lowThirdState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes detailedFirstState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes detailedSecondState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes detailedThirdState=
        cloudInitialFourStateTransportLanes();
    float previousSegmentEnd=-1.0;
    int previousBandId=-1;
    [loop] for(int sampleIndex=0;
               sampleIndex<CLOUD_LIGHT_DETAIL_SAMPLE_COUNT;
               ++sampleIndex){
        float rayDistance=0.0;
        float sampleSpacing=0.0;
        int sampleBandId=-1;
        if(!cloudLightSampleTerms(
               intervals,CLOUD_LIGHT_DETAIL_SAMPLE_COUNT,sampleIndex,
               rayDistance,sampleSpacing,sampleBandId)) continue;
        float segmentStart=rayDistance-0.5*sampleSpacing;
        if(previousSegmentEnd>=0.0&&
           (sampleBandId!=previousBandId||
            segmentStart>previousSegmentEnd+1e-3)){
            lowFirstState.active=0.0.xxxx;
            lowSecondState.active=0.0.xxxx;
            lowThirdState.active=0.0.xxxx;
            detailedFirstState.active=0.0.xxxx;
            detailedSecondState.active=0.0.xxxx;
            detailedThirdState.active=0.0.xxxx;
        }
        previousSegmentEnd=rayDistance+0.5*sampleSpacing;
        previousBandId=sampleBandId;
        float3 samplePosition=rayOrigin+lightDirection*rayDistance;
        CloudMacroSample macro=sampleCloudMacroLightingSegment(
            samplePosition,coverage,lightDirection,sampleSpacing);
        float3 lowLodDepth=cloudLowLodOpticalDepthByOrderFromMacro(
            samplePosition,macro,macro.densityWeatherMask,
            max(extinctionByOrder,0.0.xxx),
            lightDirection,sampleSpacing,
            lowFirstState,lowSecondState,lowThirdState);
        float billowVisibility=
            cloudBillowVisibilityFromSampleSpacing(sampleSpacing);
        float middleBillowVisibility=
            cloudMiddleBillowVisibilityFromSampleSpacing(sampleSpacing);
        float erosionVisibility=
            cloudErosionVisibilityFromSampleSpacing(sampleSpacing);
        float3 detailedDepth=cloudDensityOpticalDepthByOrderFromMacro(
            samplePosition,macro,macro.densityWeatherMask,
            billowVisibility,middleBillowVisibility,erosionVisibility,
            max(extinctionByOrder,0.0.xxx),
            lightDirection,sampleSpacing,
            detailedFirstState,detailedSecondState,detailedThirdState);
        residuals+=detailedDepth-lowLodDepth;
    }
    return residuals;
}

// キャッシュを使えない場所では、太陽円盤の各方向を相関長で適応積分する。
// xyzは一次・二次・三次の消散率で求めた光学的深さ。
// 光路方向の未解像密度は相関長ごとの透過率期待値へ閉じ、区間ごとの深さを加算する。
float3 traceCloudMainLightDepths(
    float3 rayOrigin,float coverage,float3 lightDirection,
    float3 extinctionByOrder){
    float3 lightDepths=0.0.xxx;
    CloudPackedBandIntervals intervals=
        intersectCloudBandsFromPosition(rayOrigin,lightDirection);
    CloudFourStateTransportLanes firstOrderState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes secondOrderState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes thirdOrderState=
        cloudInitialFourStateTransportLanes();
    int4 sampleCounts=cloudAdaptiveLightSampleCounts(
        intervals,CLOUD_LIGHT_MARCH_SAMPLE_COUNT,
        rayOrigin,lightDirection);
    float previousSegmentEnd=-1.0;
    int previousBandId=-1;
    [loop] for(int sampleIndex=0;
               sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;
               ++sampleIndex){
        float rayDistance=0.0;
        float sampleSpacing=0.0;
        int sampleBandId=-1;
        if(!cloudAdaptiveLightSampleTerms(
               intervals,sampleCounts,sampleIndex,
               rayDistance,sampleSpacing,sampleBandId)) continue;
        float segmentStart=rayDistance-0.5*sampleSpacing;
        if(previousSegmentEnd>=0.0&&
           (sampleBandId!=previousBandId||
            segmentStart>previousSegmentEnd+1e-3)){
            firstOrderState.active=0.0.xxxx;
            secondOrderState.active=0.0.xxxx;
            thirdOrderState.active=0.0.xxxx;
        }
        previousSegmentEnd=rayDistance+0.5*sampleSpacing;
        previousBandId=sampleBandId;
        float3 samplePosition=rayOrigin+lightDirection*rayDistance;
        CloudMacroSample macro=sampleCloudMacroLightingSegment(
            samplePosition,coverage,lightDirection,sampleSpacing);
        float billowVisibility=
            cloudBillowVisibilityFromSampleSpacing(sampleSpacing);
        float middleBillowVisibility=
            cloudMiddleBillowVisibilityFromSampleSpacing(sampleSpacing);
        float erosionVisibility=
            cloudErosionVisibilityFromSampleSpacing(sampleSpacing);
        // 低詳細度密度へ近距離3点だけを足さず、担当幅で表現できる完成密度を積分する。
        float3 lightSegmentDepth=cloudDensityOpticalDepthByOrderFromMacro(
            samplePosition,macro,macro.densityWeatherMask,
            billowVisibility,middleBillowVisibility,erosionVisibility,
            max(extinctionByOrder,0.0.xxx),
            lightDirection,sampleSpacing,
            firstOrderState,secondOrderState,thirdOrderState);
        lightDepths+=lightSegmentDepth;
        if(lightDepths.z>18.0) break;
    }
    return lightDepths;
}

// 移流を除いた安定XZ座標と高度から、現在の曲面雲層上の点を復元する。
// 有理化した沈み量により地球半径同士の減算を避け、下層・上層で同じ曲面を使う。
float3 cloudShadowWorldPositionAtAltitude(float2 worldXz,float altitude){
    float2 d=worldXz-worldOrigin.xz;
    float radius=CLOUD_PLANET_RADIUS+altitude;
    float d2=dot(d,d);
    float root=sqrt(max(radius*radius-d2,0.0));
    float sag=d2/max(radius+root,1.0);
    return float3(worldXz.x,worldOrigin.y+altitude-sag,worldXz.y);
}

// 中央の等間隔半径を画素数から求め、CPUの中心追従半径と別の即値にしない。
float cloudAmbientCacheUniformRadius(float inverseExtent){
    float cellWorldSize=shadowState.z/max(inverseExtent,1e-8);
    return CLOUD_AMBIENT_CACHE_UNIFORM_RADIUS_CELLS*cellWorldSize;
}

// 環境光キャッシュの一軸を、中央の等間隔と外周の二次拡張から物質座標へ写す。
float cloudAmbientCacheMaterialAxis(float textureAxis,float minimumAxis,float inverseExtent){
    float halfExtent=0.5/max(inverseExtent,1e-8);
    float centerAxis=minimumAxis+halfExtent;
    float uniformRadius=min(cloudAmbientCacheUniformRadius(inverseExtent),halfExtent);
    float centralFraction=saturate(uniformRadius/max(halfExtent,1.0));
    float signedAxis=textureAxis*2.0-1.0;
    float absoluteAxis=abs(signedAxis);
    float materialAxis=centerAxis+signedAxis*halfExtent;
    if(absoluteAxis>centralFraction&&centralFraction<1.0-1e-4){
        float outerTextureSpan=max(1.0-centralFraction,1e-4);
        float outerT=(absoluteAxis-centralFraction)/outerTextureSpan;
        float outerWorldSpan=max(halfExtent-uniformRadius,1.0);
        float guardCoefficient=max(shadowState.w,0.0);
        float worldOffset=uniformRadius+outerWorldSpan
            *(outerT+0.5*guardCoefficient*outerT*outerT);
        materialAxis=centerAxis+(signedAxis<0.0?-worldOffset:worldOffset);
    }
    return materialAxis;
}

// 物質座標から非一様な環境光キャッシュ座標へ戻す。生成側の二次式の正根だけを使う。
float cloudAmbientCacheTextureAxis(float materialAxis,float minimumAxis,float inverseExtent){
    float halfExtent=0.5/max(inverseExtent,1e-8);
    float centerAxis=minimumAxis+halfExtent;
    float uniformRadius=min(cloudAmbientCacheUniformRadius(inverseExtent),halfExtent);
    float centralFraction=saturate(uniformRadius/max(halfExtent,1.0));
    float worldOffset=materialAxis-centerAxis;
    float absoluteOffset=abs(worldOffset);
    float textureAxis=0.5+0.5*worldOffset/max(halfExtent,1.0);
    if(absoluteOffset>uniformRadius&&centralFraction<1.0-1e-4){
        float outerTextureSpan=max(1.0-centralFraction,1e-4);
        float outerWorldSpan=max(halfExtent-uniformRadius,1.0);
        float normalizedOffset=max((absoluteOffset-uniformRadius)/outerWorldSpan,0.0);
        float guardCoefficient=max(shadowState.w,0.0);
        // 二次式の正根を有理化し、係数が小さいときの減算による桁落ちを避ける。
        float outerT=2.0*normalizedOffset
            /max(sqrt(max(1.0+2.0*guardCoefficient*normalizedOffset,0.0))+1.0,1e-4);
        float absoluteAxis=centralFraction+outerTextureSpan*outerT;
        float signedAxis=worldOffset<0.0?-absoluteAxis:absoluteAxis;
        textureAxis=0.5+0.5*signedAxis;
    }
    return textureAxis;
}

// 風移流前の物質座標を、環境光キャッシュの非一様な水平座標へ戻す。
float2 cloudAmbientCacheTexturePosition(float2 materialXz){
    return float2(cloudAmbientCacheTextureAxis(materialXz.x,shadowGrid.x,shadowGrid.z),cloudAmbientCacheTextureAxis(materialXz.y,shadowGrid.y,shadowGrid.w));
}

// 非一様な一画素の物理境界を四等分し、担当する小領域の中点と幅を返す。
// テクスチャ座標を等分すると外周の物理面積が不均一になるため、写像後に分ける。
void cloudAmbientCacheAxisSample(float textureAxis,uint sampleIndex,float minimumAxis,float inverseExtent,out float samplePosition,out float sampleWidth){
    float halfTexel=0.5*shadowState.z;
    float lowerBoundary=cloudAmbientCacheMaterialAxis(textureAxis-halfTexel,minimumAxis,inverseExtent);
    float upperBoundary=cloudAmbientCacheMaterialAxis(textureAxis+halfTexel,minimumAxis,inverseExtent);
    uint boundedIndex=min(sampleIndex,CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS-1u);
    sampleWidth=max(
        (upperBoundary-lowerBoundary)
        /float(CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS),1e-4);
    samplePosition=lowerBoundary
        +(float(boundedIndex)+0.5)*sampleWidth;
}

// 一つの小領域を一スレッドだけで評価する。16標本を関数内で反復せず、
// GPUグループの16スレッドへ分けてシェーダーコンパイラの式展開を防ぐ。
float cloudAmbientQuadratureDensity(
    float2 worldXz,float2 horizontalFootprint,
    float altitude,float coverage,float verticalStep){
    float3 p=cloudShadowWorldPositionAtAltitude(worldXz,altitude);
    float3 physicalFootprint=float3(
        max(horizontalFootprint.x,0.0),max(verticalStep,0.0),
        max(horizontalFootprint.y,0.0));
    CloudMacroSample macro=sampleCloudMacroLightingBandLimited(
        p,coverage,physicalFootprint,0.0.xxx);
    return cloudLowLodDensityFromMacro(
        macro,macro.densityWeatherMask);
}

// 影キャッシュは始点から雲殻出口までを相関長に応じた区間で覆う。
// 光路方向へ平均した密度の光学的深さを加算し、区間数で透過率を変えない。
float3 traceCloudShadowDepths(
    float3 rayOrigin,float coverage,float3 lightDirection,
    float3 extinctionByOrder){
    float3 lightDepths=0.0.xxx;
    CloudPackedBandIntervals intervals=
        intersectCloudBandsFromPosition(rayOrigin,lightDirection);
    CloudFourStateTransportLanes firstOrderState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes secondOrderState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes thirdOrderState=
        cloudInitialFourStateTransportLanes();
    int4 sampleCounts=cloudAdaptiveLightSampleCounts(
        intervals,CLOUD_LIGHT_MARCH_SAMPLE_COUNT,
        rayOrigin,lightDirection);
    float previousSegmentEnd=-1.0;
    int previousBandId=-1;
    [loop] for(int sampleIndex=0;
               sampleIndex<CLOUD_LIGHT_MARCH_SAMPLE_COUNT;
               ++sampleIndex){
        float rayDistance=0.0;
        float sampleSpacing=0.0;
        int sampleBandId=-1;
        if(!cloudAdaptiveLightSampleTerms(
               intervals,sampleCounts,sampleIndex,
               rayDistance,sampleSpacing,sampleBandId)) continue;
        float segmentStart=rayDistance-0.5*sampleSpacing;
        if(previousSegmentEnd>=0.0&&
           (sampleBandId!=previousBandId||
            segmentStart>previousSegmentEnd+1e-3)){
            firstOrderState.active=0.0.xxxx;
            secondOrderState.active=0.0.xxxx;
            thirdOrderState.active=0.0.xxxx;
        }
        previousSegmentEnd=rayDistance+0.5*sampleSpacing;
        previousBandId=sampleBandId;
        float3 samplePosition=rayOrigin+lightDirection*rayDistance;
        CloudMacroSample lightMacro=sampleCloudMacroLightingSegment(
            samplePosition,coverage,lightDirection,sampleSpacing);
        float3 lightSegmentDepth=cloudLowLodOpticalDepthByOrderFromMacro(
            samplePosition,lightMacro,lightMacro.densityWeatherMask,
            max(extinctionByOrder,0.0.xxx),
            lightDirection,sampleSpacing,
            firstOrderState,secondOrderState,thirdOrderState);
        lightDepths+=lightSegmentDepth;
    }
    return lightDepths;
}

// 同一点で積分した太陽円盤4光路の一次・二次・三次透過率と混合率を返す。
// 上層がある場合は縦32画素を下層16・上層16へ分け、別の雲帯を誤って採取しない。
void sampleCloudSunTransmittance(
    float3 lp,out float cacheWeight,
    out float4 firstVisibility,out float4 secondVisibility,
    out float4 thirdVisibility){
    // 全ての棄却経路で遮蔽なしを保ち、混合率0の正確積分へ安全に戻す。
    firstVisibility=1.0.xxxx;
    secondVisibility=1.0.xxxx;
    thirdVisibility=1.0.xxxx;
    cacheWeight=0.0;
    if(shadowState.x>0.5){
        float2 q=lp.xz-cloudWindWorld();
        float altitude=cloudAltitude(lp);
        bool inLowerBand=altitude>=layer.x&&altitude<=layer.y;
        bool inUpperBand=cloudUpperLayer.w>0.5&&
            altitude>=cloudUpperLayer.x&&altitude<=cloudUpperLayer.y;
        bool upperBand=inUpperBand;
        float h=upperBand
            ?(altitude-cloudUpperLayer.x)
                /max(cloudUpperLayer.y-cloudUpperLayer.x,1e-4)
            :(altitude-layer.x)/max(layer.y-layer.x,1e-4);
        float logicalHeight=float(CLOUD_SHADOW_CACHE_HEIGHT);
        float textureHeight=float(CLOUD_SHADOW_CACHE_TEXTURE_HEIGHT);
        float profileIndex=0.5+h*(logicalHeight-1.0);
        if(cloudUpperLayer.w>0.5){
            float bandCacheHeight=0.5*logicalHeight;
            profileIndex=(upperBand?bandCacheHeight+0.5:0.5)
                +h*(bandCacheHeight-1.0);
        }
        float2 uvwXz=float2(
            (q.x-shadowGrid.x)*shadowGrid.z,
            (q.y-shadowGrid.y)*shadowGrid.w);
        // 範囲外の位置を無関係な端の画素へ固定しない。線形補間が境界をまたがないよう、
        // 水平面に1.5画素分の余白を残す。縦座標は各雲帯の画素中心へ明示的に写す。
        float2 edgeCells=min(uvwXz,1.0-uvwXz)/shadowState.z;
        float minimumEdgeCells=min(edgeCells.x,edgeCells.y);
        if((inLowerBand||inUpperBand)&&minimumEdgeCells>CLOUD_SHADOW_CACHE_FILTER_START_CELLS){
            float borderWeight=smoothstep(CLOUD_SHADOW_CACHE_FILTER_START_CELLS,CLOUD_SHADOW_CACHE_FILTER_FULL_CELLS,minimumEdgeCells);
            float3 firstUvw=float3(
                uvwXz.x,(logicalHeight+profileIndex)/textureHeight,uvwXz.y);
            float3 secondUvw=float3(
                uvwXz.x,(2.0*logicalHeight+profileIndex)/textureHeight,uvwXz.y);
            float3 thirdUvw=float3(
                uvwXz.x,(3.0*logicalHeight+profileIndex)/textureHeight,uvwXz.y);
            float4 cachedFirst=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,firstUvw,0);
            float4 cachedSecond=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,secondUvw,0);
            float4 cachedThird=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,thirdUvw,0);
            bool finiteValue=all(cachedFirst==cachedFirst)
                          && all(cachedSecond==cachedSecond)
                          && all(cachedThird==cachedThird)
                          && all(cachedFirst>=0.0)&&all(cachedFirst<=1.001)
                          && all(cachedSecond>=0.0)&&all(cachedSecond<=1.001)
                          && all(cachedThird>=0.0)&&all(cachedThird<=1.001);
            if(finiteValue){
                cacheWeight=borderWeight;
                firstVisibility=saturate(cachedFirst);
                secondVisibility=saturate(cachedSecond);
                thirdVisibility=saturate(cachedThird);
            }
        }
    }
}

// 現在地点から空と地面までの積算密度と、キャッシュ境界の混合率を返す。
// 下層と上層は同じ正規化高度のRGBAへ別々に保持し、層間を越える光路も含める。
float3 sampleCloudAmbientVisibility(float3 p){
    float3 result=float3(0.0,1.0,1.0);
    if(shadowState.x>0.5){
        float2 q=p.xz-cloudWindWorld();
        float altitude=cloudAltitude(p);
        bool inLowerBand=altitude>=layer.x&&altitude<=layer.y;
        bool inUpperBand=cloudUpperLayer.w>0.5&&
            altitude>=cloudUpperLayer.x&&altitude<=cloudUpperLayer.y;
        bool upperBand=inUpperBand;
        float h=upperBand
            ?(altitude-cloudUpperLayer.x)
                /max(cloudUpperLayer.y-cloudUpperLayer.x,1e-4)
            :(altitude-layer.x)/max(layer.y-layer.x,1e-4);
        float2 uvwXz=cloudAmbientCacheTexturePosition(q);
        float2 edgeCells=min(uvwXz,1.0-uvwXz)/shadowState.z;
        float minimumEdgeCells=min(edgeCells.x,edgeCells.y);
        if((inLowerBand||inUpperBand)&&minimumEdgeCells>CLOUD_SHADOW_CACHE_FILTER_START_CELLS){
            float borderWeight=smoothstep(CLOUD_SHADOW_CACHE_FILTER_START_CELLS,CLOUD_SHADOW_CACHE_FILTER_FULL_CELLS,minimumEdgeCells);
            float ambientCacheY=(0.5+h*float(
                CLOUD_SHADOW_CACHE_HEIGHT-1u))
                /float(CLOUD_SHADOW_CACHE_TEXTURE_HEIGHT);
            float4 cached=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,
                float3(uvwXz.x,ambientCacheY,uvwXz.y),0);
            bool finiteValue=all(cached==cached)
                           && all(cached>=0.0)
                           && all(cached<=1.001);
            if(finiteValue){
                float2 bandVisibility=saturate(
                    upperBand?cached.xy:cached.zw);
                result=float3(
                    borderWeight,bandVisibility.x,bandVisibility.y);
            }
        }
    }
    return result;
}

// 一つの端点高度で、上層と下層それぞれから空・地面へ届く光路をRGBAへ詰める。
// どちらの層でも空側と地面側の和は上下層を合わせた全光学的深さに一致する。
float4 cloudLayeredAmbientDepth(float lowerColumnDepth,float lowerGroundDepth,float lowerSegmentDepth,float upperColumnDepth,float upperGroundDepth,float upperSegmentDepth,float segmentFraction){
    segmentFraction=saturate(segmentFraction);
    float lowerSampleGroundDepth=max(lowerGroundDepth+lowerSegmentDepth*segmentFraction,0.0);
    float upperLocalGroundDepth=max(upperGroundDepth+upperSegmentDepth*segmentFraction,0.0);
    float lowerSkyDepth=max(lowerColumnDepth-lowerSampleGroundDepth+upperColumnDepth,0.0);
    float upperSkyDepth=max(upperColumnDepth-upperLocalGroundDepth,0.0);
    float upperSampleGroundDepth=upperLocalGroundDepth+lowerColumnDepth;
    return float4(upperSkyDepth,upperSampleGroundDepth,lowerSkyDepth,lowerSampleGroundDepth);
}

// 垂直光学的深さを、等方な半球放射輝度が平面へ運ぶ照度の透過率へ変換する。
// 4点Gauss-Legendre積分で 2*integral(mu*exp(-tau/mu),mu=0..1) を評価する。
float4 cloudHemisphericVisibility(float4 verticalOpticalDepth){
    const float4 directionCosines=float4(
        0.0694318442029737,0.3300094782075719,
        0.6699905217924281,0.9305681557970262);
    const float4 irradianceWeights=float4(
        0.0241522034128332,0.2152140822717850,
        0.4369310725907611,0.3237026417246206);
    float4 depth=max(verticalOpticalDepth,0.0.xxxx);
    return saturate(
        irradianceWeights.x*exp(-depth/directionCosines.x)
       +irradianceWeights.y*exp(-depth/directionCosines.y)
       +irradianceWeights.z*exp(-depth/directionCosines.z)
       +irradianceWeights.w*exp(-depth/directionCosines.w));
}

// 可視率は0..1なので、共有メモリでは各成分を16bit精度の整数へ量子化する。
// 16列の成分別総和も32bit整数へ安全に収まる。
uint4 cloudQuantizeAmbientVisibility(float4 visibility){
    return (uint4)round(saturate(visibility)*65535.0);
}

// 一つのGPUグループを一つの水平セルへ対応させ、16本の周囲光列と
// 太陽円盤4方向を同時に積分する。光学的深さは各列で透過率へ変換してから
// 平均し、疎密混在セルを暗く潰さない。
[numthreads(4,1,4)]
void CSCloudShadow(uint3 groupId : SV_GroupID,uint groupIndex : SV_GroupIndex){
    uint updateStride=max((uint)cloudShadowUpdate.z,1u);
    uint2 outputColumn=uint2(
        groupId.x*updateStride+(uint)cloudShadowUpdate.x,
        groupId.z*updateStride+(uint)cloudShadowUpdate.y);
    uint width,height,depth;
    cloudShadowOut.GetDimensions(width,height,depth);
    if(any(outputColumn>=uint2(width,depth))
       ||height!=CLOUD_SHADOW_CACHE_TEXTURE_HEIGHT)
        return;
    uint2 quadratureIndex=uint2(
        groupIndex%CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS,
        groupIndex/CLOUD_AMBIENT_CACHE_QUADRATURE_AXIS);
    float2 ambientTextureUv=(float2(outputColumn)+0.5)
        /float2(width,depth);
    float ambientSampleX,ambientWidthX;
    float ambientSampleZ,ambientWidthZ;
    cloudAmbientCacheAxisSample(
        ambientTextureUv.x,quadratureIndex.x,
        shadowGrid.x,shadowGrid.z,
        ambientSampleX,ambientWidthX);
    cloudAmbientCacheAxisSample(
        ambientTextureUv.y,quadratureIndex.y,
        shadowGrid.y,shadowGrid.w,
        ambientSampleZ,ambientWidthZ);
    float coverage=saturate(params.x);
    uint segmentBaseIndex=groupIndex*CLOUD_SHADOW_CACHE_HEIGHT;
    // 各スレッドは四等分した一つの物理面積を担当する。形状側が担当幅から
    // 解像可能な2・4・8周期を選ぶため、未解像帯域を追加点採取で追わない。
    float2 ambientFootprint=float2(ambientWidthX,ambientWidthZ);
    float2 ambientWorldXz=float2(ambientSampleX,ambientSampleZ)
        +cloudWindWorld();
    float lowerCellWorldStep=(layer.y-layer.x)
        /float(CLOUD_SHADOW_CACHE_HEIGHT);
    float upperCellWorldStep=(cloudUpperLayer.y-cloudUpperLayer.x)
        /float(CLOUD_SHADOW_CACHE_HEIGHT);
    // 空と地面から届く一次入射光なので、密度と通常の光側消散を使う。
    // 二次以降だけの縮小率を掛けると、0指定で厚い雲まで完全に明るくなる。
    float ambientExtinction=max(params.y,0.0)
        *max(cloudLightingExtinction.y,0.0);
    float lowerColumnDepth=0.0;
    [loop] for(uint densityHeightIndex=0u;
               densityHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
               ++densityHeightIndex){
        float normalizedHeight=(float(densityHeightIndex)+0.5)
            /float(CLOUD_SHADOW_CACHE_HEIGHT);
        float columnDensity=cloudAmbientQuadratureDensity(
            ambientWorldXz,ambientFootprint,
            lerp(layer.x,layer.y,normalizedHeight),
            coverage,lowerCellWorldStep);
        float segmentDepth=columnDensity*lowerCellWorldStep
            *cloudOpticalDepthScaleFromBand(false)
            *ambientExtinction;
        cloudShadowColumnSegmentDepths[
            segmentBaseIndex+densityHeightIndex]=
            float2(segmentDepth,0.0);
        lowerColumnDepth+=segmentDepth;
    }
    float upperColumnDepth=0.0;
    if(cloudUpperLayer.w>0.5){
        [loop] for(uint upperDensityHeightIndex=0u;
                   upperDensityHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
                   ++upperDensityHeightIndex){
            float upperHeight=(float(upperDensityHeightIndex)+0.5)
                /float(CLOUD_SHADOW_CACHE_HEIGHT);
            float upperDensity=cloudAmbientQuadratureDensity(
                ambientWorldXz,ambientFootprint,
                lerp(cloudUpperLayer.x,cloudUpperLayer.y,upperHeight),
                coverage,upperCellWorldStep);
            float upperSegmentDepth=upperDensity*upperCellWorldStep
                *cloudOpticalDepthScaleFromBand(true)
                *ambientExtinction;
            float2 segmentDepths=cloudShadowColumnSegmentDepths[
                segmentBaseIndex+upperDensityHeightIndex];
            segmentDepths.y=upperSegmentDepth;
            cloudShadowColumnSegmentDepths[
                segmentBaseIndex+upperDensityHeightIndex]=segmentDepths;
            upperColumnDepth+=upperSegmentDepth;
        }
    }
    float lowerGroundDepth=0.0;
    float upperGroundDepth=0.0;
    [loop] for(uint outputHeightIndex=0u;
               outputHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
               ++outputHeightIndex){
        float segmentFraction=float(outputHeightIndex)
            /max(float(CLOUD_SHADOW_CACHE_HEIGHT-1u),1.0);
        float2 segmentDepths=cloudShadowColumnSegmentDepths[
            segmentBaseIndex+outputHeightIndex];
        float4 pathDepth=cloudLayeredAmbientDepth(
            lowerColumnDepth,lowerGroundDepth,segmentDepths.x,
            upperColumnDepth,upperGroundDepth,segmentDepths.y,
            segmentFraction);
        uint profileIndex=segmentBaseIndex+outputHeightIndex;
        cloudAmbientQuantizedVisibilitySums[profileIndex]=
                cloudQuantizeAmbientVisibility(
                cloudHemisphericVisibility(pathDepth));
        lowerGroundDepth+=segmentDepths.x;
        upperGroundDepth+=segmentDepths.y;
    }
    // 先頭4スレッドは太陽円盤の各方向を担当する。方向ループを高度ループ内で
    // 展開しないため、コンパイラの式展開と一スレッドへの集中を防げる。
    if(groupIndex<CLOUD_SUN_CACHE_GROUP_THREAD_COUNT){
        float3 sun=sunDir.xyz;
        float3 finiteSunDirection=cloudSunDiskDirection(
            sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,groupIndex);
        // 太陽方向キャッシュは従来の一様48 km格子を保つ。環境光の広域写像を
        // 混ぜると、視線側の太陽キャッシュ座標と一致しなくなる。
        float2 sunColumnWorldXz=shadowGrid.xy
            +ambientTextureUv/max(shadowGrid.zw,1e-8.xx)
            +cloudWindWorld();
        [loop] for(uint sunHeightIndex=0u;
                   sunHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
                   ++sunHeightIndex){
            bool splitSunCache=cloudUpperLayer.w>0.5;
            uint samplesPerSunBand=splitSunCache
                ?CLOUD_SHADOW_CACHE_HEIGHT/2u:CLOUD_SHADOW_CACHE_HEIGHT;
            bool sunUpperBand=splitSunCache&&
                sunHeightIndex>=samplesPerSunBand;
            uint bandHeightIndex=sunUpperBand
                ?sunHeightIndex-samplesPerSunBand:sunHeightIndex;
            float sunBandHeight=float(bandHeightIndex)
                /max(float(samplesPerSunBand-1u),1.0);
            float sunAltitude=sunUpperBand
                ?lerp(cloudUpperLayer.x,cloudUpperLayer.y,sunBandHeight)
                :lerp(layer.x,layer.y,sunBandHeight);
            float3 sunP=cloudShadowWorldPositionAtAltitude(
                sunColumnWorldXz,sunAltitude);
            float firstExtinction=max(
                params.y*cloudLightingExtinction.y,0.0);
            float secondExtinction=firstExtinction
                *saturate(cloudLightingMulti.x);
            float thirdExtinction=secondExtinction
                *saturate(cloudLightingMulti.x);
            float3 sunDepths=traceCloudShadowDepths(
                sunP,coverage,finiteSunDirection,
                float3(firstExtinction,secondExtinction,thirdExtinction));
            // 現在のRHIはt0..t3の連続配置を要求する。実行時に成立しない負の密度分岐へ
            // 置くことで、t2/s2を宣言へ残しつつ通常時の採取を発生させない。
            if(params.y<0.0){
                float registerRetention=detailNoise.SampleLevel(
                    detailNoise_sampler,float3(0.5,0.5,0.5),0).x;
                sunDepths+=registerRetention.xxx;
            }
            // 深さの補間は行わない。各円盤光路を次数別の透過率へ変換してから
            // キャッシュへ保存し、空隙と濃い雲が混在するセルの太陽光を保つ。
            cloudSunVisibilityProfiles[
                groupIndex*CLOUD_SHADOW_CACHE_HEIGHT+sunHeightIndex]=
                exp(-max(sunDepths,0.0.xxx));
        }
    }
    // 全プロファイルを書き終えた後に一度だけ同期する。高度ごとの同期は
    // 初期コンパイルとGPU待機の双方を悪化させる。
    GroupMemoryBarrierWithGroupSync();
    if(groupIndex==0u){
        [loop] for(uint outputHeightIndex=0u;
                   outputHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
                   ++outputHeightIndex){
            uint4 resolvedVisibilitySum=uint4(0u,0u,0u,0u);
            [loop] for(uint sampleIndex=0u;
                       sampleIndex<CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT;
                       ++sampleIndex){
                uint profileIndex=
                    sampleIndex*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex;
                resolvedVisibilitySum+=
                    cloudAmbientQuantizedVisibilitySums[profileIndex];
            }
            cloudShadowOut[uint3(
                outputColumn.x,outputHeightIndex,outputColumn.y)]
                =saturate(float4(resolvedVisibilitySum)
                    /(65535.0
                      *float(CLOUD_SHADOW_CACHE_GROUP_THREAD_COUNT)));
            float4 firstVisibility=float4(
                cloudSunVisibilityProfiles[outputHeightIndex].x,
                cloudSunVisibilityProfiles[
                    CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].x,
                cloudSunVisibilityProfiles[
                    2u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].x,
                cloudSunVisibilityProfiles[
                    3u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].x);
            float4 secondVisibility=float4(
                cloudSunVisibilityProfiles[outputHeightIndex].y,
                cloudSunVisibilityProfiles[
                    CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].y,
                cloudSunVisibilityProfiles[
                    2u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].y,
                cloudSunVisibilityProfiles[
                    3u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].y);
            float4 thirdVisibility=float4(
                cloudSunVisibilityProfiles[outputHeightIndex].z,
                cloudSunVisibilityProfiles[
                    CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].z,
                cloudSunVisibilityProfiles[
                    2u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].z,
                cloudSunVisibilityProfiles[
                    3u*CLOUD_SHADOW_CACHE_HEIGHT+outputHeightIndex].z);
            cloudShadowOut[uint3(
                outputColumn.x,
                outputHeightIndex+CLOUD_SHADOW_CACHE_HEIGHT,
                outputColumn.y)]=saturate(firstVisibility);
            cloudShadowOut[uint3(
                outputColumn.x,
                outputHeightIndex+2u*CLOUD_SHADOW_CACHE_HEIGHT,
                outputColumn.y)]=saturate(secondVisibility);
            cloudShadowOut[uint3(
                outputColumn.x,
                outputHeightIndex+3u*CLOUD_SHADOW_CACHE_HEIGHT,
                outputColumn.y)]=saturate(thirdVisibility);
        }
    }
}

[numthreads(8,8,1)]
void CSCloudWorldShadow(uint3 tid : SV_DispatchThreadID){
    uint updateStride=max((uint)cloudWorldShadowUpdate.z,1u);
    uint2 outputPixel=tid.xy*updateStride+
        (uint2)cloudWorldShadowUpdate.xy;
    uint width,height;
    cloudOut.GetDimensions(width,height);
    if(any(outputPixel>=uint2(width,height))) return;
    float2 uv=(float2(outputPixel)+0.5)/float2(width,height);
    float2 referenceXz=cloudWorldShadowMap.xy
        +uv/max(cloudWorldShadowMap.z,1e-8);
    float3 rayOrigin=float3(referenceXz.x,cloudWorldShadowMap.w,referenceXz.y);
    float transmittance=1.0;
    // 未解像密度を区間透過率へ変換した後の有効光学的深さ。
    float opticalDepth=0.0;
    float3 sun=sunDir.xyz;
    CloudPackedBandIntervals bandIntervals=
        intersectCloudBandsFromPosition(rayOrigin,sun);
    if(sun.y>0.03&&bandIntervals.count>0){
        float extinction=max(
            params.y*cloudLightingExtinction.y,0.0);
        const int SAMPLE_COUNT=32;
        CloudFourStateTransportLanes firstOrderState=
            cloudInitialFourStateTransportLanes();
        CloudFourStateTransportLanes secondOrderState=
            cloudInitialFourStateTransportLanes();
        CloudFourStateTransportLanes thirdOrderState=
            cloudInitialFourStateTransportLanes();
        float previousSegmentEnd=-1.0;
        int previousBandId=-1;
        [loop] for(int sampleIndex=0;sampleIndex<SAMPLE_COUNT;++sampleIndex){
            float sampleDistance=0.0;
            float stepLength=0.0;
            int sampleBandId=-1;
            if(!cloudLightSampleTerms(
                   bandIntervals,SAMPLE_COUNT,sampleIndex,
                   sampleDistance,stepLength,sampleBandId)) continue;
            float segmentStart=sampleDistance-0.5*stepLength;
            if(previousSegmentEnd>=0.0&&
               (sampleBandId!=previousBandId||
                segmentStart>previousSegmentEnd+1e-3)){
                firstOrderState.active=0.0.xxxx;
                secondOrderState.active=0.0.xxxx;
                thirdOrderState.active=0.0.xxxx;
            }
            previousSegmentEnd=sampleDistance+0.5*stepLength;
            previousBandId=sampleBandId;
            float3 p=rayOrigin+sun*sampleDistance;
            CloudMacroSample macro=sampleCloudMacroLightingSegment(
                p,saturate(params.x),sun,stepLength);
            float sampleDepth=cloudLowLodOpticalDepthByOrderFromMacro(
                p,macro,macro.densityWeatherMask,extinction.xxx,
                sun,stepLength,
                firstOrderState,secondOrderState,thirdOrderState).x;
            opticalDepth+=sampleDepth;
            if(opticalDepth>=12.0) break;
        }
        // 登録番号をt0..t3で連続させるため、通常は到達しない分岐でも詳細雑音を宣言へ残す。
        if(params.y<0.0){
            opticalDepth+=detailNoise.SampleLevel(detailNoise_sampler,float3(0.5,0.5,0.5),0).x;
        }
        transmittance=exp(-max(opticalDepth,0.0));
    }
    // 現在フレームの立体物用雲影情報。固定地図の各位相が最後に採取した
    // 移流距離をCPU側で追跡するため、更新画素には現在値をそのまま保存する。
    float4 worldShadowValue=float4(
        saturate(transmittance),max(opticalDepth,0.0),0.0,1.0);
    cloudOut[outputPixel]=worldShadowValue;
}

uint CloudTemporalBlockPhase4(uint2 blockQ,uint phaseIndex) {
    // A stable block hash rotates the sixteen-phase schedule independently for
    // each 4x4 output block. Every block still visits the complete Bayer set,
    // but history ages no longer form one screen-wide repeating 4x4 lattice.
    uint blockHash=blockQ.x*0x8da6b343u^blockQ.y*0xd8163841u;
    blockHash^=blockHash>>16u;
    blockHash*=0x7feb352du;
    blockHash^=blockHash>>15u;
    return (phaseIndex+(blockHash&15u))&15u;
}
uint2 CloudTemporalPhaseOffset4(uint2 blockQ,uint phaseIndex) {
    // 4x4 Bayer-progressive order: every prefix is spread across the block,
    // avoiding the horizontal/vertical cold-start bands of raster ordering.
    const uint2 offsets[16]={
        uint2(0,0),uint2(2,2),uint2(2,0),uint2(0,2),
        uint2(1,1),uint2(3,3),uint2(3,1),uint2(1,3),
        uint2(1,0),uint2(3,2),uint2(3,0),uint2(1,2),
        uint2(0,1),uint2(2,3),uint2(2,1),uint2(0,3)};
    return offsets[CloudTemporalBlockPhase4(blockQ,phaseIndex)];
}

float3 CloudViewDirection(float2 ndc) {
    // far/near比がfloat精度を超えて遠点のwが0になっても、xyzから視線を復元する。
    float4 farHomogeneous=mul(float4(ndc,1.0,1.0),invViewProj);
    bool perspective=abs(invViewProj[2][3])>1.0e-7;
    float3 candidate=perspective
        ?farHomogeneous.xyz
        :mul(float4(0.0,0.0,1.0,0.0),invViewProj).xyz;
    float lengthSquared=dot(candidate,candidate);
    return lengthSquared>1.0e-12&&lengthSquared<3.0e38
        ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}

// 一つの実視線上の各求積点で共有する照明量。
// phaseとphaseMultiは、太陽円盤の中心方向に対して解決した値を保持する。
struct CloudLightingContext {
    float3 sun;
    float3 sunAtCloud;
    float phase;
    float phaseMulti;
    float coverage;
    float density;
    float multiOcclusion;
    float thirdOcclusion;
    float lightExtinction;
    float directionalScatteringScale;
};

// 視線と太陽円盤上の一方向から、一次・高次の位相関数を求める。
// 円盤上の各光路へ個別に適用し、透過率の平均後に中心方向の位相を掛ける近似を避ける。
void cloudScatteringPhasesForDirections(
    float3 viewDirection,float3 sunDirection,
    out float phase,out float phaseMulti){
    float cosA=clamp(dot(viewDirection,sunDirection),-1.0,1.0);
    float forwardPhase=hg(cosA,cloudLightingPhase.x);
    float backwardPhase=hg(cosA,cloudLightingPhase.y);
    phase=clamp(
        lerp(backwardPhase,forwardPhase,saturate(cloudLightingPhase.z)),
        cloudLightingMulti.y,cloudLightingMulti.z);
    // 高次ほど方向を失うため、一次散乱とは別の等方寄り位相を使う。
    phaseMulti=clamp(
        hg(cosA,cloudMultiPhase.x),
        cloudLightingMulti.y,cloudLightingMulti.z);
}

// 実視線ごとの太陽との散乱角から、一次と高次の位相関数を解決する。
// rayDirectionはカメラから雲へ向かう単位ベクトルを受け取る。
CloudLightingContext cloudLightingContextForRayDirection(
    CloudLightingContext context,float3 rayDirection){
    cloudScatteringPhasesForDirections(
        rayDirection,context.sun,context.phase,context.phaseMulti);
    return context;
}

// 一つの求積点へ到達する一次・二次・三次の局所散乱源。
struct CloudLightingSource {
    float3 firstOrder;
    float3 secondOrder;
    float3 thirdOrder;
    // 現在の実空間サブレイの局所媒質量から得た高次散乱の内部供給率。
    float higherOrderInScatterFactor;
};

// 一つの実サブレイ上のGauss点で、密度と照明を同じ物理位置から得る。
// レーンを順次処理し、4レーン分の重い太陽光路を同時に保持しない。
struct CloudPhysicalLaneDensityLightingSample {
    // 同一点の四つの未解像密度状態。
    float4 densityStates;
    // 副光線の向きと雲帯高度せん断を含む、実ノイズ由来の物理相関距離。
    float correlationLength;
    // 現在の実交差区間を0～1へ正規化したGauss-Legendre採取位置。
    float physicalFraction;
    float sourceValidity;
    float3 firstOrderSource;
    float3 secondOrderSource;
    float3 thirdOrderSource;
};

// 隣接Gauss点の散乱源を指定位置へ線形補間する。外側ではセル端で負にならない
// 傾きへ色成分ごとに制限し、線形照明の吸収重心を保つ。片側の密度が0なら、
// 空の採取点を外挿へ使わず有効側の値を区間内へ保つ。
float cloudLinearLightingSourceComponentAtFraction(
    float normalizedDistance,float leftFraction,float rightFraction,
    float leftSource,float rightSource,
    float leftValid,float rightValid,float sourceUpperBound){
    bool leftComponentValid=leftValid>0.5
        &&cloudValueIsFinite(leftSource);
    bool rightComponentValid=rightValid>0.5
        &&cloudValueIsFinite(rightSource);
    float source=0.0;
    if(leftComponentValid&&rightComponentValid
       &&cloudValueIsFinite(normalizedDistance)){
        float sourceSpan=max(rightFraction-leftFraction,1e-6);
        float sourceSlope=(rightSource-leftSource)/sourceSpan;
        float safeSourceUpper=cloudValueIsFinite(sourceUpperBound)
            ?max(sourceUpperBound,0.0):0.0;
        float sourceUpper=max(
            max(leftSource,rightSource),safeSourceUpper);
        if(normalizedDistance<leftFraction){
            float maximumPositiveSlope=max(leftSource,0.0)
                /max(leftFraction,1e-6);
            float minimumNegativeSlope=(leftSource-sourceUpper)
                /max(leftFraction,1e-6);
            sourceSlope=clamp(
                sourceSlope,minimumNegativeSlope,maximumPositiveSlope);
        }
        else if(normalizedDistance>rightFraction){
            float minimumNegativeSlope=-max(rightSource,0.0)
                /max(1.0-rightFraction,1e-6);
            float maximumPositiveSlope=(sourceUpper-rightSource)
                /max(1.0-rightFraction,1e-6);
            sourceSlope=clamp(
                sourceSlope,minimumNegativeSlope,maximumPositiveSlope);
        }
        source=leftSource+sourceSlope*(normalizedDistance-leftFraction);
    }
    else if(leftComponentValid)
        source=leftSource;
    else if(rightComponentValid)
        source=rightSource;
    return cloudValueIsFinite(source)?max(source,0.0):0.0;
}
float3 cloudLinearLightingSourceAtFraction(
    float normalizedDistance,float leftFraction,float rightFraction,
    float3 leftSource,float3 rightSource,
    float leftValid,float rightValid,float3 sourceUpperBound){
    return float3(
        cloudLinearLightingSourceComponentAtFraction(
            normalizedDistance,leftFraction,rightFraction,
            leftSource.x,rightSource.x,leftValid,rightValid,
            sourceUpperBound.x),
        cloudLinearLightingSourceComponentAtFraction(
            normalizedDistance,leftFraction,rightFraction,
            leftSource.y,rightSource.y,leftValid,rightValid,
            sourceUpperBound.y),
        cloudLinearLightingSourceComponentAtFraction(
            normalizedDistance,leftFraction,rightFraction,
            leftSource.z,rightSource.z,leftValid,rightValid,
            sourceUpperBound.z));
}

// 密度を採取した同じ位置で太陽光・環境光・地面反射を評価する。
// 消散と照明を別の代表点へ分けず、呼び出し側が前後順のBeer-Lambert重みを掛ける。
CloudLightingSource cloudLightingSourceAtPoint(
    float3 p,CloudMacroSample macro,CloudLightingContext context,
    float lowLodDensity,float3 viewRayDirection){
    CloudLightingSource source;
    source.firstOrder=0.0.xxx;
    source.secondOrder=0.0.xxx;
    source.thirdOrder=0.0.xxx;
    source.higherOrderInScatterFactor=1.0;
    float cacheBlendWeight=0.0;
    float4 cachedFirstVisibility=1.0.xxxx;
    float4 cachedSecondVisibility=1.0.xxxx;
    float4 cachedThirdVisibility=1.0.xxxx;
    // 参照描画は自己影キャッシュを介さず、各求積点から太陽面へ完成密度を積分する。
    if(CLOUD_MAIN_SHADOW_CACHE_ENABLED&&cloudLightingAmbient.w<0.5){
        sampleCloudSunTransmittance(
            p,cacheBlendWeight,
            cachedFirstVisibility,cachedSecondVisibility,
            cachedThirdVisibility);
    }
    float3 cacheExtinctionByOrder=float3(
        context.lightExtinction,
        context.lightExtinction*context.multiOcclusion,
        context.lightExtinction*context.thirdOcclusion);
    float4 firstDetailOpticalDepthResiduals=0.0.xxxx;
    float4 secondDetailOpticalDepthResiduals=0.0.xxxx;
    float4 thirdDetailOpticalDepthResiduals=0.0.xxxx;
    if(cacheBlendWeight>0.0){
        // 太陽円盤の各方向は異なる雲柱を通る。中心方向の残差を共有せず、
        // キャッシュに保存した4光路と同じ方向ごとに近距離差分を積分する。
        [loop] for(uint residualDirectionIndex=0u;
                   residualDirectionIndex<CLOUD_SUN_DISK_DIRECTION_COUNT;
                   ++residualDirectionIndex){
            float3 residualSunDirection=cloudSunDiskDirection(
                context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,
                residualDirectionIndex);
            float3 directionalResiduals=cloudNearLightOpticalDepthResiduals(
                p,context.coverage,residualSunDirection,
                cacheExtinctionByOrder);
            // 動的なベクトル書き込みはFXCに重い光路全体の展開を強制する。
            // 一方向だけ1となる選択子で加算し、通常ループのまま4成分を埋める。
            float4 residualDirectionSelector=float4(
                residualDirectionIndex==0u?1.0:0.0,
                residualDirectionIndex==1u?1.0:0.0,
                residualDirectionIndex==2u?1.0:0.0,
                residualDirectionIndex==3u?1.0:0.0);
            firstDetailOpticalDepthResiduals+=
                residualDirectionSelector*directionalResiduals.x;
            secondDetailOpticalDepthResiduals+=
                residualDirectionSelector*directionalResiduals.y;
            thirdDetailOpticalDepthResiduals+=
                residualDirectionSelector*directionalResiduals.z;
        }
    }
    float cacheReliability=cloudSunDepthResidualCacheReliability(
        cachedFirstVisibility,cachedSecondVisibility,
        cachedThirdVisibility,
        firstDetailOpticalDepthResiduals,
        secondDetailOpticalDepthResiduals,
        thirdDetailOpticalDepthResiduals);
    cacheBlendWeight*=cacheReliability;
    // FXCは動的なベクトル添字を含む固定回数ループを、[loop]指定と同時に
    // 解決できないことがある。4方向は太陽円盤積分そのものなので、添字を
    // 実行時に持たず、各光路を明示してコンパイラと計算順を安定させる。
    float firstPhase0=0.0;
    float firstPhase1=0.0;
    float firstPhase2=0.0;
    float firstPhase3=0.0;
    float higherPhase0=0.0;
    float higherPhase1=0.0;
    float higherPhase2=0.0;
    float higherPhase3=0.0;
    cloudScatteringPhasesForDirections(
        viewRayDirection,
        cloudSunDiskDirection(
            context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,0u),
        firstPhase0,higherPhase0);
    cloudScatteringPhasesForDirections(
        viewRayDirection,
        cloudSunDiskDirection(
            context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,1u),
        firstPhase1,higherPhase1);
    cloudScatteringPhasesForDirections(
        viewRayDirection,
        cloudSunDiskDirection(
            context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,2u),
        firstPhase2,higherPhase2);
    cloudScatteringPhasesForDirections(
        viewRayDirection,
        cloudSunDiskDirection(
            context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,3u),
        firstPhase3,higherPhase3);
    float4 firstDiskPhase=float4(
        firstPhase0,firstPhase1,firstPhase2,firstPhase3);
    float4 higherDiskPhase=float4(
        higherPhase0,higherPhase1,higherPhase2,higherPhase3);
    float3 exactAverageScattering=1.0.xxx;
    if(cacheBlendWeight<1.0){
        float3 exactExtinctionByOrder=float3(
            context.lightExtinction,
            context.lightExtinction*context.multiOcclusion,
            context.lightExtinction*context.thirdOcclusion);
        float3 exactScatteringSum=0.0.xxx;
        [loop] for(uint sunDirectionIndex=0u;
                   sunDirectionIndex<CLOUD_SUN_DISK_DIRECTION_COUNT;
                   ++sunDirectionIndex){
            float3 finiteSunDirection=cloudSunDiskDirection(
                context.sun,cloudLightTangent.xyz,cloudLightBitangent.xyz,
                sunDirectionIndex);
            float directionalPhase=sunDirectionIndex==0u
                ?firstPhase0
                :(sunDirectionIndex==1u
                    ?firstPhase1
                    :(sunDirectionIndex==2u?firstPhase2:firstPhase3));
            float directionalPhaseMulti=sunDirectionIndex==0u
                ?higherPhase0
                :(sunDirectionIndex==1u
                    ?higherPhase1
                    :(sunDirectionIndex==2u?higherPhase2:higherPhase3));
            float3 exactDepths=traceCloudMainLightDepths(
                p,context.coverage,finiteSunDirection,
                exactExtinctionByOrder);
            float3 exactVisibility=exp(-max(exactDepths,0.0.xxx));
            exactScatteringSum+=exactVisibility*float3(
                directionalPhase,directionalPhaseMulti,directionalPhaseMulti);
        }
        exactAverageScattering=exactScatteringSum
            /float(CLOUD_SUN_DISK_DIRECTION_COUNT);
    }
    float4 correctedCachedFirst=cloudApplySunOpticalDepthResidual(
        cachedFirstVisibility,firstDetailOpticalDepthResiduals);
    float4 correctedCachedSecond=cloudApplySunOpticalDepthResidual(
        cachedSecondVisibility,secondDetailOpticalDepthResiduals);
    float4 correctedCachedThird=cloudApplySunOpticalDepthResidual(
        cachedThirdVisibility,thirdDetailOpticalDepthResiduals);
    // 各光路の透過率と位相を先に掛けてから平均する。透過率を平均して中心方向の
    // 位相を掛けると、太陽円盤の端で生じる散乱角の違いを失い、縁の明るさが不正確になる。
    float3 correctedCachedAverageScattering=float3(
        dot(correctedCachedFirst,firstDiskPhase),
        dot(correctedCachedSecond,higherDiskPhase),
        dot(correctedCachedThird,higherDiskPhase))
        /float(CLOUD_SUN_DISK_DIRECTION_COUNT);
    float3 lightScatteringByOrder=lerp(
        exactAverageScattering,correctedCachedAverageScattering,
        cacheBlendWeight);
    float firstLightScattering=lightScatteringByOrder.x;
    float secondLightScattering=lightScatteringByOrder.y;
    float thirdLightScattering=lightScatteringByOrder.z;
    float inScatterDepthExponent=lerp(
        0.5,2.0,saturate((macro.height-0.30)/0.55));
    // ここでは一つの実サブレイだけを評価するため、旧4レーン値を複製せず
    // 同じ平均密度から一つの内部供給率を求める。
    float safeLowLodDensity=cloudValueIsFinite(lowLodDensity)
        ?max(lowLodDensity,0.0):0.0;
    float inScatterDepth=saturate(
        0.05+pow(saturate(safeLowLodDensity),inScatterDepthExponent));
    source.higherOrderInScatterFactor=lerp(
        1.0,inScatterDepth,
        cloudCondensationFiniteSaturate(cloudLightingExtinction.w));
    source.firstOrder=context.sunAtCloud
        *context.directionalScatteringScale
        *firstLightScattering;
    source.secondOrder=context.sunAtCloud
        *context.directionalScatteringScale
        *secondLightScattering;
    source.thirdOrder=context.sunAtCloud
        *context.directionalScatteringScale
        *thirdLightScattering;

    // 距離終端の表示フェードは視線消散だけへ適用する。環境光の物理媒質量へ
    // 混ぜると、同じ位置でもキャッシュ内外で雲内部の明るさが変わる。
    float ambientExtinction=max(cloudLightingExtinction.y,0.0);
    float2 fallbackAmbientDepth=
        cloudAmbientFallbackOpticalDepth(
            macro,lowLodDensity.xxxx,context.density,ambientExtinction);
    float4 fallbackAmbientVisibility=cloudHemisphericVisibility(
        float4(fallbackAmbientDepth,0.0,0.0));
    float3 cachedAmbientVisibility=sampleCloudAmbientVisibility(p);
    float skyAmbientVisibility=lerp(
        fallbackAmbientVisibility.x,
        cachedAmbientVisibility.y,
        cachedAmbientVisibility.x);
    float groundAmbientVisibility=lerp(
        fallbackAmbientVisibility.y,
        cachedAmbientVisibility.z,
        cachedAmbientVisibility.x);
    float height=macro.height;
    float skyAmbientZenithWeight=lerp(
        0.3333333,0.6666667,saturate(height));
    float3 skyAmbient=cloudSkyZenith.w>0.5
        ?lerp(skyCol.rgb,cloudSkyZenith.rgb,skyAmbientZenithWeight)
        :skyCol.rgb;
    float3 ambientLight=skyAmbient
        *lerp(cloudLightingAmbient.x,cloudLightingAmbient.y,height)
        *skyAmbientVisibility*cloudLightingExtinction.z;
    float bottomWeight=1.0-smoothstep(0.15,0.65,height);
    float3 groundLight=cloudLightingGround.rgb*cloudLightingMulti.w
        *bottomWeight*groundAmbientVisibility
        *cloudLightingExtinction.z;
    source.firstOrder+=ambientLight+groundLight;
    return source;
}

CloudPhysicalLaneDensityLightingSample
emptyCloudPhysicalLaneDensityLightingSample(){
    CloudPhysicalLaneDensityLightingSample sample;
    sample.densityStates=0.0.xxxx;
    sample.correlationLength=0.0;
    sample.physicalFraction=0.5;
    sample.sourceValidity=0.0;
    sample.firstOrderSource=0.0.xxx;
    sample.secondOrderSource=0.0.xxx;
    sample.thirdOrderSource=0.0.xxx;
    return sample;
}

// 一つの実サブレイ上で、密度と局所照明を同じGauss位置から採取する。
// 2×2面積求積の各レーンを独立させ、中心視線の影を全レーンへ流用しない。
CloudPhysicalLaneDensityLightingSample
sampleCloudPhysicalLaneDensityLightingAtFraction(
    float sampleFraction,float componentStartT,float componentEndT,
    float3 rayDirection,
    float4 coverageTerms,float3 densityLongitudinalFootprint,
    float3 pixelDirectionSpan,float angularPixelFootprint,
    int physicalBandId,
    float fadeStart,float maximumDistance,float densityScale,
    CloudLightingContext lightingContext){
    CloudPhysicalLaneDensityLightingSample sample=
        emptyCloudPhysicalLaneDensityLightingSample();
    // 球殻とセルの実交差区間へGauss-Legendre点を再写像する。
    // 重み区間の中点へ置き換えず、7次までの求積精度を維持する。
    sample.physicalFraction=sampleFraction;
    float currentSampleT=cloudIntervalDistanceAtFraction(
        componentStartT,componentEndT,sampleFraction);
    // 投影画素幅と横断面は区間重心ではなく、このGauss点の実距離で求める。
    // 近側を過剰にぼかし、遠側へ解像不能な詳細を残す非対称を持ち込まない。
    float physicalProjectedPixelWidth=cloudProjectedPixelWidth(
        currentSampleT,angularPixelFootprint);
    float physicalDetailSampleSpacing=cloudDetailSampleSpacing(
        length(densityLongitudinalFootprint),
        physicalProjectedPixelWidth);
    float3 densityTransverseFootprint=
        pixelDirectionSpan*currentSampleT;
    float billowVisibility=cloudBillowVisibilityFromSampleSpacing(
        physicalDetailSampleSpacing);
    float middleBillowVisibility=
        cloudMiddleBillowVisibilityFromSampleSpacing(
            physicalDetailSampleSpacing);
    float erosionVisibility=cloudErosionVisibilityFromSampleSpacing(
        physicalDetailSampleSpacing);
    float3 currentP=camPos.xyz+rayDirection*currentSampleT;
    CloudMacroSample currentMacro=sampleCloudMacro(
        currentP,coverageTerms,
        densityLongitudinalFootprint,0.5*densityTransverseFootprint);
    float currentDistanceFade=cloudDistanceFade(
        currentSampleT,fadeStart,maximumDistance);
    float4 detailedDistribution=cloudDensityDistributionFromMacro(
        currentP,currentMacro,currentMacro.densityWeatherMask,
        billowVisibility,middleBillowVisibility,erosionVisibility);
    detailedDistribution*=cloudOpticalDepthScaleFromBand(
        currentMacro.upperBand>0.5);
    float4 lowLodDistribution=
        cloudLowLodDensityDistributionFromMacro(
            currentMacro,currentMacro.densityWeatherMask);
    float opticalDensityScale=densityScale*currentDistanceFade;
    detailedDistribution=cloudScaleDensityDistribution(
        detailedDistribution,opticalDensityScale);
    float meanDensity=cloudDensityDistributionMean(detailedDistribution);
    float lowLodDensity=cloudDensityDistributionMean(lowLodDistribution);
    sample.densityStates=detailedDistribution;
    sample.correlationLength=
        cloudUnresolvedDensityCorrelationLengthAtDirection(
            currentP,rayDirection,physicalBandId>0);
    if(meanDensity>0.0){
        CloudLightingSource lightingSource=cloudLightingSourceAtPoint(
            currentP,currentMacro,lightingContext,lowLodDensity,
            rayDirection);
        sample.sourceValidity=1.0;
        sample.firstOrderSource=lightingSource.firstOrder;
        // 高次の内部供給率は輸送重みではなく局所散乱源の一部である。
        // 端点の完全な照明場へ含めてから、Beer吸収重心へ線形補間する。
        sample.secondOrderSource=lightingSource.secondOrder
            *lightingSource.higherOrderInScatterFactor;
        sample.thirdOrderSource=lightingSource.thirdOrder
            *lightingSource.higherOrderInScatterFactor;
    }
    return sample;
}

[numthreads(8,8,1)]
void CSCloud(uint3 tid : SV_DispatchThreadID){
    uint W=(uint)dims.x, H=(uint)dims.y;
    uint2 pixelQ=tid.xy;
    if(pixelQ.x>=W || pixelQ.y>=H) return;
    // Ultra keeps the quarter-dimension workload spatially complete, but each
    // low texel traces one exact full-resolution subpixel. Across sixteen
    // phases every 4x4 output block receives a native ray without ever reading
    // an unwritten/stale low texel. Non-Ultra policies retain their ordinary
    // reduced-resolution sample centres.
    uint2 rayPixel=pixelQ;
    float2 rayDimensions=dims.xy;
    if(temporal.w>3.5) {
        uint scheduledPhase=(uint)temporal.z&15u;
        uint2 phaseOffset=CloudTemporalPhaseOffset4(
            pixelQ,scheduledPhase);
        rayPixel=min(pixelQ*4u+phaseOffset,uint2(dims.zw)-1u);
        rayDimensions=dims.zw;
    }
    float2 uv=(float2(rayPixel)+0.5)/rayDimensions;
    float4 clip=float4(uv.x*2-1, -(uv.y*2-1), 1, 1);
    float3 dir=CloudViewDirection(clip.xy);
    // 現在画素内の2×2 Gauss位置を実投影で復元する。中心視線の雲殻区間を
    // 流用せず、接線と雲中出口を各サブレイの物理光路として保持する。
    float2 pixelNdcStepX=float2(2.0/rayDimensions.x,0.0);
    float2 pixelNdcStepY=float2(0.0,-2.0/rayDimensions.y);
    CloudPhysicalSubrayDirections subrayDirections;
    subrayDirections.lane0=CloudViewDirection(
        clip.xy-CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepX
               -CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepY);
    subrayDirections.lane1=CloudViewDirection(
        clip.xy+CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepX
               -CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepY);
    subrayDirections.lane2=CloudViewDirection(
        clip.xy-CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepX
               +CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepY);
    subrayDirections.lane3=CloudViewDirection(
        clip.xy+CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepX
               +CLOUD_SUBRAY_GAUSS_OFFSET*pixelNdcStepY);
    float3 localUp=groundHorizon.xyz;
    float signedElevation=dot(dir,localUp);
    bool cameraBelowCloudBase=cloudCameraBelowCloudBase();
    // The planet/ground occlusion boundary has no rasterizer coverage because
    // clouds are traced in compute. A hard angular compare therefore exposes
    // one quarter-resolution occupancy decision as a white, stair-stepped row
    // against the lower sky hemisphere. Integrate the projected full-resolution
    // pixel footprint around the same physical cutoff; this is analytic edge
    // coverage, not a post blur, and remains stable across TSR phases.
    float groundHorizonCoverage=1.0;
    float groundCutoff=groundHorizon.w;
    if(groundCutoff>=-1.0&&cameraBelowCloudBase) {
        // The camera-local up vector and physical ground tangent are invariant
        // for the complete frame. CPU-side evaluation mirrors cloudAltitude
        // exactly and avoids repeating its divisions/normalization/sqrt in
        // every trace invocation.
        if(signedElevation<groundCutoff-0.02) {
            groundHorizonCoverage=0.0;
        } else if(signedElevation<groundCutoff+0.02) {
            // Reconstruct both screen derivatives from exact neighbouring
            // full-resolution rays.  A centered box footprint gives partial
            // coverage on both sides of the implicit planet tangent; the old
            // one-sided ramp quantized every sloped horizon crossing to the
            // first skyward pixel and exposed a staircase.
            float2 pixelCenter=float2(rayPixel)+0.5;
            float xOffset=rayPixel.x+1u<(uint)rayDimensions.x?1.0:-1.0;
            float yOffset=rayPixel.y+1u<(uint)rayDimensions.y?1.0:-1.0;
            float2 xUv=(pixelCenter+float2(xOffset,0.0))/rayDimensions;
            float2 yUv=(pixelCenter+float2(0.0,yOffset))/rayDimensions;
            float4 xClip=float4(xUv.x*2-1,-(xUv.y*2-1),1,1);
            float4 yClip=float4(yUv.x*2-1,-(yUv.y*2-1),1,1);
            float xElevation=dot(CloudViewDirection(xClip.xy),localUp);
            float yElevation=dot(CloudViewDirection(yClip.xy),localUp);
            float coverageHalfWidth=max(
                0.5*(abs(xElevation-signedElevation)+
                     abs(yElevation-signedElevation)),1e-6);
            groundHorizonCoverage=smoothstep(
                groundCutoff-coverageHalfWidth,
                groundCutoff+coverageHalfWidth,
                signedElevation);
        }
    }
    if(groundHorizonCoverage<=0.001&&cameraBelowCloudBase){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    float coverage=saturate(params.x), density=max(params.y,0.05);
    // カメラが雲層内または境界近傍にあるときは、CPUで連続補間した局所視程を使う。
    float MAX_DISTANCE=min(cloudRange.x,max(cloudRange.w,1.0));
    CloudSubrayBandIntervals subrayIntervals;
    CloudPackedBandIntervals packedBandIntervals=
        intersectCloudSubrayBandUnion(
            subrayDirections,MAX_DISTANCE,subrayIntervals);
    if(packedBandIntervals.count<=0){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // どこまで追うか。遠い雲は 1 画素に何 km も入るので積分が成立せず、描くほど
    // «ちらつく細かいゴミ» になる。打ち切りの手前で薄くして、境界の «壁» を出さない。
    // 境界から十分離れた地上と上空では w==x のため従来の遠景距離を保つ。
    float fadeStartRatio=saturate(cloudRange.y/max(cloudRange.x,1.0));
    float fadeStart=MAX_DISTANCE*fadeStartRatio;
    int packedIntervalIndex=0;
    float intervalStart=packedBandIntervals.starts.x;
    float intervalEnd=packedBandIntervals.ends.x;
    int physicalBandId=packedBandIntervals.bandIds.x;
    // 曲面雲層には有限な地平線区間がある。距離減衰はこの入口だけでレイ全体を
    // 棄却せず、後続の各密度標本へ適用する。
    if(intervalEnd<=intervalStart){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // 現在画素の四隅から担当幅を復元する。片側隣接差を外挿しないため、
    // 画面端と広角投影でも包絡幅が同じ画素内に収まる。
    float3 cornerDirection0=CloudViewDirection(
        clip.xy-0.5*pixelNdcStepX-0.5*pixelNdcStepY);
    float3 cornerDirection1=CloudViewDirection(
        clip.xy+0.5*pixelNdcStepX-0.5*pixelNdcStepY);
    float3 cornerDirection2=CloudViewDirection(
        clip.xy-0.5*pixelNdcStepX+0.5*pixelNdcStepY);
    float3 cornerDirection3=CloudViewDirection(
        clip.xy+0.5*pixelNdcStepX+0.5*pixelNdcStepY);
    float3 minimumPixelDirection=min(
        min(cornerDirection0,cornerDirection1),
        min(cornerDirection2,cornerDirection3));
    float3 maximumPixelDirection=max(
        max(cornerDirection0,cornerDirection1),
        max(cornerDirection2,cornerDirection3));
    float3 pixelDirectionSpan=max(
        maximumPixelDirection-minimumPixelDirection,0.0.xxx);
    float angularPixelFootprint=max(
        length(cornerDirection1-cornerDirection0),
        length(cornerDirection2-cornerDirection0));
    // 標本位置は世界距離で進める。区間を固定個数で割る方式では全画素が同じ
    // 高さ割合を読むため、視点を中心とした放射状の筋が生じる。
    // 刻み数。参照描画では大きくする (cloudLightingAmbient.z に入っている)。
    int MAX_STEPS=(int)cloudLightingAmbient.z;
    if(MAX_STEPS<CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT)
        MAX_STEPS=CLOUD_MIN_VIEW_MARCH_SAMPLE_COUNT;
    float baseFineStep=cloudCoverageReciprocals.z;
    int2 physicalBandBudgets=cloudPhysicalBandSampleBudgets(MAX_STEPS);
    int4 packedIntervalBudgets=cloudPackedIntervalSampleBudgets(
        packedBandIntervals,physicalBandBudgets);
    float currentIntervalSpan=max(intervalEnd-intervalStart,0.0);
    int currentIntervalBudget=max(
        packedIntervalBudgets[packedIntervalIndex],1);
    // 距離LODは各物理雲帯の入口だけから求める。先に見える別帯の出入りで
    // 後続帯の刻みを変えず、予算由来の最小刻みと空間解像度由来の刻みを分離する。
    float safeCurrentRequestedFineStep=cloudPackedIntervalFineStep(
        intervalStart,intervalEnd,currentIntervalBudget,
        baseFineStep,MAX_DISTANCE);
    int currentFineCellCount=cloudPackedIntervalFineCellCount(
        intervalStart,intervalEnd,currentIntervalBudget,
        safeCurrentRequestedFineStep);
    float3 sun=sunDir.xyz;
    // dir はカメラから雲、sun は雲から太陽へ向く。光子の入射・出射方向は
    // それぞれ -sun と -dir なので、同じ向きのdirとsunが前方散乱になる。
    // HG は全立体角で積分すると1になる位相関数であり、指向性光の放射照度へそのまま掛ける。
    // Lambert面の1/PIは半球反射用で、体積散乱へ流用すると位相積分が4へ増えてしまう。
    // 前方・後方の混合率も物性値として固定し、視線刻みを変えても位相を変えない。
    float multiOcclusion=saturate(cloudLightingMulti.x);
    float multiContribution=min(
        saturate(cloudLightingPhase.w),multiOcclusion);
    float thirdContribution=multiContribution*multiContribution;
    float thirdOcclusion=multiOcclusion*multiOcclusion;
    CloudLightingContext lightingContext;
    lightingContext.sun=sun;
    lightingContext.sunAtCloud=
        sunCol.rgb*cloudSunTransmittance.rgb;
    lightingContext.phase=0.0;
    lightingContext.phaseMulti=0.0;
    lightingContext.coverage=coverage;
    lightingContext.density=density;
    lightingContext.multiOcclusion=multiOcclusion;
    lightingContext.thirdOcclusion=thirdOcclusion;
    lightingContext.lightExtinction=
        density*cloudLightingExtinction.y;
    lightingContext.directionalScatteringScale=
        cloudLightingExtinction.z*cloudLightingGround.w;
    lightingContext=cloudLightingContextForRayDirection(
        lightingContext,dir);
    // 一次透過率が0になった後も残る高次散乱の上限を、実際のHDR太陽強度から求める。
    // 雲テクスチャは非乗算済みの雲放射輝度を保存するため、背景色はこの上限へ混ぜない。
    float3 nonnegativeSunAtCloud=max(
        lightingContext.sunAtCloud,0.0.xxx);
    float directionalScaleUpper=max(
        lightingContext.directionalScatteringScale,0.0);
    // 外側Gauss点の傾きを制限するため、各局所散乱源の物理上限も保持する。
    float3 skyAmbientColorUpper=max(
        max(skyCol.rgb,cloudSkyZenith.rgb),0.0.xxx);
    float ambientStrengthUpper=max(
        max(cloudLightingAmbient.x,cloudLightingAmbient.y),0.0)
        *max(cloudLightingExtinction.z,0.0);
    float3 groundSourceUpper=max(cloudLightingGround.rgb,0.0.xxx)
        *max(cloudLightingMulti.w,0.0)
        *max(cloudLightingExtinction.z,0.0);
    // 各サブレイの位相は異なるため、中心視線の値ではなく許容上限を使う。
    // これにより早期終了判定が周辺サブレイの前方散乱を過小評価しない。
    float phaseUpper=max(cloudLightingMulti.z,0.0);
    float3 firstOrderSourceUpper=
        nonnegativeSunAtCloud*directionalScaleUpper*phaseUpper
        +skyAmbientColorUpper*ambientStrengthUpper
        +groundSourceUpper;
    float3 higherOrderSourceUpper=
        nonnegativeSunAtCloud*directionalScaleUpper*phaseUpper;
    float4 coverageTerms=cloudCoverage;
    float4 transmitLanes=1.0.xxxx;
    float4 secondOrderTransmitLanes=1.0.xxxx;
    float4 thirdOrderTransmitLanes=1.0.xxxx;
    // 極薄区間の透過率を乗算すると1へ丸められるため、光学的深さを直接積算する。
    float4 accumulatedOpticalDepthLanes=0.0.xxxx;
    float4 secondOrderAccumulatedOpticalDepthLanes=0.0.xxxx;
    float4 thirdOrderAccumulatedOpticalDepthLanes=0.0.xxxx;
    CloudFourStateTransportLanes firstOrderTransportState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes secondOrderTransportState=
        cloudInitialFourStateTransportLanes();
    CloudFourStateTransportLanes thirdOrderTransportState=
        cloudInitialFourStateTransportLanes();
    float transmit=1.0;
    float secondOrderTransmit=1.0;
    float thirdOrderTransmit=1.0;
    float3 scatter=float3(0,0,0);
    float depthMoment=0.0;
    // 雲殻内から始まるレイは、最初の粗い区間を飛ばすとカメラ直前の密度を積分できない。
    // 最初の二細密セルだけ確認し、空なら二セル単位の粗い探索へ戻す。
    bool startsInsideShell=intervalStart<=1e-4;
    int fineCellIndex=0;
    bool nearDensity=startsInsideShell;
    int refineUntilCell=startsInsideShell
        ?min(2,currentFineCellCount):0;
    [loop] for(int i=0;i<MAX_STEPS;i++){
        bool intervalFinished=fineCellIndex>=currentFineCellCount;
        if(intervalFinished){
            packedIntervalIndex++;
            if(packedIntervalIndex>=packedBandIntervals.count) break;
            intervalStart=packedBandIntervals.starts[packedIntervalIndex];
            intervalEnd=packedBandIntervals.ends[packedIntervalIndex];
            physicalBandId=
                packedBandIntervals.bandIds[packedIntervalIndex];
            currentIntervalBudget=max(
                packedIntervalBudgets[packedIntervalIndex],1);
            currentIntervalSpan=max(intervalEnd-intervalStart,0.0);
            safeCurrentRequestedFineStep=cloudPackedIntervalFineStep(
                intervalStart,intervalEnd,currentIntervalBudget,
                baseFineStep,MAX_DISTANCE);
            currentFineCellCount=cloudPackedIntervalFineCellCount(
                intervalStart,intervalEnd,currentIntervalBudget,
                safeCurrentRequestedFineStep);
            // 別の連続雲殻区間は晴天域または別物理帯を挟む。未解像状態を
            // 遠側へ持ち越さず、次に各レーンが入った位置で定常分布から再開する。
            firstOrderTransportState.active=0.0.xxxx;
            secondOrderTransportState.active=0.0.xxxx;
            thirdOrderTransportState.active=0.0.xxxx;
            // 薄い遠側区間も粗い採取位相だけで飛ばさないよう、各区間の先頭を確認する。
            fineCellIndex=0;
            nearDensity=true;
            refineUntilCell=min(2,currentFineCellCount);
        }
        int traversalCellCount=nearDensity?1:2;
        int nextFineCellIndex=min(
            fineCellIndex+traversalCellCount,currentFineCellCount);
        float cellStartOffset=cloudRayCellOffset(
            currentIntervalSpan,fineCellIndex,currentFineCellCount,
            safeCurrentRequestedFineStep);
        float cellEndOffset=cloudRayCellOffset(
            currentIntervalSpan,nextFineCellIndex,currentFineCellCount,
            safeCurrentRequestedFineStep);
        float stepLength=max(cellEndOffset-cellStartOffset,0.0);
        if(stepLength<=0.0){
            fineCellIndex=nextFineCellIndex;
            continue;
        }
        // 占有判定は位相に依存しないセル中央で行い、担当幅をセルの両端へ一致させる。
        // 実密度だけを同じセル内でずらすため、隣接セル間に未検査の隙間は生じない。
        float occupancySampleT=
            intervalStart+cellStartOffset+0.5*stepLength;
        float3 occupancyP=camPos.xyz+dir*occupancySampleT;
        // 隣接する実視線差から画素四角形の軸別包絡を作り、レイ区間の
        // 長手方向と加える。円形近似で画面方向の相関を失わない。
        float3 occupancyCrossSectionWidth=
            pixelDirectionSpan*occupancySampleT;
        float3 occupancyPhysicalFootprint=
            abs(dir)*stepLength+occupancyCrossSectionWidth;
        float2 occupancySample=cloudShapeOccupancyAtInterval(
            occupancyP,occupancyPhysicalFootprint);
        // 凝結場の実数上限へ後段の最大物理倍率を掛けて空間棄却する。
        // 0/1支持へ潰すと非飽和密度を過小評価するため、C1正値化後の値を保つ。
        float occupancyDensityUpperBound=occupancySample.x
            *cloudHeightPrecipitationDensityScale(0.0,1.0)
            *density;
        if(occupancySample.y>0.5)
            occupancyDensityUpperBound*=cloudUpperTerms.y;
        // 局所密度しきい値では棄却しない。薄い上界でも長い接線経路では
        // 見える不透明度になり得るため、厳密に支持域が0の場合だけ飛ばす。
        if(occupancyDensityUpperBound<=0.0){
            // 保守的な支持上界が厳密に0なら、画素内の全実光路で参与媒質が
            // 切れている。晴天距離を無視して条件付き状態を次の雲へ渡さない。
            firstOrderTransportState.active=0.0.xxxx;
            secondOrderTransportState.active=0.0.xxxx;
            thirdOrderTransportState.active=0.0.xxxx;
            fineCellIndex=nextFineCellIndex;
            if(!nearDensity||fineCellIndex>=refineUntilCell)
                nearDensity=false;
            continue;
        }
        if(!nearDensity && traversalCellCount>1){
            // 粗い親セルの支持域を見つけた同じ反復で、先頭の細密子セルを積分する。
            // 位置を進めないcontinueを挟まず、整数セル予算内で全区間へ必ず到達する。
            refineUntilCell=nextFineCellIndex;
            nearDensity=true;
            nextFineCellIndex=min(
                fineCellIndex+1,currentFineCellCount);
            cellEndOffset=cloudRayCellOffset(
                currentIntervalSpan,nextFineCellIndex,currentFineCellCount,
                safeCurrentRequestedFineStep);
            stepLength=max(cellEndOffset-cellStartOffset,0.0);
            if(stepLength<=0.0){
                fineCellIndex=nextFineCellIndex;
                continue;
            }
        }
        nearDensity=true;
        refineUntilCell=max(
            refineUntilCell,
            min(nextFineCellIndex+2,currentFineCellCount));
        // 各実サブレイと球殻成分の交差区間へ4点Gauss-Legendre求積を再写像する。
        // レーンごとの実区間長から周波数制限幅も求め、接線境界で中心セル幅を流用しない。
        float cellStartT=intervalStart+cellStartOffset;
        float cellEndT=cellStartT+stepLength;
        // 近側と遠側を同じ長さ・重心へ縮約すると間の晴天域でも相関状態が続く。
        // 動的ループで各連結成分を順に処理し、重い輸送本体を二重展開しない。
        [loop] for(int shellComponentIndex=0;
                   shellComponentIndex<CLOUD_SHELL_COMPONENT_COUNT;
                   ++shellComponentIndex){
        CloudPhysicalSubraySegmentOverlaps cellComponentOverlaps=
            cloudPhysicalSubrayBandOverlaps(
                subrayIntervals,physicalBandId,shellComponentIndex,
                cellStartT,cellEndT);
        if(!any(cellComponentOverlaps.ends>
                cellComponentOverlaps.starts)) continue;
        if(shellComponentIndex>0){
            float4 componentEntryMasks=
                cloudPhysicalSubraySecondComponentEntryMasks(
                    subrayIntervals,physicalBandId,cellStartT,cellEndT);
            // 晴天域を跨いだレーンだけ、遠側の物理入口で定常状態から再開する。
            firstOrderTransportState.active*=
                1.0.xxxx-componentEntryMasks;
            secondOrderTransportState.active*=
                1.0.xxxx-componentEntryMasks;
            thirdOrderTransportState.active*=
                1.0.xxxx-componentEntryMasks;
        }
        // レーン間のBeer状態は独立なので、一レーンの密度・照明・輸送を完結して
        // から次へ進む。4レーン分の太陽光路を同時保持せず、旧FXCの一時領域を抑える。
        [loop] for(int physicalLaneIndex=0;physicalLaneIndex<4;
                   ++physicalLaneIndex){
            float4 physicalLaneSelector=
                cloudPhysicalSubraySelector(physicalLaneIndex);
            float laneComponentStartT=dot(
                physicalLaneSelector,cellComponentOverlaps.starts);
            float laneComponentEndT=dot(
                physicalLaneSelector,cellComponentOverlaps.ends);
            float cellLaneLength=max(
                laneComponentEndT-laneComponentStartT,0.0);
            if(cellLaneLength<=0.0) continue;
            float3 physicalRayDirection=cloudPhysicalSubrayDirectionAt(
                subrayDirections,physicalLaneIndex);
            CloudLightingContext physicalLaneLightingContext=
                cloudLightingContextForRayDirection(
                    lightingContext,physicalRayDirection);
            float physicalDensityResolutionSpacing=cellLaneLength
                *CLOUD_DENSITY_GAUSS_MAXIMUM_GAP_FRACTION;
            float3 physicalDensityLongitudinalFootprint=
                abs(physicalRayDirection)*physicalDensityResolutionSpacing;
            CloudPhysicalLaneDensityLightingSample densityLightingSample0=
                emptyCloudPhysicalLaneDensityLightingSample();
            CloudPhysicalLaneDensityLightingSample densityLightingSample1=
                emptyCloudPhysicalLaneDensityLightingSample();
            CloudPhysicalLaneDensityLightingSample densityLightingSample2=
                emptyCloudPhysicalLaneDensityLightingSample();
            CloudPhysicalLaneDensityLightingSample densityLightingSample3=
                emptyCloudPhysicalLaneDensityLightingSample();
            [loop] for(int densitySampleIndex=0;
                       densitySampleIndex<CLOUD_DENSITY_GAUSS_SAMPLE_COUNT;
                       ++densitySampleIndex){
                float densitySampleFraction=
                    cloudDensityGaussFractionAt(densitySampleIndex);
                CloudPhysicalLaneDensityLightingSample currentDensityLightingSample=
                    sampleCloudPhysicalLaneDensityLightingAtFraction(
                        densitySampleFraction,
                        laneComponentStartT,laneComponentEndT,
                        physicalRayDirection,coverageTerms,
                        physicalDensityLongitudinalFootprint,
                        pixelDirectionSpan,angularPixelFootprint,
                        physicalBandId,
                        fadeStart,MAX_DISTANCE,density,
                        physicalLaneLightingContext);
                if(densitySampleIndex==0)
                    densityLightingSample0=currentDensityLightingSample;
                else if(densitySampleIndex==1)
                    densityLightingSample1=currentDensityLightingSample;
                else if(densitySampleIndex==2)
                    densityLightingSample2=currentDensityLightingSample;
                else
                    densityLightingSample3=currentDensityLightingSample;
            }
            // 密度層境界と照明採取位置を合わせた八区間を、現在レーンの
            // Beer-Lambert吸収重心で積分し、最後に面積重み0.25を掛ける。
            [loop] for(int transportSegmentIndex=0;
                         transportSegmentIndex<CLOUD_BEER_TRANSPORT_SEGMENT_COUNT;
                         ++transportSegmentIndex){
                int densitySampleIndex=transportSegmentIndex>>1;
                CloudPhysicalLaneDensityLightingSample densityLightingSample=
                    densityLightingSample0;
                if(densitySampleIndex==1)
                    densityLightingSample=densityLightingSample1;
                else if(densitySampleIndex==2)
                    densityLightingSample=densityLightingSample2;
                else if(densitySampleIndex==3)
                    densityLightingSample=densityLightingSample3;
                float segmentStart=
                    cloudBeerTransportBoundaryAt(transportSegmentIndex);
                float segmentEnd=
                    cloudBeerTransportBoundaryAt(transportSegmentIndex+1);
                float segmentWidth=segmentEnd-segmentStart;
                float segmentRayStart=cloudIntervalDistanceAtFraction(
                    laneComponentStartT,laneComponentEndT,segmentStart);
                float segmentRayEnd=transportSegmentIndex+1
                    >=CLOUD_BEER_TRANSPORT_SEGMENT_COUNT
                    ?laneComponentEndT
                    :cloudIntervalDistanceAtFraction(
                        laneComponentStartT,laneComponentEndT,segmentEnd);
                float segmentRayLength=max(
                    segmentRayEnd-segmentRayStart,0.0);
                float4 laneSegmentLengths=
                    physicalLaneSelector*segmentRayLength;
                float firstOrderExtinction=max(
                    cloudLightingExtinction.x,0.0);
                float secondOrderExtinction=firstOrderExtinction
                    *max(lightingContext.multiOcclusion,0.0);
                float thirdOrderExtinction=firstOrderExtinction
                    *max(lightingContext.thirdOcclusion,0.0);
                CloudFourStateTransportResultLanes firstTransport=
                    cloudFourStateTransportLanes(
                        densityLightingSample.densityStates.xxxx,
                        densityLightingSample.densityStates.yyyy,
                        densityLightingSample.densityStates.zzzz,
                        densityLightingSample.densityStates.wwww,
                        firstOrderExtinction,
                        densityLightingSample.correlationLength.xxxx,
                        laneSegmentLengths,firstOrderTransportState);
                CloudFourStateTransportResultLanes secondTransport=
                    cloudFourStateTransportLanes(
                        densityLightingSample.densityStates.xxxx,
                        densityLightingSample.densityStates.yyyy,
                        densityLightingSample.densityStates.zzzz,
                        densityLightingSample.densityStates.wwww,
                        secondOrderExtinction,
                        densityLightingSample.correlationLength.xxxx,
                        laneSegmentLengths,secondOrderTransportState);
                CloudFourStateTransportResultLanes thirdTransport=
                    cloudFourStateTransportLanes(
                        densityLightingSample.densityStates.xxxx,
                        densityLightingSample.densityStates.yyyy,
                        densityLightingSample.densityStates.zzzz,
                        densityLightingSample.densityStates.wwww,
                        thirdOrderExtinction,
                        densityLightingSample.correlationLength.xxxx,
                        laneSegmentLengths,thirdOrderTransportState);
                float firstIntervalAbsorption=dot(
                    physicalLaneSelector,firstTransport.absorptions);
                float secondIntervalAbsorption=dot(
                    physicalLaneSelector,secondTransport.absorptions);
                float thirdIntervalAbsorption=dot(
                    physicalLaneSelector,thirdTransport.absorptions);
                float secondIntervalTransmittance=dot(
                    physicalLaneSelector,secondTransport.transmittances);
                float thirdIntervalTransmittance=dot(
                    physicalLaneSelector,thirdTransport.transmittances);
                float viewOpticalDepth=cloudOpticalDepthFromAbsorption(
                    firstIntervalAbsorption);
                float secondOpticalDepth=cloudOpticalDepthFromAbsorption(
                    secondIntervalAbsorption);
                float thirdOpticalDepth=cloudOpticalDepthFromAbsorption(
                    thirdIntervalAbsorption);
                float firstLaneTransmit=dot(
                    physicalLaneSelector,transmitLanes);
                float secondLaneTransmit=dot(
                    physicalLaneSelector,secondOrderTransmitLanes);
                float thirdLaneTransmit=dot(
                    physicalLaneSelector,thirdOrderTransmitLanes);
                float firstSampleWeight=0.25*firstLaneTransmit
                    *firstIntervalAbsorption;
                float secondSampleWeight=0.25*secondLaneTransmit
                    *cloudReducedIntervalScatteringWeight(
                        viewOpticalDepth,secondOpticalDepth,
                        secondIntervalTransmittance,multiContribution);
                float thirdSampleWeight=0.25*thirdLaneTransmit
                    *cloudReducedIntervalScatteringWeight(
                        viewOpticalDepth,thirdOpticalDepth,
                        thirdIntervalTransmittance,thirdContribution);
                float overlapStart=segmentRayStart;
                float firstRayCentroid=overlapStart+dot(
                    physicalLaneSelector,firstTransport.centroidDistances);
                float secondRayCentroid=overlapStart+dot(
                    physicalLaneSelector,secondTransport.centroidDistances);
                float thirdRayCentroid=overlapStart+dot(
                    physicalLaneSelector,thirdTransport.centroidDistances);
                float firstCentroidFraction=firstSampleWeight>0.0
                    ?saturate((firstRayCentroid-laneComponentStartT)
                        /cellLaneLength)
                    :segmentStart+0.5*segmentWidth;
                float secondCentroidFraction=secondSampleWeight>0.0
                    ?saturate((secondRayCentroid-laneComponentStartT)
                        /cellLaneLength)
                    :segmentStart+0.5*segmentWidth;
                float thirdCentroidFraction=thirdSampleWeight>0.0
                    ?saturate((thirdRayCentroid-laneComponentStartT)
                        /cellLaneLength)
                    :segmentStart+0.5*segmentWidth;
                int sourceLeftIndex=0;
                if(transportSegmentIndex>=3) sourceLeftIndex=1;
                if(transportSegmentIndex>=5) sourceLeftIndex=2;
                int sourceRightIndex=sourceLeftIndex+1;
                CloudPhysicalLaneDensityLightingSample leftLightingSample=
                    densityLightingSample0;
                CloudPhysicalLaneDensityLightingSample rightLightingSample=
                    densityLightingSample1;
                if(sourceLeftIndex==1){
                    leftLightingSample=densityLightingSample1;
                    rightLightingSample=densityLightingSample2;
                }
                else if(sourceLeftIndex==2){
                    leftLightingSample=densityLightingSample2;
                    rightLightingSample=densityLightingSample3;
                }
                float leftFraction=leftLightingSample.physicalFraction;
                float rightFraction=rightLightingSample.physicalFraction;
                float leftValid=leftLightingSample.sourceValidity;
                float rightValid=rightLightingSample.sourceValidity;
                float3 firstOrderSource=cloudLinearLightingSourceAtFraction(
                    firstCentroidFraction,leftFraction,rightFraction,
                    leftLightingSample.firstOrderSource,
                    rightLightingSample.firstOrderSource,
                    leftValid,rightValid,firstOrderSourceUpper);
                float3 secondOrderSource=cloudLinearLightingSourceAtFraction(
                    secondCentroidFraction,leftFraction,rightFraction,
                    leftLightingSample.secondOrderSource,
                    rightLightingSample.secondOrderSource,
                    leftValid,rightValid,higherOrderSourceUpper);
                float3 thirdOrderSource=cloudLinearLightingSourceAtFraction(
                    thirdCentroidFraction,leftFraction,rightFraction,
                    leftLightingSample.thirdOrderSource,
                    rightLightingSample.thirdOrderSource,
                    leftValid,rightValid,higherOrderSourceUpper);
                scatter+=firstSampleWeight*firstOrderSource
                    +secondSampleWeight*secondOrderSource
                    +thirdSampleWeight*thirdOrderSource;
                float firstSampleT=laneComponentStartT
                    +firstCentroidFraction*cellLaneLength;
                depthMoment+=firstSampleWeight*firstSampleT;
                accumulatedOpticalDepthLanes=min(
                    accumulatedOpticalDepthLanes
                        +physicalLaneSelector*viewOpticalDepth,
                    80.0.xxxx);
                secondOrderAccumulatedOpticalDepthLanes=min(
                    secondOrderAccumulatedOpticalDepthLanes
                        +physicalLaneSelector*secondOpticalDepth,
                    80.0.xxxx);
                thirdOrderAccumulatedOpticalDepthLanes=min(
                    thirdOrderAccumulatedOpticalDepthLanes
                        +physicalLaneSelector*thirdOpticalDepth,
                    80.0.xxxx);
                transmitLanes=exp(-accumulatedOpticalDepthLanes);
                secondOrderTransmitLanes=exp(
                    -secondOrderAccumulatedOpticalDepthLanes);
                thirdOrderTransmitLanes=exp(
                    -thirdOrderAccumulatedOpticalDepthLanes);
                transmit=saturate(dot(
                    CLOUD_SUBRAY_AREA_WEIGHTS,transmitLanes));
                secondOrderTransmit=saturate(dot(
                    CLOUD_SUBRAY_AREA_WEIGHTS,
                    secondOrderTransmitLanes));
                thirdOrderTransmit=saturate(dot(
                    CLOUD_SUBRAY_AREA_WEIGHTS,
                    thirdOrderTransmitLanes));
            }
        }
        }
        // 残りの有限経路を最大密度で満たしたときの、各散乱次数の最大寄与を求める。
        // 無限媒質の上限を使うと、小さい高次消散率で終了不能になる。
        float traceEnd=packedBandIntervals.ends[
            max(packedBandIntervals.count-1,0)];
        float traceEndUpper=cloudInflatePositiveFloatUpper(
            max(traceEnd,0.0));
        float remainingDistance=cloudPositiveDifferenceUpper(
            traceEndUpper,intervalStart+cellEndOffset);
        float maximumOpticalDensityPerMeter=cloudPositiveProductUpper(
            max(density,0.0),
            cloudCondensationDensity(
                1.0+CLOUD_CONDENSATION_MAXIMUM_POSITIVE_OFFSET));
        maximumOpticalDensityPerMeter=cloudPositiveProductUpper(
            maximumOpticalDensityPerMeter,
            cloudHeightPrecipitationDensityScale(0.0,1.0));
        maximumOpticalDensityPerMeter=cloudPositiveProductUpper(
            maximumOpticalDensityPerMeter,
            max(max(layer.w,cloudUpperTerms.z),0.0));
        maximumOpticalDensityPerMeter=cloudPositiveProductUpper(
            maximumOpticalDensityPerMeter,
            max(cloudLightingExtinction.x,0.0));
        float remainingPrimaryOpticalDepth=cloudPositiveProductUpper(
            remainingDistance,maximumOpticalDensityPerMeter);
        int remainingPackedCellBudget=0;
        [unroll] for(int remainingIntervalIndex=0;
                     remainingIntervalIndex<4;
                     ++remainingIntervalIndex){
            if(remainingIntervalIndex>packedIntervalIndex&&
               remainingIntervalIndex<packedBandIntervals.count)
                remainingPackedCellBudget+=max(
                    packedIntervalBudgets[remainingIntervalIndex],0);
        }
        uint remainingTransportSegmentCount=
            cloudRemainingTransportSegmentCount(
                currentFineCellCount,nextFineCellIndex,
                remainingPackedCellBudget>0,remainingPackedCellBudget);
        float remainingFirstOpacityUpper=cloudTransportWeightUpper(
            transmit,remainingPrimaryOpticalDepth,1.0,1.0,
            remainingTransportSegmentCount);
        float remainingSecondWeightUpper=cloudTransportWeightUpper(
            secondOrderTransmit,remainingPrimaryOpticalDepth,
            multiContribution,multiOcclusion,
            remainingTransportSegmentCount);
        float remainingThirdWeightUpper=cloudTransportWeightUpper(
            thirdOrderTransmit,remainingPrimaryOpticalDepth,
            thirdContribution,thirdOcclusion,
            remainingTransportSegmentCount);
        float3 remainingCloudRadianceUpper=
            cloudPositiveSumUpper3(
                cloudPositiveSumUpper3(
                    cloudPositiveProductUpper3(
                        remainingFirstOpacityUpper,firstOrderSourceUpper),
                    cloudPositiveProductUpper3(
                        remainingSecondWeightUpper,higherOrderSourceUpper)),
                cloudPositiveProductUpper3(
                    remainingThirdWeightUpper,higherOrderSourceUpper));
        float currentOpacity=saturate(1.0-transmit);
        float maximumOpacity=remainingFirstOpacityUpper>0.0
            ?saturate(cloudPositiveSumUpper(
                currentOpacity,remainingFirstOpacityUpper))
            :currentOpacity;
        float3 currentColor=currentOpacity>1e-4
            ?scatter/currentOpacity:0.0.xxx;
        float3 minimumColor=maximumOpacity>currentOpacity
            ?cloudDeflatePositiveFloatLower3(
                scatter/maximumOpacity):currentColor;
        float3 maximumColor=currentColor;
        if(currentOpacity>1e-4
           &&any(remainingCloudRadianceUpper>0.0)){
            float3 maximumRadiance=cloudInflatePositiveFloatUpper3(
                scatter+remainingCloudRadianceUpper);
            maximumColor=cloudInflatePositiveFloatUpper3(
                maximumRadiance/currentOpacity);
        }
        else if(currentOpacity<=1e-4)
            maximumColor=float3(65505.0,65505.0,65505.0);
        bool colorCodeStable=currentOpacity>1e-4
            &&cloudR16ValueRangeKeepsCode(
                currentColor.r,minimumColor.r,maximumColor.r)
            &&cloudR16ValueRangeKeepsCode(
                currentColor.g,minimumColor.g,maximumColor.g)
            &&cloudR16ValueRangeKeepsCode(
                currentColor.b,minimumColor.b,maximumColor.b);
        bool opacityCodeStable=cloudR16ValueRangeKeepsCode(
            currentOpacity,currentOpacity,maximumOpacity);
        bool opacityFloatCodeStable=cloudR32PositiveRangeKeepsCode(
            currentOpacity,maximumOpacity);
        float currentMeanDepth=currentOpacity>1e-4
            ?depthMoment/currentOpacity:250001.0;
        float maximumMeanDepth=currentMeanDepth;
        if(currentOpacity>1e-4&&remainingFirstOpacityUpper>0.0){
            float maximumDepthMoment=cloudPositiveSumUpper(
                depthMoment,cloudPositiveProductUpper(
                    remainingFirstOpacityUpper,traceEndUpper));
            maximumMeanDepth=cloudInflatePositiveFloatUpper(
                maximumDepthMoment/currentOpacity);
        }
        bool opacityDepthStable=cloudR32PositiveRangeKeepsCode(
            currentMeanDepth,maximumMeanDepth);
        if(colorCodeStable&&opacityCodeStable&&opacityFloatCodeStable
           &&opacityDepthStable)
            break;
        fineCellIndex=nextFineCellIndex;
    }
    float baseA = saturate(1.0 - transmit);
    float3 col = baseA>1e-4 ? scatter/baseA : float3(0,0,0);
    bool radianceValid=baseA==baseA
        && all(col==col) && all(abs(col)<=65504.0)
        && depthMoment==depthMoment;
    if(!radianceValid){
        baseA=0.0;
        col=float3(0,0,0);
        depthMoment=0.0;
    }
    col=max(col,0.0);
    float resolvedA=baseA;
    cloudOut[pixelQ]=float4(col,resolvedA);
    float meanDepth=baseA>1e-4 ? depthMoment/baseA : 250001.0;
    if(!(meanDepth==meanDepth) || meanDepth<0.0 || meanDepth>250000.0){
        meanDepth=250001.0;
        resolvedA=0.0;
        cloudOut[pixelQ]=float4(0,0,0,0);
    }
    cloudDepthOut[pixelQ]=float2(meanDepth,resolvedA);
}
)";

// 雲合成: 全画面三角形で cloudTex を AlphaBlend 合成 (sky の上に)。
const char* kCloudCompVS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4x4 prevCameraRelativeViewProj;
    float4 camPos;
    float4 prevCamPos;
    float4 sunDir;
    float4 sunCol;
    float4 skyCol;
    float4 params;
    float4 dims;
    float4 temporal;
    float4 layer;
    float4 worldOrigin;
    float4 shadowGrid;
    float4 shadowState;
    float4 groundHorizon;
};
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 farPoint:TEXCOORD1; };
float4 CloudFarPoint(float2 uv) {
    float4 clip=float4(uv.x*2.0-1.0,-(uv.y*2.0-1.0),1.0,1.0);
    return mul(clip,invViewProj);
}
VSOut VSMain(uint id:SV_VertexID){
    float2 uv=float2((id<<1)&2, id&2);
    VSOut o; o.uv=uv; o.pos=float4(uv.x*2.0-1.0,-(uv.y*2.0-1.0),0.0,1.0);
    // 逆射影は三頂点だけで行い、画素中心の遠点は線形補間する。
    o.farPoint=CloudFarPoint(uv);
    return o;
}
)";

// Scaled raymarch → full-res reconstruction。low-res の代表 cloud depth で
// 3x3 bilateral gather を導き、world-space layer と wind motion を補正した
// cloud hit point で history を再投影する。color/depth は一つの compute
// dispatch から別 format UAV へ同時に出力し、raster/MRT overhead と重複 read を避ける。
const char* kCloudResolveCS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4x4 prevCameraRelativeViewProj;
    float4 camPos;
    float4 prevCamPos;
    float4 sunDir;
    float4 sunCol;
    float4 skyCol;
    float4 params;
    float4 dims;       // xy=ray-march 解像度, zw=全解像度
    float4 temporal;   // x=history valid, y=previous wind, z=frame/phase, w=4x4 TSR divisor
    float4 layer;      // x=world base Y, y=world top Y, z=XZ noise scale, w=1 m当たりの基準消散
    float4 worldOrigin;// xyz=rebased shell origin, w=history camera stationary
    float4 shadowGrid;
    float4 shadowState;
    float4 groundHorizon;// xyz=camera local up, w=ground tangent elevation; <-1 disables
    float4 cloudFrameTerms;
    float4 cloudLightTangent;
    float4 cloudLightBitangent;
    float4 cloudCoverage;
    float4 cloudCoverageReciprocals;
    float4 cloudShellRayOrigin;
    float4 cloudShellTerms;
    float4 cloudLightingExtinction;
    float4 cloudLightingPhase;
    float4 cloudLightingMulti;
    float4 cloudLightingAmbient;
    float4 cloudLightingGround;
    float4 cloudSunTransmittance;
    float4 cloudSkyZenith;
    float4 cloudMultiPhase;
    float4 cloudRange;
    float4 cloudUpperLayer;
    float4 cloudUpperTerms;
    float4 cloudEvolution;
    float4 cloudWeatherControl;
    float4 cloudShadowUpdate;
    float4 cloudWorldShadowMap;
    float4 cloudPreviousEvolution;
    float4 cloudWorldShadowUpdate;
    float4 cloudLightingHistory;
};
Texture2D<float4> cloudLow     : register(t0);
Texture2D<float2> cloudDepth   : register(t1);
Texture2D<float4> historyColor : register(t2);
Texture2D<float2> historyDepth : register(t3);
RWTexture2D<float4> historyColorOut : register(u0);
RWTexture2D<float2> historyDepthOut : register(u1);
SamplerState cloudLow_sampler     : register(s0);
SamplerState cloudDepth_sampler   : register(s1);
SamplerState historyColor_sampler : register(s2);
SamplerState historyDepth_sampler : register(s3);
static const float CLOUD_PLANET_RADIUS=6360000.0;
static const float4 CLOUD_TEMPORAL_MIN_RANGE=float4(0.015,0.015,0.015,0.025);
float2 LowUv(int2 q) {
    q=clamp(q,int2(0,0),int2(dims.xy)-1);
    return (float2(q)+0.5)/dims.xy;
}
uint CloudTemporalBlockPhase4(uint2 blockQ,uint phaseIndex) {
    // Keep this equivalent to the march shader. A deterministic per-block
    // rotation breaks the global history-age lattice without changing the
    // number of rays, taps, phases, or the sample set within each block.
    uint blockHash=blockQ.x*0x8da6b343u^blockQ.y*0xd8163841u;
    blockHash^=blockHash>>16u;
    blockHash*=0x7feb352du;
    blockHash^=blockHash>>15u;
    return (phaseIndex+(blockHash&15u))&15u;
}
uint2 CloudTemporalPhaseOffset4(uint2 blockQ,uint phaseIndex) {
    const uint2 offsets[16]={
        uint2(0,0),uint2(2,2),uint2(2,0),uint2(0,2),
        uint2(1,1),uint2(3,3),uint2(3,1),uint2(1,3),
        uint2(1,0),uint2(3,2),uint2(3,0),uint2(1,2),
        uint2(0,1),uint2(2,3),uint2(2,1),uint2(0,3)};
    return offsets[CloudTemporalBlockPhase4(blockQ,phaseIndex)];
}
bool IsTemporalSuperResolution() {
    return temporal.w>3.5;
}
bool IsScheduledFullPixel(uint2 pixel,uint2 phaseOffset) {
    return ((pixel.x&3u)==phaseOffset.x) &&
           ((pixel.y&3u)==phaseOffset.y);
}
// 対流位相が変わった画素では、前フレームの雲形状をそのまま再利用しない。
// 位相差は局所形状の変化量を表し、履歴の再混合率へ連続的に反映する。
float CloudTemporalEvolutionMismatch() {
    float2 slowDelta=abs(cloudEvolution.xy-cloudPreviousEvolution.xy);
    float2 fineDelta=abs(cloudEvolution.zw-cloudPreviousEvolution.zw);
    float delta=max(max(slowDelta.x,slowDelta.y),max(fineDelta.x,fineDelta.y));
    return saturate(delta*220.0);
}
// 照明は形状を壊さないため、前フレームを捨てず現在の放射輝度へ連続収束させる。
// CPUが正規化した角度・放射輝度差を使い、太陽や空の切替でも1フレームで追従する。
float CloudTemporalLightingMismatch() {
    return saturate(cloudLightingHistory.x);
}
// 16フレームぶりの等倍標本を一度に表示せず、同じ雲体と判定済みの履歴へ段階的に反映する。
// 静止形状でも10周期後の残差を4%未満にし、対流差が大きい場合は現在形状へ速く追従する。
float CloudTemporalScheduledCurrentWeight(float evolutionMismatch) {
    return saturate(0.28+evolutionMismatch*2.0);
}
// 4x4時間再構成ではない縮小描画は全画素の現在再構成を毎フレーム持つ。
// 現在値を最低18%反映し、低解像度標本の変化を抑えながら再投影へ固定しない。
float CloudTemporalScaledCurrentWeight(float evolutionMismatch) {
    return max(0.18,saturate(evolutionMismatch));
}
float2 CurrentSamplePixel(int2 q,uint phaseIndex,bool temporalSuperRes) {
    q=clamp(q,int2(0,0),int2(dims.xy)-1);
    float2 samplePixel=(float2(q)+0.5)*(dims.zw/dims.xy)-0.5;
    if(temporalSuperRes) {
        uint2 phaseOffset=CloudTemporalPhaseOffset4(
            uint2(q),phaseIndex);
        uint2 exactPixel=min(
            uint2(q)*4u+phaseOffset,uint2(dims.zw)-1u);
        samplePixel=float2(exactPixel);
    }
    return samplePixel;
}
float3 ResolveViewDirection(float2 uv) {
    float2 ndc=float2(uv.x*2.0-1.0,-(uv.y*2.0-1.0));
    float4 farHomogeneous=mul(
        float4(ndc,1.0,1.0),invViewProj);
    bool perspective=abs(invViewProj[2][3])>1.0e-7;
    float3 candidate=perspective
        ?farHomogeneous.xyz
        :mul(float4(0.0,0.0,1.0,0.0),invViewProj).xyz;
    float lengthSquared=dot(candidate,candidate);
    return lengthSquared>1.0e-12&&lengthSquared<3.0e38
        ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}
// 現在フレームの低解像度標本を、履歴と同じ乗算済み表現へ揃える。
float4 CloudTemporalPackedCurrent(int2 q) {
    q=clamp(q,int2(0,0),int2(dims.xy)-1);
    float4 currentSample=cloudLow.Load(int3(q,0));
    return float4(currentSample.rgb*currentSample.a,currentSample.a);
}
// 履歴を現在近傍の範囲へ収めるだけにし、現在値との平均化による細部の拡散を避ける。
float4 CloudTemporalClipHistory(float4 historyPacked,float4 currentMin,float4 currentMax) {
    float4 currentRange=max(currentMax-currentMin,CLOUD_TEMPORAL_MIN_RANGE);
    return clamp(historyPacked,currentMin-currentRange*0.35,currentMax+currentRange*0.35);
}
// 8bitで視認できる不透明度または輝度差がある場合だけ、十字近傍の範囲を確認する。
bool CloudTemporalNeedsNeighborhoodClip(float4 historyPacked,float4 currentPacked) {
    float alphaDifference=abs(historyPacked.a-currentPacked.a);
    float luminanceDifference=abs(dot(historyPacked.rgb-currentPacked.rgb,float3(0.2126,0.7152,0.0722)));
    return alphaDifference>CLOUD_TEMPORAL_MIN_RANGE.a||luminanceDifference>CLOUD_TEMPORAL_MIN_RANGE.r;
}
// 近傍確認を画素ごとに8位相へ分散し、連続した格子を作らず最大8フレーム以内に外れを補正する。
bool CloudTemporalNeighborhoodClipScheduled(uint2 pixel,uint phaseIndex) {
    return (CloudTemporalBlockPhase4(pixel,phaseIndex)&7u)==0u;
}

[numthreads(8,8,1)]
void CSResolve(uint3 tid : SV_DispatchThreadID) {
    uint fullW=(uint)dims.z;
    uint fullH=(uint)dims.w;
    if(tid.x>=fullW || tid.y>=fullH) return;
    float2 uv=(float2(tid.xy)+0.5)/dims.zw;
    bool temporalSuperRes=IsTemporalSuperResolution();
    float evolutionMismatch=CloudTemporalEvolutionMismatch();
    float lightingMismatch=CloudTemporalLightingMismatch();
    float temporalMismatch=max(evolutionMismatch,lightingMismatch);
    // 未採取画素の現在値は別の等倍レイから作った空間再構成なので、固定割合で混ぜると
    // 16フレームの正確な画素履歴を毎フレームぼかす。非剛体な対流変化が実際に進んだ分だけ
    // 現在値へ寄せ、変化が無い場合は次の等倍採取まで画素別履歴をそのまま保つ。
    float temporalCurrentWeight=temporalMismatch;
    float scheduledCurrentWeight=
        CloudTemporalScheduledCurrentWeight(temporalMismatch);
    float scaledCurrentWeight=
        CloudTemporalScaledCurrentWeight(temporalMismatch);
    uint phaseIndex=(uint)temporal.z&15u;
    uint2 pixelBlock=min(tid.xy>>2u,uint2(dims.xy)-1u);
    uint2 phaseOffset=CloudTemporalPhaseOffset4(pixelBlock,phaseIndex);
    bool scheduled=temporalSuperRes &&
        IsScheduledFullPixel(tid.xy,phaseOffset);
    float2 lowPos=temporalSuperRes
        ?(float2(tid.xy)-float2(phaseOffset))*0.25
        :uv*dims.xy-0.5;
    int2 nearestQ=clamp(int2(floor(lowPos+0.5)),int2(0,0),int2(dims.xy)-1);
    float2 nearestUv=LowUv(nearestQ);
    // The low trace is spatially complete every frame. Every read below is
    // current; temporal scheduling exists only in full-resolution output space.
    float4 refC=cloudLow.SampleLevel(cloudLow_sampler,nearestUv,0);
    float2 refD=cloudDepth.SampleLevel(cloudDepth_sampler,nearestUv,0);
    bool nativeMarch =
        dims.x+0.5>=dims.z && dims.y+0.5>=dims.w &&
        !temporalSuperRes;
    float2 nativeDepth=float2(
        (refC.a>0.003 && refD.x<=250000.0)?refD.x:250001.0,
        saturate(refC.a));
    // 安定した履歴では高価な両側採取を省くが、通常経路と同じ乗算済み表現を保つ。
    // 地平線被覆は履歴へ保存せず、最終合成で一度だけ適用する。
    bool stableHistoryResolved=false;
    float4 resolved=float4(0,0,0,0);
    float2 resolvedDepth=float2(250001.0,0.0);

    // The steady-state TSR resolve does not need a nine-tap spatial fallback
    // for the interior of a stable cloud. Reproject those pixels first, while
    // retaining the complete bilateral path for empty sky, silhouettes,
    // disocclusions, camera motion and uncertain coverage. In particular,
    // empty pixels must reach the gather: one low texel represents a 4x4 phase
    // block and cannot conservatively classify a horizon edge by itself.
    bool stableUnscheduled=temporal.x>0.5 && temporalSuperRes &&
        !scheduled && worldOrigin.w>0.5 && temporalMismatch<0.08;
    if(stableUnscheduled) {
        float4 sameScreenColor=historyColor.Load(int3(tid.xy,0));
        float2 sameScreenDepth=historyDepth.Load(int3(tid.xy,0));
        bool currentDefinitelyEmpty=refC.a<=0.003 && refD.x>250000.0;
        if(currentDefinitelyEmpty && sameScreenColor.a<=0.003) {
            // A single low texel cannot classify a 4x4 TSR block. Confirm a
            // full-resolution 5x5 cross/corner neighbourhood from accumulated
            // history before taking the cheap empty-sky path. Horizon and
            // natural cloud silhouettes therefore always reach the bilateral
            // reconstruction below.
            float maximumHistoryAlpha=0.0;
            [unroll] for(int emptyY=-1;emptyY<=1;emptyY++) {
                [unroll] for(int emptyX=-1;emptyX<=1;emptyX++) {
                    int2 emptyPixel=clamp(
                        int2(tid.xy)+int2(emptyX,emptyY)*2,
                        int2(0,0),int2(dims.zw)-1);
                    maximumHistoryAlpha=max(
                        maximumHistoryAlpha,
                        historyColor.Load(int3(emptyPixel,0)).a);
                }
            }
            if(maximumHistoryAlpha<=0.003) {
                historyColorOut[tid.xy]=float4(0,0,0,0);
                historyDepthOut[tid.xy]=float2(250001.0,0.0);
                return;
            }
        }

        bool seedValid=sameScreenDepth.x<=250000.0 &&
            sameScreenDepth.y>0.08 && sameScreenColor.a>0.08 &&
            abs(sameScreenColor.a-sameScreenDepth.y)<0.08;
        bool currentInterior=refC.a>0.12 && refD.x<=250000.0 &&
            abs(refC.a-sameScreenColor.a)<0.42;
        if(seedValid && currentInterior) {
            float3 stableRay=ResolveViewDirection(uv);
            float stableWindDelta=params.z-temporal.y;
            // 現在の相対位置へカメラ移動量と風の逆移流だけを加え、
            // 大きなワールド座標を経由せず前フレームの位置を作る。
            float3 stablePrevCameraRelativeP=stableRay*sameScreenDepth.x+(camPos.xyz-prevCamPos.xyz)-float3(stableWindDelta*0.9284767,0.0,stableWindDelta*0.3713907);
            float4 stablePrevClip=mul(float4(stablePrevCameraRelativeP,1.0),prevCameraRelativeViewProj);
            if(stablePrevClip.w>1e-5) {
                float2 stablePrevNdc=stablePrevClip.xy/stablePrevClip.w;
                float2 stableHistoryUv=float2(
                    stablePrevNdc.x*0.5+0.5,
                    -stablePrevNdc.y*0.5+0.5);
                bool stableOnScreen=all(stableHistoryUv>=0.001) &&
                    all(stableHistoryUv<=0.999);
                if(stableOnScreen) {
                    int2 stableHistoryPixel=clamp(
                        int2(stableHistoryUv*dims.zw),int2(0,0),
                        int2(dims.zw)-1);
                    float4 stableHist=historyColor.Load(
                        int3(stableHistoryPixel,0));
                    float2 stableHistD=historyDepth.Load(
                        int3(stableHistoryPixel,0));
                    float stableExpectedDepth=length(stablePrevCameraRelativeP);
                    float stableDepthTolerance=max(
                        0.30,stableExpectedDepth*0.01);
                    bool stableDepthOk=stableHistD.x<=250000.0 &&
                        stableHistD.y>0.08 &&
                        abs(stableHistD.x-stableExpectedDepth)<
                            stableDepthTolerance;
                    bool stableAlphaOk=stableHist.a>0.08 &&
                        abs(stableHist.a-sameScreenDepth.y)<0.42;
                    if(stableDepthOk && stableAlphaOk) {
                        // 現在の十字近傍が示す範囲から外れた、古い16位相履歴だけを制限する。
                        // 値は混ぜないため、広い雲縁や房の範囲内にある画素別の細部は維持する。
                        float4 stableHistPacked=float4(stableHist.rgb*stableHist.a,stableHist.a);
                        float4 stableReferencePacked=float4(refC.rgb*refC.a,refC.a);
                        if(CloudTemporalNeighborhoodClipScheduled(tid.xy,phaseIndex) && CloudTemporalNeedsNeighborhoodClip(stableHistPacked,stableReferencePacked)) {
                            float4 stableCurrentMin=stableReferencePacked;
                            float4 stableCurrentMax=stableReferencePacked;
                            const int2 stableOffsets[4]={int2(-1,0),int2(1,0),int2(0,-1),int2(0,1)};
                            [unroll] for(int stableTap=0;stableTap<4;stableTap++) {
                                float4 stableCurrent=CloudTemporalPackedCurrent(nearestQ+stableOffsets[stableTap]);
                                stableCurrentMin=min(stableCurrentMin,stableCurrent);
                                stableCurrentMax=max(stableCurrentMax,stableCurrent);
                            }
                            stableHistPacked=CloudTemporalClipHistory(stableHistPacked,stableCurrentMin,stableCurrentMax);
                        }
                        resolved=lerp(
                            stableHistPacked,stableReferencePacked,
                            temporalCurrentWeight);
                        float stableCurrentDepth=refD.x<=250000.0
                            ?refD.x:sameScreenDepth.x;
                        resolvedDepth=float2(
                            lerp(sameScreenDepth.x,stableCurrentDepth,
                                 temporalCurrentWeight),
                            resolved.a);
                        stableHistoryResolved=true;
                    }
                }
            }
        }
    }

    if(!stableHistoryResolved) {
    float3 premulSum=0.0;
    float alphaSum=0.0, weightSum=0.0, depthSum=0.0, depthWeight=0.0;
    float4 neighborhoodMin=float4(1e6,1e6,1e6,1e6);
    float4 neighborhoodMax=float4(-1e6,-1e6,-1e6,-1e6);
    int2 baseQ=int2(floor(lowPos));
    [unroll] for(int oy=-1;oy<=1;oy++) {
        [unroll] for(int ox=-1;ox<=1;ox++) {
            int2 q=clamp(baseQ+int2(ox,oy),int2(0,0),int2(dims.xy)-1);
            float2 suv=LowUv(q);
            float4 c=cloudLow.SampleLevel(cloudLow_sampler,suv,0);
            float2 d=cloudDepth.SampleLevel(cloudDepth_sampler,suv,0);
            if(d.y<0.0) continue;
            float2 samplePixel=CurrentSamplePixel(
                q,phaseIndex,temporalSuperRes);
            float2 pixelStride=max(dims.zw/dims.xy,1.0);
            float2 delta=(samplePixel-float2(tid.xy))/pixelStride;
            float marchScale=saturate(dims.x/max(dims.z,1.0));
            float spatialSharpness=lerp(0.72,1.80,
                saturate((marchScale-0.50)*2.0));
            float spatial=exp(-dot(delta,delta)*spatialSharpness);
            bool refEmpty=refC.a<0.008 || refD.x>250000.0;
            bool tapEmpty=c.a<0.008 || d.x>250000.0;
            float bilateral;
            if(refEmpty || tapEmpty) {
                if(refEmpty!=tapEmpty) {
                    // Empty/occupied taps define a volumetric coverage edge,
                    // not an unrelated surface. Keep strong cloud banks sharp,
                    // while allowing low-alpha wisps and the analytic horizon
                    // fade to reconstruct continuously instead of snapping to
                    // the nearest quarter-resolution occupancy block.
                    bilateral=exp(-max(refC.a,c.a)*5.0);
                } else {
                    bilateral=1.0;
                }
            }
            else {
                float depthScale=max(0.12,min(refD.x,d.x)*0.045);
                bilateral=exp(-abs(d.x-refD.x)/depthScale)
                         * exp(-abs(c.a-refC.a)*4.0);
            }
            float w=max(spatial*bilateral,1e-5);
            float3 premul=c.rgb*c.a;
            premulSum+=premul*w;
            alphaSum+=c.a*w;
            weightSum+=w;
            if(!tapEmpty) { depthSum+=d.x*c.a*w; depthWeight+=c.a*w; }
            float4 packed=float4(premul,c.a);
            neighborhoodMin=min(neighborhoodMin,packed);
            neighborhoodMax=max(neighborhoodMax,packed);
        }
    }
    float gatheredA=saturate(alphaSum/max(weightSum,1e-5));
    float3 gatheredPremul=premulSum/max(weightSum,1e-5);
    float gatheredDepth=depthWeight>1e-5
        ?depthSum/depthWeight:250001.0;
    float4 spatialCurrent=float4(gatheredPremul,gatheredA);
    float2 spatialDepth=float2(gatheredDepth,gatheredA);
    // 採取位相の画素は4x4領域内の等倍レイを持つ。未採取画素は、初回表示や
    // 新規露出時だけ現在フレームの両側再構成を使い、有効な履歴はぼかさない。
    bool exactCurrent=nativeMarch || scheduled;
    float curA=exactCurrent
        ? saturate(refC.a)
        : gatheredA;
    float3 curPremul=exactCurrent
        ? refC.rgb*refC.a
        : gatheredPremul;
    float curDepth=exactCurrent
        ? ((refC.a>0.003 && refD.x<=250000.0)?refD.x:250001.0)
        : gatheredDepth;
    float4 current=float4(curPremul,curA);
    resolved=current;
    resolvedDepth=nativeMarch
        ?nativeDepth:float2(curDepth,curA);
    float reprojectionDepth=curDepth;
    float2 seedDepth=float2(curDepth,curA);
    if(temporalSuperRes && !scheduled && worldOrigin.w>0.5) {
        // At a stationary camera this pixel's accumulated exact subpixel depth
        // is a better reprojection seed than the current phase's bilateral
        // neighbourhood mean. Prefer it whenever it represents cloud, so wispy
        // edges survive all fifteen unscheduled frames. Camera motion keeps the
        // current bilateral seed above and cannot create view-locked trails.
        float2 sameScreenDepth=historyDepth.Load(int3(tid.xy,0));
        bool sameScreenSeedValid=
            sameScreenDepth.x<=250000.0 && sameScreenDepth.y>0.001;
        if(sameScreenSeedValid) {
            seedDepth=sameScreenDepth;
            reprojectionDepth=sameScreenDepth.x;
        }
    }
    bool historyAccepted=false;
    if(temporal.x>0.5 &&
       reprojectionDepth<=250000.0 && seedDepth.y>0.001) {
        float3 ray=ResolveViewDirection(uv);
        // 同じ密度形状を前フレームへ戻す。相対位置へカメラ移動量を加え、
        // 風の移流量を引くことで、ワールド固定を保ったまま精度低下を避ける。
        float windDelta=params.z-temporal.y;
        float3 prevCameraRelativeP=ray*reprojectionDepth+(camPos.xyz-prevCamPos.xyz)-float3(windDelta*0.9284767,0.0,windDelta*0.3713907);
        float4 prevClip=mul(float4(prevCameraRelativeP,1.0),prevCameraRelativeViewProj);
        if(prevClip.w>1e-5) {
            float2 prevNdc=prevClip.xy/prevClip.w;
            float2 historyUv=float2(prevNdc.x*0.5+0.5,-prevNdc.y*0.5+0.5);
            bool onScreen=all(historyUv>=0.001)&&all(historyUv<=0.999);
            if(onScreen) {
                int2 historyPixel=clamp(
                    int2(historyUv*dims.zw),int2(0,0),int2(dims.zw)-1);
                // Color and depth must describe one surface. Paired point loads
                // prevent a linear depth sample from selecting a different
                // cloud edge than the color sample used for reprojection.
                float4 hist=historyColor.Load(int3(historyPixel,0));
                float2 histD=historyDepth.Load(int3(historyPixel,0));
                float expectedDepth=length(prevCameraRelativeP);
                float depthTolerance=max(0.30,expectedDepth*0.01);
                bool depthOk=histD.x<=250000.0 && histD.y>0.001
                    && abs(histD.x-expectedDepth)<depthTolerance;
                // 未採取画素のcurAは同じ4x4領域にある別レイの空間再構成であり、
                // その画素自身の現在被覆ではない。等倍標本がある経路だけ空／雲の不一致を判定する。
                bool occupancyMismatch=(!temporalSuperRes || scheduled) && ((curA<0.02 && hist.a>0.08) || (curA>0.08 && hist.a<0.02));
                bool alphaOk=!occupancyMismatch &&
                    abs(hist.a-seedDepth.y)<0.42;
                if(depthOk && alphaOk) {
                    float4 histPacked=float4(hist.rgb*hist.a,hist.a);
                    if(!temporalSuperRes || scheduled) {
                        histPacked=CloudTemporalClipHistory(histPacked,neighborhoodMin,neighborhoodMax);
                    }
                    if(temporalSuperRes) {
                        if(scheduled) {
                            // 等倍標本の100%置換は16フレーム周期の輝度差を点滅として露出する。
                            // 現在近傍へ制限した同一雲体の履歴だけを残し、周期差を段階的に収束させる。
                            resolved=lerp(histPacked,current,scheduledCurrentWeight);
                            resolvedDepth=float2(curDepth,curA);
                        } else {
                            // 未採取画素は、他の15位相で得た等倍標本をワールド移動だけ補正して保つ。
                            // 4x4領域の代表レイを混ぜると、正確な履歴を低解像度補間へ毎回戻して雲頂差を面状に拡散する。
                            resolved=lerp(
                                histPacked,current,temporalCurrentWeight);
                            float currentDepthForResolve=curDepth<=250000.0
                                ?curDepth:reprojectionDepth;
                            resolvedDepth=float2(
                                lerp(reprojectionDepth,currentDepthForResolve,
                                     temporalCurrentWeight),
                                resolved.a);
                        }
                    } else if(nativeMarch) {
                        // 等倍追跡だけは全画素が現在フレームの実レイを持つため、履歴を混ぜない。
                        resolved=current;
                        resolvedDepth=nativeDepth;
                    } else {
                        // 任意倍率の縮小追跡を等倍と誤分類すると、粗い空間再構成が毎フレーム
                        // 直接表示される。近傍制限済み履歴へ全画素の現在値を連続反映する。
                        resolved=lerp(histPacked,current,scaledCurrentWeight);
                        resolvedDepth=float2(curDepth,resolved.a);
                    }
                    historyAccepted=true;
                }
            }
        }
    }

    // Empty sky has no world depth to reproject. It is nevertheless stable at
    // the same screen coordinate; retain it for an unscheduled phase when both
    // history and the conservative spatial fallback agree that it is empty.
    if(temporal.x>0.5 && temporalSuperRes && !scheduled &&
       worldOrigin.w>0.5 &&
       !historyAccepted && curA<=0.003) {
        float4 hist=historyColor.Load(int3(tid.xy,0));
        float2 histD=historyDepth.Load(int3(tid.xy,0));
        if(hist.a<=0.003 && (histD.x>250000.0 || histD.y<=0.001)) {
            resolved=float4(0,0,0,0);
            resolvedDepth=float2(250001.0,0.0);
        }
    }
    // 新規露出や初回フレームで履歴を使えない採取画素も、等倍レイと周囲の現在再構成を
    // 同じ反映率で混ぜる。両側再構成への100%切り替えによる周期的な粗さを避ける。
    if(temporalSuperRes && scheduled && !historyAccepted) {
        resolved=lerp(spatialCurrent,current,scheduledCurrentWeight);
        float fallbackDepth=curDepth<=250000.0
            ?curDepth:spatialDepth.x;
        resolvedDepth=float2(fallbackDepth,resolved.a);
    }
    }

    // 履歴には地平線被覆前の値を保存し、部分被覆をフレームごとに重ねない。
    float outA=saturate(resolved.a);
    resolvedDepth.y=outA;
    if(outA<=0.001) {
        resolved.rgb=0.0;
        outA=0.0;
        resolvedDepth=float2(250001.0,0.0);
    }
    historyColorOut[tid.xy]=float4(
        outA>1e-5 ? resolved.rgb/outA : float3(0,0,0),outA);
    historyDepthOut[tid.xy]=resolvedDepth;
}
)";

const char* kCloudCompPS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4x4 prevCameraRelativeViewProj;
    float4 camPos;
    float4 prevCamPos;
    float4 sunDir;
    float4 sunCol;
    float4 skyCol;
    float4 params;
    float4 dims;
    float4 temporal;
    float4 layer;
    float4 worldOrigin;
    float4 shadowGrid;
    float4 shadowState;
    float4 groundHorizon;
    float4 cloudFrameTerms;
    float4 cloudLightTangent;
    float4 cloudLightBitangent;
    float4 cloudCoverage;
    float4 cloudCoverageReciprocals;
    float4 cloudShellRayOrigin;
    float4 cloudShellTerms;
    float4 cloudLightingExtinction;
    float4 cloudLightingPhase;
    float4 cloudLightingMulti;
    float4 cloudLightingAmbient;
    float4 cloudLightingGround;
    float4 cloudSunTransmittance;
    float4 cloudSkyZenith;
    float4 cloudMultiPhase;
    float4 cloudRange;
    float4 cloudUpperLayer;
    float4 cloudUpperTerms;
    float4 cloudEvolution;
    float4 cloudWeatherControl;
    float4 cloudShadowUpdate;
    float4 cloudWorldShadowMap;
    // 前フレームの対流位相。合成段階では使わないが、定数バッファの末尾を共通化する。
    float4 cloudPreviousEvolution;
    float4 cloudWorldShadowUpdate;
    float4 cloudLightingHistory;
};
Texture2D<float4> cloudTex : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float2> cloudDepth : register(t2);
SamplerState cloudTex_sampler : register(s0);
SamplerState sceneDepth_sampler : register(s1);
SamplerState cloudDepth_sampler : register(s2);
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 farPoint:TEXCOORD1; };
float3 CloudCompositeViewDirection(float4 farHomogeneous) {
    bool perspective=abs(invViewProj[2][3])>1.0e-7;
    float3 candidate=perspective
        ?farHomogeneous.xyz
        :mul(float4(0.0,0.0,1.0,0.0),invViewProj).xyz;
    float lengthSquared=dot(candidate,candidate);
    return lengthSquared>1.0e-12&&lengthSquared<3.0e38
        ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}
static const float CLOUD_PLANET_RADIUS=6360000.0;
float CloudCameraAltitude(){
    float3 local=camPos.xyz-worldOrigin.xyz;
    float radialY=max(CLOUD_PLANET_RADIUS+local.y,1.0);
    float radialXzSquared=dot(local.xz,local.xz);
    float q=radialXzSquared/radialY;
    return local.y+q*(0.5-q/radialY*0.125);
}
// 地上視点でだけ地面の地平線より下を除外する。雲層内または雲層より上では、
// 下向きの視線が雲を通過して地面へ向かうため、地平線除外を適用しない。
bool CloudCameraBelowCloudBase(){
    return CloudCameraAltitude()<layer.x;
}
// 履歴へ混ぜず、表示する全解像度画素で地平線被覆を一度だけ求める。
float CloudGroundCoverage(VSOut v) {
    float result=1.0;
    bool cameraBelowCloudBase=CloudCameraBelowCloudBase();
    if(groundHorizon.w>=-1.0&&cameraBelowCloudBase) {
        uint2 fullSize=max(uint2(dims.zw),uint2(1,1));
        uint2 pixel=min(uint2(v.pos.xy),fullSize-1u);
        float centerElevation=dot(
            CloudCompositeViewDirection(v.farPoint),groundHorizon.xyz);
        if(centerElevation<groundHorizon.w-0.02) {
            result=0.0;
        } else if(centerElevation<groundHorizon.w+0.02) {
            float xOffset=pixel.x+1u<fullSize.x?1.0:-1.0;
            float yOffset=pixel.y+1u<fullSize.y?1.0:-1.0;
            // 同次遠点は画面座標に対して線形なので、行列の対応行だけで隣接画素へ進める。
            float4 xFarP=v.farPoint+xOffset*(2.0/dims.z)*invViewProj[0];
            float4 yFarP=v.farPoint-yOffset*(2.0/dims.w)*invViewProj[1];
            float xElevation=dot(CloudCompositeViewDirection(xFarP),groundHorizon.xyz);
            float yElevation=dot(CloudCompositeViewDirection(yFarP),groundHorizon.xyz);
            float coverageHalfWidth=max(0.5*(abs(xElevation-centerElevation)+abs(yElevation-centerElevation)),1e-6);
            result=smoothstep(groundHorizon.w-coverageHalfWidth,groundHorizon.w+coverageHalfWidth,centerElevation);
        }
    }
    return result;
}
float4 PSMain(VSOut v):SV_TARGET {
    float4 cloud = cloudTex.SampleLevel(cloudTex_sampler, v.uv, 0.0);
    float2 cloudHit = cloudDepth.SampleLevel(cloudDepth_sampler, v.uv, 0.0);
    if (cloud.a < 0.001 || cloudHit.y < 0.001 || cloudHit.x > 250000.0)
        return float4(0,0,0,0);
    float groundCoverage=CloudGroundCoverage(v);
    cloud.a*=groundCoverage;
    cloudHit.y*=groundCoverage;
    bool cloudCompositeFinite=all(cloud==cloud)
        &&all(abs(cloud)<=65504.0)
        &&all(cloudHit==cloudHit)
        &&all(abs(cloudHit)<=250001.0)
        &&groundCoverage==groundCoverage;
    if(!cloudCompositeFinite)
        return float4(0,0,0,0);
    if (cloud.a < 0.001)
        return float4(0,0,0,0);

    // 深度が存在するだけで雲を消さず、雲と形状の実距離を比較する。
    // これにより、雲層の内側や上側から見下ろしたときも手前の雲を残す。
    float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0);
    // 通常深度は1.0で消去されるため、それ未満は遠クリップ面付近も含めて形状として扱う。
    if (depth < 1.0) {
        float4 clip = float4(v.uv.x*2.0-1.0, -(v.uv.y*2.0-1.0), depth, 1.0);
        float4 world = mul(clip, invViewProj);
        world /= max(abs(world.w), 1e-6);
        float sceneDistance = length(world.xyz);
        float tolerance = max(0.05, sceneDistance * 0.001);
        if (cloudHit.x >= sceneDistance - tolerance) discard;
    }
    return cloud;
}
)";

// Atmosphere-aware companion composite. The source cloud texture stores
// straight radiance + alpha, while the atmosphere volumes store premultiplied
// in-scatter L and wavelength-dependent transmittance T. Transforming the
// straight cloud radiance before its
// normal alpha blend gives:
//   a*(S+T*C) + (1-a)*(S+T*B) = S + T*(a*C+(1-a)*B)
// so foreground medium is neither omitted nor counted twice.
const char* kCloudCompAtmosPS = R"(
#pragma pack_matrix(row_major)
cbuffer CloudCB : register(b0) {
    float4x4 invViewProj;
    float4x4 prevCameraRelativeViewProj;
    float4 camPos;
    float4 prevCamPos;
    float4 sunDir;
    float4 sunCol;
    float4 skyCol;
    float4 params;
    float4 dims;
    float4 temporal;
    float4 layer;
    float4 worldOrigin;
    float4 shadowGrid;
    float4 shadowState;
    float4 groundHorizon;
    float4 cloudFrameTerms;
    float4 cloudLightTangent;
    float4 cloudLightBitangent;
    float4 cloudCoverage;
    float4 cloudCoverageReciprocals;
    float4 cloudShellRayOrigin;
    float4 cloudShellTerms;
    float4 cloudLightingExtinction;
    float4 cloudLightingPhase;
    float4 cloudLightingMulti;
    float4 cloudLightingAmbient;
    float4 cloudLightingGround;
    float4 cloudSunTransmittance;
    float4 cloudSkyZenith;
    float4 cloudMultiPhase;
    float4 cloudRange;
    float4 cloudUpperLayer;
    float4 cloudUpperTerms;
    float4 cloudEvolution;
    float4 cloudWeatherControl;
    float4 cloudShadowUpdate;
    float4 cloudWorldShadowMap;
    // 前フレームの対流位相。合成段階では使わないが、定数バッファの末尾を共通化する。
    float4 cloudPreviousEvolution;
    float4 cloudWorldShadowUpdate;
    float4 cloudLightingHistory;
};
cbuffer CloudAtmosphereCB : register(b1) {
    float4 atmosphereParams; // x=maximum camera-volume distance
};
Texture2D<float4> cloudTex : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float2> cloudDepth : register(t2);
Texture3D<float4> atmosphereVolume : register(t3);
Texture3D<float4> atmosphereTransmittance : register(t4);
SamplerState cloudTex_sampler : register(s0);
SamplerState sceneDepth_sampler : register(s1);
SamplerState cloudDepth_sampler : register(s2);
SamplerState atmosphereVolume_sampler : register(s3);
SamplerState atmosphereTransmittance_sampler : register(s4);
struct VSOut { float4 pos:SV_POSITION; float2 uv:TEXCOORD0; float4 farPoint:TEXCOORD1; };
float3 CloudCompositeViewDirection(float4 farHomogeneous) {
    bool perspective=abs(invViewProj[2][3])>1.0e-7;
    float3 candidate=perspective
        ?farHomogeneous.xyz
        :mul(float4(0.0,0.0,1.0,0.0),invViewProj).xyz;
    float lengthSquared=dot(candidate,candidate);
    return lengthSquared>1.0e-12&&lengthSquared<3.0e38
        ?candidate*rsqrt(lengthSquared):float3(0.0,0.0,1.0);
}
static const float CLOUD_PLANET_RADIUS=6360000.0;
float CloudCameraAltitude(){
    float3 local=camPos.xyz-worldOrigin.xyz;
    float radialY=max(CLOUD_PLANET_RADIUS+local.y,1.0);
    float radialXzSquared=dot(local.xz,local.xz);
    float q=radialXzSquared/radialY;
    return local.y+q*(0.5-q/radialY*0.125);
}
bool CloudCameraInsideCloudLayer(){
    float cameraAltitude=CloudCameraAltitude();
    bool insideLower=cameraAltitude>=layer.x&&cameraAltitude<layer.y;
    bool insideUpper=cloudUpperLayer.w>0.5&&
        cameraAltitude>=cloudUpperLayer.x&&cameraAltitude<cloudUpperLayer.y;
    return insideLower||insideUpper;
}
// 雲層内では雲の代表深度を、雲より手前の大気区間として再利用しない。
// その区間は既に雲の積分へ含まれているため、雲中からは大気の先行距離を0にする。
float CloudCompositeAtmosphereDistance(float cloudDistance){
    return CloudCameraInsideCloudLayer()?0.0:max(cloudDistance,0.0);
}
// 地上視点でだけ地面の地平線より下を除外する。雲層内または雲層より上では、
// 下向きの視線が雲を通過して地面へ向かうため、地平線除外を適用しない。
bool CloudCameraBelowCloudBase(){
    return CloudCameraAltitude()<layer.x;
}
// 履歴へ混ぜず、表示する全解像度画素で地平線被覆を一度だけ求める。
float CloudGroundCoverage(VSOut v) {
    float result=1.0;
    bool cameraBelowCloudBase=CloudCameraBelowCloudBase();
    if(groundHorizon.w>=-1.0&&cameraBelowCloudBase) {
        uint2 fullSize=max(uint2(dims.zw),uint2(1,1));
        uint2 pixel=min(uint2(v.pos.xy),fullSize-1u);
        float centerElevation=dot(
            CloudCompositeViewDirection(v.farPoint),groundHorizon.xyz);
        if(centerElevation<groundHorizon.w-0.02) {
            result=0.0;
        } else if(centerElevation<groundHorizon.w+0.02) {
            float xOffset=pixel.x+1u<fullSize.x?1.0:-1.0;
            float yOffset=pixel.y+1u<fullSize.y?1.0:-1.0;
            // 同次遠点は画面座標に対して線形なので、行列の対応行だけで隣接画素へ進める。
            float4 xFarP=v.farPoint+xOffset*(2.0/dims.z)*invViewProj[0];
            float4 yFarP=v.farPoint-yOffset*(2.0/dims.w)*invViewProj[1];
            float xElevation=dot(CloudCompositeViewDirection(xFarP),groundHorizon.xyz);
            float yElevation=dot(CloudCompositeViewDirection(yFarP),groundHorizon.xyz);
            float coverageHalfWidth=max(0.5*(abs(xElevation-centerElevation)+abs(yElevation-centerElevation)),1e-6);
            result=smoothstep(groundHorizon.w-coverageHalfWidth,groundHorizon.w+coverageHalfWidth,centerElevation);
        }
    }
    return result;
}
float4 PSMainAtmos(VSOut v):SV_TARGET {
    float4 cloud = cloudTex.SampleLevel(cloudTex_sampler, v.uv, 0.0);
    float2 cloudHit = cloudDepth.SampleLevel(cloudDepth_sampler, v.uv, 0.0);
    if (cloud.a < 0.001 || cloudHit.y < 0.001 || cloudHit.x > 250000.0)
        return float4(0,0,0,0);
    float groundCoverage=CloudGroundCoverage(v);
    cloud.a*=groundCoverage;
    cloudHit.y*=groundCoverage;
    bool cloudCompositeFinite=all(cloud==cloud)
        &&all(abs(cloud)<=65504.0)
        &&all(cloudHit==cloudHit)
        &&all(abs(cloudHit)<=250001.0)
        &&groundCoverage==groundCoverage;
    if(!cloudCompositeFinite)
        return float4(0,0,0,0);
    if (cloud.a < 0.001)
        return float4(0,0,0,0);

    float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0);
    if (depth < 1.0) {
        float4 clip = float4(v.uv.x*2.0-1.0, -(v.uv.y*2.0-1.0), depth, 1.0);
        float4 world = mul(clip, invViewProj);
        world /= max(abs(world.w), 1e-6);
        float sceneDistance = length(world.xyz);
        float tolerance = max(0.05, sceneDistance * 0.001);
        if (cloudHit.x >= sceneDistance - tolerance) discard;
    }

    float maxDistance = max(atmosphereParams.x, 1e-3);
    float atmosphereDistance=CloudCompositeAtmosphereDistance(cloudHit.x);
    float slice = sqrt(saturate(atmosphereDistance / maxDistance));
    float4 inScatterSample = atmosphereVolume.SampleLevel(
        atmosphereVolume_sampler, float3(v.uv, slice), 0.0);
    float4 transmittanceSample = atmosphereTransmittance.SampleLevel(
        atmosphereTransmittance_sampler, float3(v.uv, slice), 0.0);
    // The physical AP dispatch writes transmittance alpha=1 for every valid
    // froxel. An unbound, stale or partially written u1 commonly samples zero;
    // fail open to identity transfer instead of multiplying cloud radiance by
    // black. Comparisons also reject NaN without relying on HLSL isfinite
    // availability across FXC/DXC backends.
    bool transmittanceValid =
        transmittanceSample.a == transmittanceSample.a &&
        transmittanceSample.a >= 0.5 &&
        transmittanceSample.a <= 1.01 &&
        all(transmittanceSample.rgb == transmittanceSample.rgb) &&
        all(transmittanceSample.rgb >= 0.0) &&
        all(transmittanceSample.rgb <= 1.001);
    bool inScatterValid =
        all(inScatterSample.rgb == inScatterSample.rgb) &&
        all(inScatterSample.rgb >= 0.0) &&
        all(inScatterSample.rgb <= 65504.0);
    float3 inScatter =
        inScatterValid && transmittanceValid
            ? max(inScatterSample.rgb, 0.0) : float3(0.0,0.0,0.0);
    float3 transmittance =
        transmittanceValid
            ? saturate(transmittanceSample.rgb) : float3(1.0,1.0,1.0);
    cloud.rgb = inScatter + transmittance * cloud.rgb;
    return cloud;
}
)";

// 表示用の雲なしcubemapと、同じvolumeを6方向へ積分した雲を合成する。
// 出力はIBL convolution専用であり、画面へ描くEnvCubemap自体は変更しない。
const char* kCloudEnvironmentCompositeHLSL = R"(
cbuffer CloudEnvironmentCompositeCB : register(b0) {
    int faceIndex;
    float3 padding;
};
TextureCube<float4> baseEnvironment : register(t0);
Texture2D<float4> cloudEnvironment : register(t1);
SamplerState baseEnvironment_sampler : register(s0);
SamplerState cloudEnvironment_sampler : register(s1);
struct VSOut {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};
VSOut VSMain(uint id : SV_VertexID) {
    float2 uv=float2((id<<1)&2,id&2);
    VSOut output;
    output.uv=uv;
    output.position=float4(uv.x*2.0-1.0,-(uv.y*2.0-1.0),0.0,1.0);
    return output;
}
float3 CubeFaceDirection(float2 uv,int face) {
    float2 m=uv*2.0-1.0;
    float3 direction=float3(-m.x,-m.y,-1.0);
    if(face==0) direction=float3(1.0,-m.y,-m.x);
    else if(face==1) direction=float3(-1.0,-m.y,m.x);
    else if(face==2) direction=float3(m.x,1.0,m.y);
    else if(face==3) direction=float3(m.x,-1.0,-m.y);
    else if(face==4) direction=float3(m.x,-m.y,1.0);
    return normalize(direction);
}
float4 PSMain(VSOut input) : SV_TARGET {
    float3 base=max(baseEnvironment.SampleLevel(
        baseEnvironment_sampler,
        CubeFaceDirection(input.uv,faceIndex),0.0).rgb,0.0);
    float4 cloud=cloudEnvironment.SampleLevel(
        cloudEnvironment_sampler,input.uv,0.0);
    bool valid=cloud.a==cloud.a && all(cloud.rgb==cloud.rgb)
        && cloud.a>=0.0 && cloud.a<=1.001
        && all(cloud.rgb>=0.0) && all(cloud.rgb<=65504.0);
    if(!valid) cloud=float4(0.0,0.0,0.0,0.0);
    return float4(lerp(base,max(cloud.rgb,0.0),saturate(cloud.a)),1.0);
}
)";

constexpr u32 kVolumetricCloudEnvironmentCubeSize = 64u;
constexpr u32 kVolumetricCloudEnvironmentViewSteps = 64u;
// 64x6方向のray marchとIBL畳み込みは高価なので、連続補間中は最大でも
// この成功雲frame数ごとに一度だけ更新する。30は60 Hz時に約0.5秒。
constexpr u64 kVolumetricCloudEnvironmentRefreshInterval = 30u;

// 連続補間の微小差を同じ署名へ丸める間隔。設定の大きな変更は署名を変え、
// 実際の再生成頻度は上の固定frame間隔でも制限する。
constexpr f32 kCloudEnvironmentCoverageSignatureStep = 1.0f / 32.0f;
constexpr f32 kCloudEnvironmentDensitySignatureStep = 1.0f / 16.0f;
constexpr f32 kCloudEnvironmentWindSignatureStep = 1.0f / 8.0f;
constexpr f32 kCloudEnvironmentRadianceSignatureStep = 1.0f / 64.0f;
// 雲層へ進入または退出したときだけ環境光を更新し、微小な上下動では再生成しない。
constexpr f32 kCloudEnvironmentViewDistanceSignatureStep = 250.0f;

struct FCloudEnvironmentCompositeCb {
    i32 face_index = 0;
    f32 padding[3]{};
};
static_assert(
    sizeof(FCloudEnvironmentCompositeCb) == 16u,
    "Cloud environment composite CB must match HLSL");

struct FCloudCb {
    FMat4 invViewProj;
    FMat4 prevCameraRelativeViewProj;
    FVec4 camPos;
    FVec4 prevCamPos;
    FVec4 sunDir;
    FVec4 sunCol;
    FVec4 skyCol;
    FVec4 params;
    FVec4 dims;
    FVec4 temporal;
    FVec4 layer;
    FVec4 worldOrigin;
    FVec4 shadowGrid;
    FVec4 shadowState;
    FVec4 groundHorizon;
    FVec4 cloudFrameTerms;
    FVec4 cloudLightTangent;
    FVec4 cloudLightBitangent;
    FVec4 cloudCoverage;
    FVec4 cloudCoverageReciprocals;
    FVec4 cloudShellRayOrigin;
    FVec4 cloudShellTerms;
    FVec4 cloudLightingExtinction;
    FVec4 cloudLightingPhase;
    FVec4 cloudLightingMulti;
    FVec4 cloudLightingAmbient;
    FVec4 cloudLightingGround;
    FVec4 cloudSunTransmittance;
    FVec4 cloudSkyZenith;
    FVec4 cloudMultiPhase;
    FVec4 cloudRange;
    FVec4 cloudUpperLayer;
    FVec4 cloudUpperTerms;
    FVec4 cloudEvolution;
    FVec4 cloudWeatherControl;
    FVec4 cloudShadowUpdate;
    FVec4 cloudWorldShadowMap;
    FVec4 cloudPreviousEvolution;
    FVec4 cloudWorldShadowUpdate;
    FVec4 cloudLightingHistory;
};
static_assert(sizeof(FCloudCb) == 736, "CloudCB must match the HLSL layout");
static_assert(
    offsetof(FCloudCb, groundHorizon) == 320u,
    "CloudCB ground horizon must remain at HLSL register c20");
static_assert(
    offsetof(FCloudCb, groundHorizon) % 16u == 0u,
    "CloudCB fields must remain on float4 register boundaries");
static_assert(
    offsetof(FCloudCb, cloudFrameTerms) == 336u,
    "CloudCB density frame terms must remain at HLSL register c21");
static_assert(
    offsetof(FCloudCb, cloudLightTangent) == 352u &&
        offsetof(FCloudCb, cloudLightBitangent) == 368u,
    "CloudCB light basis must remain at HLSL registers c22-c23");
static_assert(
    offsetof(FCloudCb, cloudCoverage) == 384u,
    "CloudCB coverage terms must remain at HLSL register c24");
static_assert(
    offsetof(FCloudCb, cloudCoverageReciprocals) == 400u,
    "CloudCB coverage reciprocals must remain at HLSL register c25");
static_assert(
    offsetof(FCloudCb, cloudShellRayOrigin) == 416u,
    "CloudCB shell ray origin must remain at HLSL register c26");
static_assert(
    offsetof(FCloudCb, cloudShellTerms) == 432u,
    "CloudCB shell terms must remain at HLSL register c27");
static_assert(
    offsetof(FCloudCb, cloudLightingExtinction) == 448u,
    "CloudCB lighting terms must remain at HLSL register c28");
static_assert(
    offsetof(FCloudCb, cloudEvolution) == 624u,
    "CloudCB の時間変化項は HLSL の c39 と一致させる");
static_assert(offsetof(FCloudCb, cloudWeatherControl) == 640u, "CloudCB の天候制御項は HLSL の c40 と一致させる");
static_assert(offsetof(FCloudCb, cloudShadowUpdate) == 656u, "CloudCB の自己影更新項は HLSL の c41 と一致させる");
static_assert(offsetof(FCloudCb, cloudWorldShadowMap) == 672u, "CloudCB の立体物用雲影座標は HLSL の c42 と一致させる");
static_assert(offsetof(FCloudCb, cloudPreviousEvolution) == 688u, "CloudCB の前フレーム対流位相は HLSL の c43 と一致させる");
static_assert(offsetof(FCloudCb, cloudWorldShadowUpdate) == 704u, "CloudCB の立体物影更新項は HLSL の c44 と一致させる");
static_assert(offsetof(FCloudCb, cloudLightingHistory) == 720u, "CloudCB の照明履歴更新項は HLSL の c45 と一致させる");
static_assert(
    CBSize<FCloudCb>() == 768u,
    "CloudCB allocation must preserve DX12's 256-byte alignment");

/** 画面描画と環境キューブマップで共有する密度採取項。 */
struct FCloudSamplingTerms {
    /** xy=占有用と実密度用の天候しきい値、zw=予約。 */
    FVec4 coverage{};

    /** xy=天候遷移幅の逆数、z=視線の細密刻み、w=予約。 */
    FVec4 coverageReciprocals{};

    /** xy=上層の被覆と濃さ、z=1 m当たりの基準消散、w=予約。 */
    FVec4 upperTerms{};
};

/**
 * 同じ密度シェーダーを使う全経路へ、同一の被覆・採取尺度を渡す。
 *
 * @param safeCoverage 0～1へ正規化済みの被覆。
 * @param horizontalNoiseScale 下層の水平方向ノイズ尺度。
 * @param upperLayer 正規化済みの上層設定。
 * @return 定数バッファーへそのまま設定できる共有項。
 */
FCloudSamplingTerms ResolveVolumetricCloudSamplingTerms_Internal(
    f32 safeCoverage,f32 horizontalNoiseScale,
    const FVolumetricCloudUpperLayer& upperLayer) noexcept {
    // 呼び出し側が定数バッファーへ複製せず設定できる採取項。
    FCloudSamplingTerms out{};
    // 空領域の早期棄却では、実密度より少し広い範囲を残す。
    const f32 occupancyCoverage = safeCoverage + 0.08f < 1.0f ? safeCoverage + 0.08f : 1.0f;
    // 雲量は天候被覆だけへ適用する。符号付き3D場の値域は雲量で再正規化しない。
    out.coverage = FVec4{
        0.72f - 0.36f * occupancyCoverage,
        0.72f - 0.36f * safeCoverage,
        0.0f,0.0f};
    // smoothstepの上端を求め、狭い遷移でも逆数が有限になる範囲へ収める。
    const f32 occupancyWeatherUpper =
        out.coverage.x + 0.14f < 0.98f
            ? out.coverage.x + 0.14f : 0.98f;
    const f32 densityWeatherUpper =
        out.coverage.y + 0.14f < 0.98f
            ? out.coverage.y + 0.14f : 0.98f;
    // ノイズ尺度に応じた細密刻みを、過剰採取と標本不足の両方を避ける範囲へ収める。
    const f32 unclampedFineStep = 0.035f / (horizontalNoiseScale > 0.001f ? horizontalNoiseScale : 0.001f);
    const f32 fineStep = unclampedFineStep < 0.5f ? 0.5f : (unclampedFineStep > 2.0f ? 2.0f : unclampedFineStep);
    out.coverageReciprocals = FVec4{
        1.0f / (occupancyWeatherUpper - out.coverage.x),
        1.0f / (densityWeatherUpper - out.coverage.y),fineStep,0.0f};
    out.upperTerms = FVec4{
        upperLayer.coverage_scale,upperLayer.density_scale,
        kVolumetricCloudReferenceExtinctionPerMeter,0.0f};
    return out;
}

struct FCloudAtmosphereCb {
    FVec4 atmosphereParams;
};

/** cubemap faceとIBL側のD3D規約を一致させた、カメラ相対の逆view-projectionを返す。 */
FMat4 CloudEnvironmentInverseViewProjection(
        u32 face, f32 far_distance) noexcept {
    static constexpr FVec3 kDirections[6] = {
        FVec3{1.0f, 0.0f, 0.0f},
        FVec3{-1.0f, 0.0f, 0.0f},
        FVec3{0.0f, 1.0f, 0.0f},
        FVec3{0.0f, -1.0f, 0.0f},
        FVec3{0.0f, 0.0f, 1.0f},
        FVec3{0.0f, 0.0f, -1.0f}};
    static constexpr FVec3 kUp[6] = {
        FVec3{0.0f, 1.0f, 0.0f},
        FVec3{0.0f, 1.0f, 0.0f},
        FVec3{0.0f, 0.0f, -1.0f},
        FVec3{0.0f, 0.0f, 1.0f},
        FVec3{0.0f, 1.0f, 0.0f},
        FVec3{0.0f, 1.0f, 0.0f}};
    const u32 safe_face = face < 6u ? face : 5u;
    CCamera camera;
    camera.SetPerspective(
        kPi * 0.5f, 1.0f, 0.5f,
        far_distance > 1.0f ? far_distance : 1.0f);
    camera.SetLookAt(
        FVec3{}, kDirections[safe_face], kUp[safe_face]);
    return Inverse(camera.ViewProjection());
}

/** FNV-1aで設定値を順序付き32bit署名へ混ぜる。 */
u32 HashCloudEnvironmentWord(u32 hash, u32 value) noexcept {
    hash ^= value;
    hash *= 16777619u;
    return hash;
}

/** 有限化済みfloatの表現を署名へ混ぜる。 */
u32 HashCloudEnvironmentFloat(u32 hash, f32 value) noexcept {
    u32 bits = 0u;
    static_assert(sizeof(bits) == sizeof(value), "float hash size mismatch");
    ::memcpy(&bits, &value, sizeof(bits));
    return HashCloudEnvironmentWord(hash, bits);
}

/** 有限かつ有界な値を指定間隔へ丸め、連続補間の微小差を署名から除く。 */
u32 HashCloudEnvironmentQuantizedFloat(u32 hash, f32 value, f32 step) noexcept {
    const i32 bucket = static_cast<i32>(Floor(value / step + 0.5f));
    return HashCloudEnvironmentWord(hash, static_cast<u32>(bucket));
}

bool IsFiniteCloudVector(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

/** 曲面惑星の局所原点から測った位置を、GPUと同じ近似式で高度へ変換する。 */
f32 CloudAltitudeFromLocalPosition(FVec3 local_position) noexcept
{
    if (!IsFiniteCloudVector(local_position)) return 0.0f;
    const f32 unboundedRadialY =
        kVolumetricCloudPlanetRadius + local_position.y;
    const f32 radialY = unboundedRadialY > 1.0f ? unboundedRadialY : 1.0f;
    const f32 radialXzSquared =
        local_position.x * local_position.x +
        local_position.z * local_position.z;
    const f32 q = radialXzSquared / radialY;
    return local_position.y + q * (0.5f - q / (8.0f * radialY));
}

/** 下層と上層をまとめて、現在高度で使う最短の雲描画距離を求める。 */
f32 ResolveVolumetricCloudViewDistance_Internal(FVec3 shell_local_origin, const FVolumetricCloudLayer& lower_layer, const FVolumetricCloudUpperLayer& upper_layer, bool has_upper_layer, f32 maximum_distance) noexcept
{
    const f32 camera_altitude =
        CloudAltitudeFromLocalPosition(shell_local_origin);
    f32 current_view_distance = EvaluateVolumetricCloudInteriorViewDistance(camera_altitude, lower_layer.base_height, lower_layer.top_height, maximum_distance);
    if (has_upper_layer) {
        const f32 upper_view_distance = EvaluateVolumetricCloudInteriorViewDistance(camera_altitude, upper_layer.base_height, upper_layer.top_height, maximum_distance);
        if (upper_view_distance < current_view_distance) {
            current_view_distance = upper_view_distance;
        }
    }
    return current_view_distance;
}

bool IsFiniteCloudMatrix(const FMat4& value) noexcept {
    for (u32 row = 0; row < 4u; ++row) {
        for (u32 column = 0; column < 4u; ++column) {
            if (!std::isfinite(value.m[row][column])) return false;
        }
    }
    return true;
}

bool UnprojectCloudViewDirection(const FMat4& inverse_view_projection,
                                 f32 ndc_x, f32 ndc_y,
                                 FVec3& direction) noexcept {
    return TryBuildCameraRelativeViewDirection(
        inverse_view_projection, ndc_x, ndc_y, direction);
}

FVec3 SanitizeCloudRadiance(FVec3 value, FVec3 fallback) noexcept {
    constexpr f32 kMaximumCloudRadiance = 16384.0f;
    auto sanitize = [kMaximumCloudRadiance](
                        f32 component, f32 fallbackComponent) noexcept {
        f32 result = std::isfinite(component) ? component : fallbackComponent;
        if (!std::isfinite(result) || result < 0.0f) result = 0.0f;
        if (result > kMaximumCloudRadiance) result = kMaximumCloudRadiance;
        return result;
    };
    return FVec3{sanitize(value.x, fallback.x),
                 sanitize(value.y, fallback.y),
                 sanitize(value.z, fallback.z)};
}

/** 浮動小数設定を有限値へ戻し、指定範囲へ収める。 */
f32 SanitizeCloudScalar(f32 value, f32 fallback, f32 minimum, f32 maximum) noexcept
{
    const f32 finiteValue = std::isfinite(value) ? value : fallback;
    return Clamp(finiteValue, minimum, maximum);
}

/** 透過率などの RGB 比率を成分ごとに 0..1 へ収める。 */
FVec3 SanitizeCloudUnitColor(FVec3 value, FVec3 fallback) noexcept
{
    return FVec3{SanitizeCloudScalar(value.x, fallback.x, 0.0f, 1.0f),
                 SanitizeCloudScalar(value.y, fallback.y, 0.0f, 1.0f),
                 SanitizeCloudScalar(value.z, fallback.z, 0.0f, 1.0f)};
}

/** 下層設定が成分単位で同じか返す。 */
bool CloudLayerEqual(const FVolumetricCloudLayer& lhs, const FVolumetricCloudLayer& rhs) noexcept
{
    return lhs.base_height == rhs.base_height && lhs.top_height == rhs.top_height &&
           lhs.horizontal_noise_scale == rhs.horizontal_noise_scale;
}

/** 照明設定が成分単位で同じか返す。 */
bool CloudLightingEqual(const FVolumetricCloudLighting& lhs, const FVolumetricCloudLighting& rhs) noexcept
{
    return lhs.ViewExtinction == rhs.ViewExtinction && lhs.LightExtinction == rhs.LightExtinction &&
           lhs.SunScatter == rhs.SunScatter &&
           lhs.SunScatteringLuminanceScale == rhs.SunScatteringLuminanceScale &&
           lhs.PowderStrength == rhs.PowderStrength &&
           lhs.PhaseForward == rhs.PhaseForward && lhs.PhaseBackward == rhs.PhaseBackward &&
           lhs.PhaseBlend == rhs.PhaseBlend && lhs.PhaseMin == rhs.PhaseMin && lhs.PhaseMax == rhs.PhaseMax &&
           lhs.MultiScatterContribution == rhs.MultiScatterContribution &&
           lhs.MultiScatterOcclusion == rhs.MultiScatterOcclusion && lhs.SkyZenithColor.x == rhs.SkyZenithColor.x &&
           lhs.SkyZenithColor.y == rhs.SkyZenithColor.y && lhs.SkyZenithColor.z == rhs.SkyZenithColor.z &&
           lhs.MultiScatterEccentricity == rhs.MultiScatterEccentricity && lhs.AmbientAtBase == rhs.AmbientAtBase &&
           lhs.AmbientAtTop == rhs.AmbientAtTop && lhs.GroundContribution == rhs.GroundContribution &&
           lhs.SunTransmittance.x == rhs.SunTransmittance.x && lhs.SunTransmittance.y == rhs.SunTransmittance.y &&
           lhs.SunTransmittance.z == rhs.SunTransmittance.z && lhs.GroundColor.x == rhs.GroundColor.x &&
           lhs.GroundColor.y == rhs.GroundColor.y && lhs.GroundColor.z == rhs.GroundColor.z;
}

/** 影キャッシュへ焼き込む光学係数が同じか返す。放射輝度と位相は採取時に適用する。 */
bool CloudLightingShadowTransportEqual_Internal(
    const FVolumetricCloudLighting& lhs,
    const FVolumetricCloudLighting& rhs) noexcept
{
    return lhs.LightExtinction == rhs.LightExtinction &&
           lhs.MultiScatterOcclusion == rhs.MultiScatterOcclusion;
}

/** 照明履歴を再利用できる、放射輝度以外の設定が同じか返す。 */
bool CloudLightingHistoryTransportEqual_Internal(
    const FVolumetricCloudLighting& lhs,
    const FVolumetricCloudLighting& rhs) noexcept
{
    return lhs.ViewExtinction == rhs.ViewExtinction &&
           lhs.LightExtinction == rhs.LightExtinction &&
           lhs.SunScatter == rhs.SunScatter &&
           lhs.PowderStrength == rhs.PowderStrength &&
           lhs.PhaseForward == rhs.PhaseForward &&
           lhs.PhaseBackward == rhs.PhaseBackward &&
           lhs.PhaseBlend == rhs.PhaseBlend &&
           lhs.PhaseMin == rhs.PhaseMin &&
           lhs.PhaseMax == rhs.PhaseMax &&
           lhs.MultiScatterContribution == rhs.MultiScatterContribution &&
           lhs.MultiScatterOcclusion == rhs.MultiScatterOcclusion &&
           lhs.MultiScatterEccentricity == rhs.MultiScatterEccentricity &&
           lhs.AmbientAtBase == rhs.AmbientAtBase &&
           lhs.AmbientAtTop == rhs.AmbientAtTop &&
           lhs.GroundContribution == rhs.GroundContribution &&
           lhs.SunScatteringLuminanceScale ==
               rhs.SunScatteringLuminanceScale;
}

/** 天候設定が成分単位で同じか返す。 */
bool CloudWeatherEqual(const FVolumetricCloudWeather& lhs, const FVolumetricCloudWeather& rhs) noexcept
{
    return lhs.CloudType == rhs.CloudType && lhs.CloudTypeInfluence == rhs.CloudTypeInfluence &&
           lhs.Precipitation == rhs.Precipitation &&
           lhs.PrecipitationInfluence == rhs.PrecipitationInfluence;
}

/** 距離設定が成分単位で同じか返す。 */
bool CloudRangeEqual(const FVolumetricCloudRange& lhs, const FVolumetricCloudRange& rhs) noexcept
{
    return lhs.MaxDistance == rhs.MaxDistance && lhs.FadeFraction == rhs.FadeFraction &&
           lhs.StepGrowth == rhs.StepGrowth && lhs.ViewSteps == rhs.ViewSteps;
}

/** 上層設定が成分単位で同じか返す。 */
bool CloudUpperLayerEqual(const FVolumetricCloudUpperLayer& lhs, const FVolumetricCloudUpperLayer& rhs) noexcept
{
    return lhs.base_height == rhs.base_height && lhs.top_height == rhs.top_height &&
           lhs.coverage_scale == rhs.coverage_scale && lhs.density_scale == rhs.density_scale;
}

} // namespace

FVolumetricCloudLayer SanitizeVolumetricCloudLayer(const FVolumetricCloudLayer& requested) noexcept
{
    const FVolumetricCloudLayer defaults{};
    FVolumetricCloudLayer layer{};
    layer.base_height = SanitizeCloudScalar(requested.base_height, defaults.base_height, -kVolumetricCloudMaxDistance,
                                            kVolumetricCloudMaxDistance);
    layer.top_height = SanitizeCloudScalar(requested.top_height, defaults.top_height, -kVolumetricCloudMaxDistance,
                                           kVolumetricCloudMaxDistance);
    layer.horizontal_noise_scale = SanitizeCloudScalar(requested.horizontal_noise_scale,
                                                       defaults.horizontal_noise_scale, 0.001f, 1.0f);

    if (layer.top_height < layer.base_height) {
        const f32 swap = layer.base_height;
        layer.base_height = layer.top_height;
        layer.top_height = swap;
    }
    const f32 thickness = layer.top_height - layer.base_height;
    if (!std::isfinite(thickness) || thickness < kVolumetricCloudMinLayerThickness) {
        const f32 expandedTop = layer.base_height + kVolumetricCloudMinLayerThickness;
        if (std::isfinite(expandedTop) && expandedTop <= kVolumetricCloudMaxDistance) {
            layer.top_height = expandedTop;
        } else {
            layer.base_height = defaults.base_height;
            layer.top_height = defaults.top_height;
        }
    }
    return layer;
}

FVolumetricCloudWeather SanitizeVolumetricCloudWeather(const FVolumetricCloudWeather& requested) noexcept
{
    const FVolumetricCloudWeather defaults{};
    FVolumetricCloudWeather weather{};
    weather.CloudType = SanitizeCloudScalar(requested.CloudType, defaults.CloudType, 0.0f, 1.0f);
    weather.CloudTypeInfluence = SanitizeCloudScalar(
        requested.CloudTypeInfluence, defaults.CloudTypeInfluence, 0.0f, 1.0f);
    weather.Precipitation = SanitizeCloudScalar(
        requested.Precipitation, defaults.Precipitation, 0.0f, 1.0f);
    weather.PrecipitationInfluence = SanitizeCloudScalar(
        requested.PrecipitationInfluence, defaults.PrecipitationInfluence, 0.0f, 1.0f);
    return weather;
}

FVolumetricCloudLighting SanitizeVolumetricCloudLighting(const FVolumetricCloudLighting& requested) noexcept
{
    const FVolumetricCloudLighting defaults{};
    FVolumetricCloudLighting lighting{};
    lighting.ViewExtinction = SanitizeCloudScalar(requested.ViewExtinction, defaults.ViewExtinction, 0.0f,
                                                  kVolumetricCloudMaxExtinction);
    lighting.LightExtinction = SanitizeCloudScalar(requested.LightExtinction, defaults.LightExtinction, 0.0f,
                                                   kVolumetricCloudMaxExtinction);
    lighting.SunScatter = SanitizeCloudScalar(requested.SunScatter, defaults.SunScatter, 0.0f, 1.0f);
    lighting.SunScatteringLuminanceScale = SanitizeCloudScalar(requested.SunScatteringLuminanceScale, defaults.SunScatteringLuminanceScale, 0.0f, kVolumetricCloudMaxSunScatteringLuminanceScale);
    lighting.PowderStrength = SanitizeCloudScalar(requested.PowderStrength, defaults.PowderStrength, 0.0f,
                                                  kVolumetricCloudMaxPowderStrength);
    lighting.PhaseForward = SanitizeCloudScalar(requested.PhaseForward, defaults.PhaseForward,
                                                -kVolumetricCloudMaxPhaseEccentricity,
                                                kVolumetricCloudMaxPhaseEccentricity);
    lighting.PhaseBackward = SanitizeCloudScalar(requested.PhaseBackward, defaults.PhaseBackward,
                                                 -kVolumetricCloudMaxPhaseEccentricity,
                                                 kVolumetricCloudMaxPhaseEccentricity);
    lighting.PhaseBlend = SanitizeCloudScalar(requested.PhaseBlend, defaults.PhaseBlend, 0.0f, 1.0f);
    lighting.PhaseMin = SanitizeCloudScalar(requested.PhaseMin, defaults.PhaseMin, 0.0f,
                                            kVolumetricCloudMaxPhaseValue);
    lighting.PhaseMax = SanitizeCloudScalar(requested.PhaseMax, defaults.PhaseMax, 0.0f,
                                            kVolumetricCloudMaxPhaseValue);
    if (lighting.PhaseMax < lighting.PhaseMin) {
        const f32 swap = lighting.PhaseMin;
        lighting.PhaseMin = lighting.PhaseMax;
        lighting.PhaseMax = swap;
    }
    lighting.MultiScatterOcclusion = SanitizeCloudScalar(requested.MultiScatterOcclusion,
                                                         defaults.MultiScatterOcclusion, 0.0f, 1.0f);
    lighting.MultiScatterContribution = SanitizeCloudScalar(requested.MultiScatterContribution,
                                                            defaults.MultiScatterContribution, 0.0f,
                                                            lighting.MultiScatterOcclusion);
    lighting.SkyZenithColor = SanitizeCloudRadiance(requested.SkyZenithColor, defaults.SkyZenithColor);
    lighting.MultiScatterEccentricity = SanitizeCloudScalar(requested.MultiScatterEccentricity,
                                                            defaults.MultiScatterEccentricity,
                                                            -kVolumetricCloudMaxPhaseEccentricity,
                                                            kVolumetricCloudMaxPhaseEccentricity);
    lighting.AmbientAtBase = SanitizeCloudScalar(requested.AmbientAtBase, defaults.AmbientAtBase, 0.0f, 1.0f);
    lighting.AmbientAtTop = SanitizeCloudScalar(requested.AmbientAtTop, defaults.AmbientAtTop, 0.0f, 1.0f);
    lighting.GroundContribution = SanitizeCloudScalar(requested.GroundContribution, defaults.GroundContribution, 0.0f,
                                                      1.0f);
    lighting.SunTransmittance = SanitizeCloudUnitColor(requested.SunTransmittance, defaults.SunTransmittance);
    lighting.GroundColor = SanitizeCloudRadiance(requested.GroundColor, defaults.GroundColor);
    return lighting;
}

FVec2 EvaluateVolumetricCloudDirectionalScattering(f32 light_optical_depth, f32 single_phase, f32 multiple_phase, const FVolumetricCloudLighting& requested) noexcept
{
    /** GPU と同じ有効範囲へ直した照明設定。 */
    const FVolumetricCloudLighting lighting = SanitizeVolumetricCloudLighting(requested);
    if (!std::isfinite(light_optical_depth)) return FVec2{};
    /** 密度積分から得る非負の光学的な厚さ。 */
    const f32 opticalDepth = light_optical_depth > 0.0f ? light_optical_depth : 0.0f;
    /** 一次散乱へ使う有限で有界な位相値。 */
    const f32 singlePhase = SanitizeCloudScalar(single_phase, 0.0f, lighting.PhaseMin, lighting.PhaseMax);
    /** 二次以降の散乱へ使う有限で有界な位相値。 */
    const f32 multiplePhase = SanitizeCloudScalar(multiple_phase, 0.0f, lighting.PhaseMin, lighting.PhaseMax);
    /** 単散乱アルベドと有限次数の輝度補償を分離した有向光倍率。 */
    const f32 directionalScatteringScale =
        lighting.SunScatter * lighting.SunScatteringLuminanceScale;
    /** 太陽から直接届く一次散乱。 */
    const f32 singleScattering = directionalScatteringScale * Exp(-opticalDepth) * singlePhase;
    /** 二次散乱へ使う散乱係数の縮小率。 */
    const f32 secondContribution = lighting.MultiScatterContribution;
    /** 二次散乱へ使う消散係数の縮小率。 */
    const f32 secondOcclusion = lighting.MultiScatterOcclusion;
    /** 消散を弱めた経路から届く近似二次散乱。 */
    const f32 secondScattering = directionalScatteringScale * secondContribution *
        Exp(-opticalDepth * secondOcclusion) * multiplePhase;
    /** 三次散乱へ使う散乱係数の縮小率。 */
    const f32 thirdContribution = secondContribution * secondContribution;
    /** 三次散乱へ使う消散係数の縮小率。 */
    const f32 thirdOcclusion = secondOcclusion * secondOcclusion;
    /** さらに内部へ回った経路から届く近似三次散乱。 */
    const f32 thirdScattering = directionalScatteringScale * thirdContribution *
        Exp(-opticalDepth * thirdOcclusion) * multiplePhase;
    return FVec2{singleScattering, secondScattering + thirdScattering};
}

f32 EvaluateVolumetricCloudInScatterFactor(f32 low_lod_density, f32 normalized_height, f32 strength) noexcept
{
    /** GPU が参照する低 LOD 密度。 */
    const f32 density = SanitizeCloudScalar(low_lod_density, 0.0f, 0.0f, 1.0f);
    /** 雲層内で正規化した高さ。 */
    const f32 height = SanitizeCloudScalar(normalized_height, 0.0f, 0.0f, 1.0f);
    /** 補正なしと周囲散乱源の確率を混ぜる割合。 */
    const f32 blend = SanitizeCloudScalar(strength, 0.0f, 0.0f, 1.0f);
    /** 雲頂へ近づくほど低密度域を強く抑え、密な領域だけを残す指数。 */
    const f32 depthExponent = 0.5f + 1.5f * Clamp((height - 0.30f) / 0.55f, 0.0f, 1.0f);
    /** 周囲に散乱源が存在する確率。 */
    const f32 depthProbability = Clamp(0.05f + std::pow(density, depthExponent), 0.0f, 1.0f);
    // 雲底の直上にも厚い雲体があるため、高さだけで高次散乱を抑えない。
    // 高さは上の指数に使い、疎な雲頂縁だけを強く抑える。
    /** 補正なしの 1 と有界な周囲散乱源の確率を混ぜた高次散乱係数。 */
    return 1.0f - blend * (1.0f - depthProbability);
}

FVolumetricCloudRange SanitizeVolumetricCloudRange(const FVolumetricCloudRange& requested) noexcept
{
    const FVolumetricCloudRange defaults{};
    FVolumetricCloudRange range{};
    range.MaxDistance = SanitizeCloudScalar(requested.MaxDistance, defaults.MaxDistance, kVolumetricCloudMinDistance,
                                            kVolumetricCloudMaxDistance);
    range.FadeFraction = SanitizeCloudScalar(requested.FadeFraction, defaults.FadeFraction, 0.0f,
                                             kVolumetricCloudMaxFadeFraction);
    range.StepGrowth = SanitizeCloudScalar(requested.StepGrowth, defaults.StepGrowth, 0.0f,
                                           kVolumetricCloudMaxStepGrowth);
    range.ViewSteps = requested.ViewSteps;
    if (range.ViewSteps > 0u && range.ViewSteps < kVolumetricCloudMinViewSteps) {
        range.ViewSteps = kVolumetricCloudMinViewSteps;
    }
    if (range.ViewSteps > kVolumetricCloudMaxViewMarchSamples) {
        range.ViewSteps = kVolumetricCloudMaxViewMarchSamples;
    }
    return range;
}

f32 EvaluateVolumetricCloudDistanceFade(f32 sample_distance, f32 max_distance, f32 fade_fraction) noexcept
{
    const FVolumetricCloudRange defaults{};
    const f32 maximum = SanitizeCloudScalar(max_distance, defaults.MaxDistance, kVolumetricCloudMinDistance,
                                            kVolumetricCloudMaxDistance);
    const f32 fadeFraction = SanitizeCloudScalar(fade_fraction, defaults.FadeFraction, 0.0f,
                                                 kVolumetricCloudMaxFadeFraction);
    if (!(sample_distance == sample_distance) || sample_distance < -kVolumetricCloudMaxDistance ||
        sample_distance > kVolumetricCloudMaxDistance) return 0.0f;
    if (sample_distance <= 0.0f) return 1.0f;
    if (sample_distance >= maximum) return 0.0f;
    if (fadeFraction <= 0.0f) return 1.0f;

    const f32 fadeStart = maximum * (1.0f - fadeFraction);
    f32 blend = (sample_distance - fadeStart) / (maximum - fadeStart);
    if (blend <= 0.0f) return 1.0f;
    if (blend >= 1.0f) return 0.0f;
    blend = blend * blend * (3.0f - 2.0f * blend);
    return 1.0f - blend;
}

f32 EvaluateVolumetricCloudInteriorViewDistance(f32 camera_altitude, f32 layer_base_height, f32 layer_top_height, f32 maximum_distance) noexcept
{
    const FVolumetricCloudRange defaults{};
    const f32 maximum = SanitizeCloudScalar(maximum_distance, defaults.MaxDistance, kVolumetricCloudMinDistance, kVolumetricCloudMaxDistance);
    if (!(camera_altitude >= -kVolumetricCloudMaxDistance && camera_altitude <= kVolumetricCloudMaxDistance) || !(layer_base_height >= -kVolumetricCloudMaxDistance && layer_base_height <= kVolumetricCloudMaxDistance) || !(layer_top_height >= -kVolumetricCloudMaxDistance && layer_top_height <= kVolumetricCloudMaxDistance)) {
        return maximum;
    }
    const f32 thickness = layer_top_height - layer_base_height;
    if (thickness < kVolumetricCloudMinLayerThickness) {
        return maximum;
    }

    // 層内と境界面で使う局所視程。利用側の指定距離を広げない。
    f32 localDistance = Clamp(thickness * kVolumetricCloudInteriorDistanceScale, kVolumetricCloudInteriorMinDistance, kVolumetricCloudInteriorMaxDistance);
    if (localDistance > maximum) localDistance = maximum;
    if (localDistance >= maximum) return maximum;

    // 層内を0とし、層外だけで最寄りの境界面までの距離を測る。
    f32 distanceFromLayer = 0.0f;
    if (camera_altitude < layer_base_height) {
        distanceFromLayer = layer_base_height - camera_altitude;
    } else if (camera_altitude > layer_top_height) {
        distanceFromLayer = camera_altitude - layer_top_height;
    }
    // 層の厚さに比例させた、境界外側の局所視程維持範囲。
    const f32 transitionDistance = thickness * kVolumetricCloudBoundaryTransitionFraction;
    if (distanceFromLayer >= transitionDistance) return maximum;

    // 近接帯の外縁へ進むほど、遠景距離の減衰率へ滑らかに戻す割合。
    f32 maximumWeight = Clamp(distanceFromLayer / transitionDistance, 0.0f, 1.0f);
    maximumWeight = maximumWeight * maximumWeight * (3.0f - 2.0f * maximumWeight);
    const f32 localWeight = 1.0f - maximumWeight;

    // 視程の逆数は媒質の減衰率として加算できる。距離を直接補間するよりも、層境界で
    // 遠景の不透明化が急に現れず、進入と退出で対称な変化になる。
    const f32 inverseMaximum = 1.0f / maximum;
    const f32 inverseLocal = 1.0f / localDistance;
    return 1.0f / (inverseMaximum + (inverseLocal - inverseMaximum) * localWeight);
}

FVolumetricCloudUpperLayer SanitizeVolumetricCloudUpperLayer(const FVolumetricCloudUpperLayer& requested,
                                                             const FVolumetricCloudLayer& lower_layer) noexcept
{
    const FVolumetricCloudUpperLayer defaults{};
    FVolumetricCloudUpperLayer upper{};
    upper.coverage_scale = SanitizeCloudScalar(requested.coverage_scale, defaults.coverage_scale, 0.0f, 1.0f);
    upper.density_scale = SanitizeCloudScalar(requested.density_scale, defaults.density_scale, 0.0f, 1.0f);
    if (!std::isfinite(requested.base_height) || !std::isfinite(requested.top_height)) {
        return upper;
    }

    const FVolumetricCloudLayer lower = SanitizeVolumetricCloudLayer(lower_layer);
    upper.base_height = Clamp(requested.base_height, -kVolumetricCloudMaxDistance, kVolumetricCloudMaxDistance);
    upper.top_height = Clamp(requested.top_height, -kVolumetricCloudMaxDistance, kVolumetricCloudMaxDistance);
    if (upper.top_height <= lower.top_height || upper.top_height <= upper.base_height) {
        upper.base_height = 0.0f;
        upper.top_height = 0.0f;
        return upper;
    }
    if (upper.base_height < lower.top_height) {
        upper.base_height = lower.top_height;
    }
    const f32 thickness = upper.top_height - upper.base_height;
    if (!std::isfinite(thickness) || thickness < kVolumetricCloudMinLayerThickness) {
        const f32 expandedTop = upper.base_height + kVolumetricCloudMinLayerThickness;
        if (std::isfinite(expandedTop) && expandedTop <= kVolumetricCloudMaxDistance) {
            upper.top_height = expandedTop;
        } else {
            upper.base_height = 0.0f;
            upper.top_height = 0.0f;
        }
    }
    return upper;
}

f32 SanitizeVolumetricCloudQualityMultiplier(
    f32 requested_render_scale) noexcept {
    f32 qualityMultiplier = std::isfinite(requested_render_scale)
                          ? requested_render_scale
                          : 1.0f;
    // 上は 4.0 まで許す。1.0 が方策どおりの 1/4 で、4.0 で等倍になる。
    // 以前は 1.0 で頭打ちだったため、余裕のある機械でも 1/4 より細かくできず、
    // 低解像度トレース由来のドット感を消す手段が «参照描画» 以外に無かった。
    if (qualityMultiplier < 0.5f) qualityMultiplier = 0.5f;
    if (qualityMultiplier > static_cast<f32>(kVolumetricCloudUltraTraceDivisor))
        qualityMultiplier = static_cast<f32>(kVolumetricCloudUltraTraceDivisor);
    return qualityMultiplier;
}

FVolumetricCloudTraceResolution ResolveVolumetricCloudTraceResolution(
    u32 full_width, u32 full_height,
    f32 requested_render_scale,
    bool reference_mode) noexcept {
    const u32 fullWidth = full_width > 0u ? full_width : 1u;
    const u32 fullHeight = full_height > 0u ? full_height : 1u;
    const f32 qualityMultiplier =
        SanitizeVolumetricCloudQualityMultiplier(requested_render_scale);

    FVolumetricCloudTraceResolution resolution{};
    static_assert(kVolumetricCloudUltraTraceDivisor > 0u);
    if (reference_mode) {
        // 参照描画は方策を通さず等倍で刻む。見比べるためだけなので速度は捨てる。
        resolution.quality_multiplier = 1.0f;
        resolution.effective_dimension_scale = 1.0f;
        resolution.width = fullWidth > 0u ? fullWidth : 1u;
        resolution.height = fullHeight > 0u ? fullHeight : 1u;
        return resolution;
    }
    resolution.quality_multiplier = qualityMultiplier;
    resolution.effective_dimension_scale =
        qualityMultiplier /
        static_cast<f32>(kVolumetricCloudUltraTraceDivisor);
    resolution.width = static_cast<u32>(std::ceil(
        static_cast<f32>(fullWidth) * resolution.effective_dimension_scale));
    resolution.height = static_cast<u32>(std::ceil(
        static_cast<f32>(fullHeight) * resolution.effective_dimension_scale));
    if (resolution.width == 0u) resolution.width = 1u;
    if (resolution.height == 0u) resolution.height = 1u;
    return resolution;
}

namespace {

constexpr u64 kCloudWorkloadMaximum = ~u64{0};
// キャッシュ境界の最悪経路は、太陽円盤4方向それぞれの適応最大16標本と高周波3標本を使う。
constexpr u64 kCloudMaximumMainLightDensitySamples =
    4u * (static_cast<u64>(kVolumetricCloudMaxLightMarchSamples) + 3u);
u64 SaturatingCloudWorkloadAdd(u64 left, u64 right) noexcept {
    return right > kCloudWorkloadMaximum - left
        ? kCloudWorkloadMaximum : left + right;
}

u64 SaturatingCloudWorkloadMultiply(u64 left, u64 right) noexcept {
    if (left == 0u || right == 0u) return 0u;
    return right > kCloudWorkloadMaximum / left
        ? kCloudWorkloadMaximum : left * right;
}

u64 CloudLogicalInvocations2D(u32 width, u32 height) noexcept {
    return SaturatingCloudWorkloadMultiply(
        static_cast<u64>(width), static_cast<u64>(height));
}

u64 CloudLogicalInvocations3D(
    u32 width, u32 height, u32 depth) noexcept {
    return SaturatingCloudWorkloadMultiply(
        CloudLogicalInvocations2D(width, height),
        static_cast<u64>(depth));
}

u64 CloudRoundedThreadExtent(u32 extent, u32 group_size) noexcept {
    if (extent == 0u || group_size == 0u) return 0u;
    const u64 groups =
        (static_cast<u64>(extent) + group_size - 1u) / group_size;
    return SaturatingCloudWorkloadMultiply(
        groups, static_cast<u64>(group_size));
}

u64 CloudLaunchedThreads2D(
    u32 width, u32 height, u32 group_width, u32 group_height) noexcept {
    return SaturatingCloudWorkloadMultiply(
        CloudRoundedThreadExtent(width, group_width),
        CloudRoundedThreadExtent(height, group_height));
}

u64 CloudLaunchedThreads3D(
    u32 width, u32 height, u32 depth,
    u32 group_width, u32 group_height, u32 group_depth) noexcept {
    return SaturatingCloudWorkloadMultiply(
        CloudLaunchedThreads2D(
            width, height, group_width, group_height),
        CloudRoundedThreadExtent(depth, group_depth));
}

u32 CloudCeilDivisor(u32 value, u32 divisor) noexcept {
    if (divisor == 0u) return 0u;
    return value / divisor + (value % divisor != 0u ? 1u : 0u);
}

/** 公開入力を変えず、実行中だけ必要な準備段階を渡す内部診断値。 */
struct FVolumetricCloudFrameWorkloadInternalOptions {
    /** 画面用の雲と解決処理を数える場合はtrue。 */
    bool render_cloud = true;

    /** 今回数える形状密度場の段階数。公開計画では全四段階、実行時は一段階。 */
    u32 shape_bake_dispatches = 4u;

    /** 立体物影の各軸更新間隔。0なら公開計画の自己影値を共用する。 */
    u32 world_shadow_update_divisor = 0u;
};

FVolumetricCloudFrameWorkload PlanVolumetricCloudFrameWorkload_Internal(
    const FVolumetricCloudFrameWorkloadPlan& plan,
    const FVolumetricCloudFrameWorkloadInternalOptions& options) noexcept {
    FVolumetricCloudFrameWorkload out{};
    out.trace_width = plan.trace_width;
    out.trace_height = plan.trace_height;
    out.output_width = plan.output_width;
    out.output_height = plan.output_height;

    out.trace_logical_invocations = options.render_cloud
        ? CloudLogicalInvocations2D(plan.trace_width, plan.trace_height) : 0u;
    out.resolve_logical_invocations = options.render_cloud
        ? CloudLogicalInvocations2D(plan.output_width, plan.output_height) : 0u;
    if (out.trace_logical_invocations != 0u) {
        out.trace_launched_threads = CloudLaunchedThreads2D(
            plan.trace_width, plan.trace_height, 8u, 8u);
        ++out.steady_dispatches;
    }
    if (out.resolve_logical_invocations != 0u) {
        out.resolve_launched_threads = CloudLaunchedThreads2D(
            plan.output_width, plan.output_height, 8u, 8u);
        ++out.steady_dispatches;
    }

    const auto add_bake_2d =
        [&out](bool enabled, u32 width, u32 height,
               u32 group_width, u32 group_height) noexcept {
            if (!enabled) return;
            ++out.one_time_bake_dispatches;
            out.one_time_bake_logical_invocations =
                SaturatingCloudWorkloadAdd(
                    out.one_time_bake_logical_invocations,
                    CloudLogicalInvocations2D(width, height));
            out.one_time_bake_launched_threads =
                SaturatingCloudWorkloadAdd(
                    out.one_time_bake_launched_threads,
                    CloudLaunchedThreads2D(
                        width, height, group_width, group_height));
        };
    const auto add_bake_3d =
        [&out](bool enabled, u32 width, u32 height, u32 depth,
               u32 group_width, u32 group_height,
               u32 group_depth) noexcept {
            if (!enabled) return;
            ++out.one_time_bake_dispatches;
            out.one_time_bake_logical_invocations =
                SaturatingCloudWorkloadAdd(
                    out.one_time_bake_logical_invocations,
                    CloudLogicalInvocations3D(width, height, depth));
            out.one_time_bake_launched_threads =
                SaturatingCloudWorkloadAdd(
                    out.one_time_bake_launched_threads,
                    CloudLaunchedThreads3D(
                        width, height, depth,
                        group_width, group_height, group_depth));
        };

    // 周波数別完成形状の生成と、X・Y・Zの探索用周期最大値を数える。
    u32 shapeBakeDispatches = options.shape_bake_dispatches;
    if (shapeBakeDispatches > 4u) shapeBakeDispatches = 4u;
    for (u32 dispatchIndex = 0u;
         dispatchIndex < shapeBakeDispatches;
         ++dispatchIndex) {
        add_bake_3d(
            plan.bake_shape_noise,
            128u, 128u, 128u,
            dispatchIndex == 0u ? 4u : 128u,
            dispatchIndex == 0u ? 4u : 1u,
            dispatchIndex == 0u ? 4u : 1u);
    }
    add_bake_2d(plan.bake_weather, 512u, 512u, 8u, 8u);
    add_bake_3d(plan.bake_detail_noise, 64u, 64u, 64u, 4u, 4u, 4u);
    add_bake_2d(plan.bake_curl_noise, 128u, 128u, 8u, 8u);

    const u32 shadowUpdateDivisor = plan.shadow_update_divisor == kVolumetricCloudShadowTemporalDivisor
                                        ? kVolumetricCloudShadowTemporalDivisor
                                        : 1u;
    const u32 worldShadowUpdateDivisor =
        options.world_shadow_update_divisor ==
                kVolumetricCloudShadowTemporalDivisor
            ? kVolumetricCloudShadowTemporalDivisor
            : (options.world_shadow_update_divisor == 0u
                ? shadowUpdateDivisor : 1u);
    const u32 shadowCacheUpdateWidth = CloudCeilDivisor(kVolumetricCloudShadowCacheWidth, shadowUpdateDivisor);
    const u32 shadowCacheUpdateDepth = CloudCeilDivisor(kVolumetricCloudShadowCacheDepth, shadowUpdateDivisor);
    const u32 worldShadowUpdateResolution = CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution, worldShadowUpdateDivisor);

    if (plan.rebuild_shadow_cache) {
        // 一セル16スレッドのうち先頭4本が太陽円盤も担当し、一回の投入で
        // 周囲光と太陽光路を同じテクスチャの別領域へ直接書く。
        out.shadow_cache_dispatches = 1u;
        const u64 shadowColumnCount = CloudLogicalInvocations2D(
            shadowCacheUpdateWidth, shadowCacheUpdateDepth);
        out.shadow_cache_logical_invocations =
            SaturatingCloudWorkloadMultiply(
                shadowColumnCount,
                kVolumetricCloudAmbientCacheQuadratureSamples);
        out.shadow_cache_launched_threads =
            out.shadow_cache_logical_invocations;
    }

    if (plan.rebuild_world_shadow) {
        out.world_shadow_dispatches = 1u;
        out.world_shadow_logical_invocations = CloudLogicalInvocations2D(worldShadowUpdateResolution, worldShadowUpdateResolution);
        out.world_shadow_launched_threads = CloudLaunchedThreads2D(worldShadowUpdateResolution, worldShadowUpdateResolution, 8u, 8u);
    }

    out.total_compute_dispatches =
        out.steady_dispatches +
        out.one_time_bake_dispatches +
        out.shadow_cache_dispatches +
        out.world_shadow_dispatches;
    out.total_logical_invocations = SaturatingCloudWorkloadAdd(SaturatingCloudWorkloadAdd(out.trace_logical_invocations, out.resolve_logical_invocations), SaturatingCloudWorkloadAdd(SaturatingCloudWorkloadAdd(out.one_time_bake_logical_invocations, out.shadow_cache_logical_invocations), out.world_shadow_logical_invocations));
    out.total_launched_threads = SaturatingCloudWorkloadAdd(SaturatingCloudWorkloadAdd(out.trace_launched_threads, out.resolve_launched_threads), SaturatingCloudWorkloadAdd(SaturatingCloudWorkloadAdd(out.one_time_bake_launched_threads, out.shadow_cache_launched_threads), out.world_shadow_launched_threads));
    u32 maximumViewSteps = plan.maximum_view_steps;
    if (maximumViewSteps < kVolumetricCloudMinViewSteps) {
        maximumViewSteps = kVolumetricCloudMinViewSteps;
    }
    if (maximumViewSteps > kVolumetricCloudReferenceViewSteps) {
        maximumViewSteps = kVolumetricCloudReferenceViewSteps;
    }
    const u64 maximumViewCells = SaturatingCloudWorkloadMultiply(
        out.trace_logical_invocations, maximumViewSteps);
    out.maximum_view_samples = SaturatingCloudWorkloadMultiply(
        maximumViewCells,
        render_internal::kVolumetricCloudDensityGaussSampleCount);
    // 密度と照明は四つのGauss点ごとに同じ位置で評価する。セル数だけを
    // 掛けると旧重心一点方式の値になり、実際の最悪光路標本数を四分の一に見積もる。
    out.maximum_light_samples = SaturatingCloudWorkloadMultiply(
        out.maximum_view_samples,
        kCloudMaximumMainLightDensitySamples);
    out.maximum_world_shadow_samples = SaturatingCloudWorkloadMultiply(out.world_shadow_logical_invocations, kVolumetricCloudWorldShadowSamples);
    out.temporal_super_resolution =
        plan.output_width != 0u && plan.output_height != 0u &&
        plan.trace_width == CloudCeilDivisor(
            plan.output_width, kVolumetricCloudUltraTraceDivisor) &&
        plan.trace_height == CloudCeilDivisor(
            plan.output_height, kVolumetricCloudUltraTraceDivisor);
    return out;
}

} // namespace

FVolumetricCloudFrameWorkload PlanVolumetricCloudFrameWorkload(
    const FVolumetricCloudFrameWorkloadPlan& plan) noexcept {
    return PlanVolumetricCloudFrameWorkload_Internal(
        plan, FVolumetricCloudFrameWorkloadInternalOptions{});
}

f32 ResolveVolumetricCloudHorizonCoverage(
    f32 signed_elevation, f32 cutoff,
    f32 elevation_delta_x, f32 elevation_delta_y) noexcept {
    if (!std::isfinite(signed_elevation) || !std::isfinite(cutoff) ||
        !std::isfinite(elevation_delta_x) ||
        !std::isfinite(elevation_delta_y)) {
        return 0.0f;
    }
    f32 halfWidth =
        0.5f * (std::fabs(elevation_delta_x) +
                std::fabs(elevation_delta_y));
    if (halfWidth < 1.0e-6f) halfWidth = 1.0e-6f;
    f32 t = (signed_elevation - (cutoff - halfWidth)) /
            (2.0f * halfWidth);
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

FVolumetricCloudGroundHorizon ResolveVolumetricCloudGroundHorizon(
    FVec3 camera_position, const FVolumetricCloudLayer& layer,
    FVec3 world_origin) noexcept {
    FVolumetricCloudGroundHorizon out{};
    if (!IsFiniteCloudVector(camera_position) ||
        !IsFiniteCloudVector(world_origin) ||
        !std::isfinite(layer.base_height)) {
        return out;
    }

    const FVec3 cameraLocal{
        camera_position.x - world_origin.x,
        camera_position.y - world_origin.y,
        camera_position.z - world_origin.z};
    const FVec3 fromPlanetCenter{
        cameraLocal.x,
        cameraLocal.y + kVolumetricCloudPlanetRadius,
        cameraLocal.z};
    const f32 upLengthSquared =
        fromPlanetCenter.x * fromPlanetCenter.x +
        fromPlanetCenter.y * fromPlanetCenter.y +
        fromPlanetCenter.z * fromPlanetCenter.z;
    if (!std::isfinite(upLengthSquared) ||
        upLengthSquared <= 1.0e-12f) {
        return out;
    }
    out.local_up = NormalizeSafe(fromPlanetCenter);

    const f32 cameraAltitude =
        CloudAltitudeFromLocalPosition(cameraLocal);
    if (!std::isfinite(cameraAltitude) ||
        cameraAltitude >= layer.base_height) {
        return out;
    }

    const f32 observerAltitude =
        cameraAltitude > 0.0f ? cameraAltitude : 0.0f;
    const f32 denominator =
        kVolumetricCloudPlanetRadius + observerAltitude;
    const f32 radiusRatio =
        kVolumetricCloudPlanetRadius / denominator;
    f32 tangentSquared = 1.0f - radiusRatio * radiusRatio;
    if (tangentSquared < 0.0f) tangentSquared = 0.0f;
    if (tangentSquared > 1.0f) tangentSquared = 1.0f;
    const f32 cutoff = -Sqrt(tangentSquared);
    if (std::isfinite(cutoff)) out.ground_cutoff = cutoff;
    return out;
}

f32 ResolveVolumetricCloudAdvectionDistance(
    f32 time, f32 wind_speed) noexcept {
    if (!std::isfinite(time)) time = 0.0f;
    if (!std::isfinite(wind_speed)) wind_speed = 0.0f;
    if (time < -10000000.0f) time = -10000000.0f;
    if (time > 10000000.0f) time = 10000000.0f;
    if (wind_speed < -20.0f) wind_speed = -20.0f;
    if (wind_speed > 20.0f) wind_speed = 20.0f;

    // CloudWind は作者が扱いやすい倍率であり、1.0 を中層雲で一般的な移流速度へ変換する。
    constexpr f32 kWorldUnitsPerSecondAtUnitWind = 12.0f;
    return time * wind_speed * kWorldUnitsPerSecondAtUnitWind;
}

FVec2 VolumetricCloudWindOffsetXZ(f32 wind_offset) noexcept {
    if (!std::isfinite(wind_offset)) wind_offset = 0.0f;
    return FVec2{wind_offset * 0.9284767f,
                 wind_offset * 0.3713907f};
}

FVolumetricCloudDensityFrameTerms ResolveVolumetricCloudDensityFrameTerms(
    const FVolumetricCloudLayer& layer, f32 wind_offset) noexcept {
    FVolumetricCloudDensityFrameTerms out{};
    out.wind_world = VolumetricCloudWindOffsetXZ(wind_offset);

    f32 authoredScale = layer.horizontal_noise_scale;
    if (!std::isfinite(authoredScale)) authoredScale = 0.02f;
    // 128^3形状を約18 km幅へ引き伸ばすと、雲塊の一単位が画面上でぼけて
    // 大きな板に見える。低周波の連結性を保ちつつ、主形状の房が読める尺度へ
    // 詰め、同じ標本数のまま中規模の雲塊を見せる。
    f32 shapeScale = authoredScale * 0.0030f;
    if (shapeScale < 0.00004f) shapeScale = 0.00004f;
    if (shapeScale > 0.00020f) shapeScale = 0.00020f;
    out.shape_scale = shapeScale;

    f32 layerHeight = layer.top_height - layer.base_height;
    if (!std::isfinite(layerHeight) || layerHeight < 1.0e-4f) {
        layerHeight = 1.0e-4f;
    }
    out.inverse_layer_height = 1.0f / layerHeight;
    return out;
}

FVolumetricCloudEvolutionFrameTerms ResolveVolumetricCloudEvolutionFrameTerms(
    f32 time, f32 wind_speed) noexcept {
    FVolumetricCloudEvolutionFrameTerms out{};
    if (!std::isfinite(time)) time = 0.0f;
    if (!std::isfinite(wind_speed)) wind_speed = 0.0f;

    // 極端な入力でも三角関数へ巨大な角度を渡さず、描画側の入力範囲と一致させる。
    if (time < -10000000.0f) time = -10000000.0f;
    if (time > 10000000.0f) time = 10000000.0f;
    f32 windMagnitude = std::fabs(wind_speed);
    if (windMagnitude > 4.0f) windMagnitude = 4.0f;

    // 無風でも対流による変形を残し、強風時だけ穏やかに変化速度を上げる。
    const f64 rateScale = 0.8 + static_cast<f64>(windMagnitude) * 0.2;
    const auto periodicSin = [time, rateScale](f64 angularSpeed) noexcept {
        constexpr f64 kTwoPi = 6.28318530717958647692;
        const f64 angle = std::fmod(
            static_cast<f64>(time) * rateScale * angularSpeed, kTwoPi);
        return static_cast<f32>(std::sin(angle));
    };

    // 約2～6分の互いに割り切れない周期により、短い時間で同じ形へ戻る反復を避ける。
    // 10～30秒でも輪郭変化を判別できる一方、隣接フレームでは微小な変化に留める。
    out.shape_phase = FVec2{
        periodicSin(0.033) * 0.18f,
        periodicSin(0.019) * 0.16f};
    out.fine_phase = FVec2{
        periodicSin(0.057) * 0.11f,
        periodicSin(0.043) * 0.09f};
    return out;
}

FVolumetricCloudLightBasis ResolveVolumetricCloudLightBasis(
    FVec3 sun_direction) noexcept {
    FVolumetricCloudLightBasis out{};
    out.direction = NormalizeSafe(sun_direction);

    const f32 signY = out.direction.y >= 0.0f ? 1.0f : -1.0f;
    const f32 a = -1.0f / (signY + out.direction.y);
    const f32 b = out.direction.x * out.direction.z * a;
    out.tangent = FVec3{
        1.0f + signY * out.direction.x * out.direction.x * a,
        -signY * out.direction.x,
        signY * b};
    out.bitangent = FVec3{
        out.direction.y * out.tangent.z -
            out.direction.z * out.tangent.y,
        out.direction.z * out.tangent.x -
            out.direction.x * out.tangent.z,
        out.direction.x * out.tangent.y -
            out.direction.y * out.tangent.x};
    return out;
}

FVec2 VolumetricCloudMaterialXZ(FVec3 world_position,
                                f32 wind_offset) noexcept {
    const FVec2 wind = VolumetricCloudWindOffsetXZ(wind_offset);
    const f32 x = std::isfinite(world_position.x) ? world_position.x : 0.0f;
    const f32 z = std::isfinite(world_position.z) ? world_position.z : 0.0f;
    return FVec2{x - wind.x, z - wind.y};
}

FVolumetricCloudShadowCacheMapping CenterVolumetricCloudShadowCache(
    FVec2 material_position) noexcept {
    if (!std::isfinite(material_position.x)) material_position.x = 0.0f;
    if (!std::isfinite(material_position.y)) material_position.y = 0.0f;
    const f32 cell = kVolumetricCloudShadowCacheCellSize;
    const f32 halfExtent = kVolumetricCloudShadowCacheExtent * 0.5f;
    FVolumetricCloudShadowCacheMapping out{};
    out.center_material_xz = FVec2{
        std::floor(material_position.x / cell + 0.5f) * cell,
        std::floor(material_position.y / cell + 0.5f) * cell};
    out.min_material_xz = FVec2{
        out.center_material_xz.x - halfExtent,
        out.center_material_xz.y - halfExtent};
    return out;
}

FVec2 ProjectVolumetricCloudWorldShadowReferenceXZ(FVec3 world_position, FVec3 sun_direction, f32 reference_height) noexcept {
    if (!std::isfinite(reference_height)) reference_height = 0.0f;
    const f32 worldX =
        std::isfinite(world_position.x) ? world_position.x : 0.0f;
    const f32 worldY =
        std::isfinite(world_position.y) ? world_position.y : reference_height;
    const f32 worldZ =
        std::isfinite(world_position.z) ? world_position.z : 0.0f;
    const f32 lengthSquared =
        sun_direction.x * sun_direction.x +
        sun_direction.y * sun_direction.y +
        sun_direction.z * sun_direction.z;
    if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
        return FVec2{worldX, worldZ};
    }
    const f32 inverseLength = 1.0f / Sqrt(lengthSquared);
    const FVec3 sun{
        sun_direction.x * inverseLength,
        sun_direction.y * inverseLength,
        sun_direction.z * inverseLength};
    if (sun.y <= kVolumetricCloudWorldShadowMinimumSunY) {
        return FVec2{worldX, worldZ};
    }
    const f32 distanceAlongSun =
        (worldY - reference_height) / sun.y;
    return FVec2{
        worldX - sun.x * distanceAlongSun,
        worldZ - sun.z * distanceAlongSun};
}

FVec2 VolumetricCloudWorldShadowMapMinimum(FVec2 center_reference_xz) noexcept {
    if (!std::isfinite(center_reference_xz.x)) center_reference_xz.x = 0.0f;
    if (!std::isfinite(center_reference_xz.y)) center_reference_xz.y = 0.0f;
    const f32 cell = kVolumetricCloudWorldShadowMapTexelSize;
    const f32 halfExtent = kVolumetricCloudWorldShadowMapExtent * 0.5f;
    const FVec2 snappedCenter{
        std::floor(center_reference_xz.x / cell + 0.5f) * cell,
        std::floor(center_reference_xz.y / cell + 0.5f) * cell};
    return FVec2{
        snappedCenter.x - halfExtent,
        snappedCenter.y - halfExtent};
}

FVec3 RebaseVolumetricCloudWorldOrigin(FVec3 camera_position,
                                       f32 grid_size) noexcept {
    if (!(grid_size >= 1.0f) || grid_size > 65536.0f) {
        grid_size = kVolumetricCloudOriginRebaseGrid;
    }
    const f32 invGrid = 1.0f / grid_size;
    // Keep the tangent origin exactly fixed through the central half of each
    // rebase cell, then ease it to the neighbouring origin through the outer
    // quarters.  Hard rounding jumps the local planet centre by one complete
    // grid cell; at the 2.5 km cloud horizon that visibly moves the profile by
    // several world units.
    //
    // This soft snap remains stationary during normal editor orbits around a
    // cell centre, unlike copying camera XZ every frame, and is C1 continuous
    // at every cell boundary.
    auto soft_snap = [invGrid, grid_size](f32 value) noexcept {
        const f32 scaled = value * invGrid;
        const f32 cell = Floor(scaled);
        const f32 fraction = scaled - cell;
        f32 blend = (fraction - 0.25f) * 2.0f;
        if (blend < 0.0f) blend = 0.0f;
        if (blend > 1.0f) blend = 1.0f;
        blend = blend * blend * (3.0f - 2.0f * blend);
        return (cell + blend) * grid_size;
    };
    const f32 x = soft_snap(camera_position.x);
    const f32 z = soft_snap(camera_position.z);
    return FVec3{x, 0.0f, z};
}

bool VolumetricCloudLightingChanged(
    FVec3 previous_sun_direction, FVec3 previous_sun_color,
    FVec3 previous_sky_color, FVec3 sun_direction,
    FVec3 sun_color, FVec3 sky_color) noexcept {
    return previous_sun_direction.x != sun_direction.x ||
           previous_sun_direction.y != sun_direction.y ||
           previous_sun_direction.z != sun_direction.z ||
           previous_sun_color.x != sun_color.x ||
           previous_sun_color.y != sun_color.y ||
           previous_sun_color.z != sun_color.z ||
           previous_sky_color.x != sky_color.x ||
           previous_sky_color.y != sky_color.y ||
           previous_sky_color.z != sky_color.z;
}

f32 VolumetricCloudLightingTemporalMismatch(
    const FVolumetricCloudLighting& previous_lighting,
    FVec3 previous_sun_direction, FVec3 previous_sun_color,
    FVec3 previous_sky_color, const FVolumetricCloudLighting& lighting,
    FVec3 sun_direction, FVec3 sun_color, FVec3 sky_color) noexcept {
    if (!IsFiniteCloudVector(previous_sun_direction) ||
        !IsFiniteCloudVector(previous_sun_color) ||
        !IsFiniteCloudVector(previous_sky_color) ||
        !IsFiniteCloudVector(previous_lighting.SkyZenithColor) ||
        !IsFiniteCloudVector(previous_lighting.SunTransmittance) ||
        !IsFiniteCloudVector(previous_lighting.GroundColor) ||
        !IsFiniteCloudVector(sun_direction) ||
        !IsFiniteCloudVector(sun_color) ||
        !IsFiniteCloudVector(sky_color) ||
        !IsFiniteCloudVector(lighting.SkyZenithColor) ||
        !IsFiniteCloudVector(lighting.SunTransmittance) ||
        !IsFiniteCloudVector(lighting.GroundColor)) {
        return 1.0f;
    }

    const FVec3 previous_direction = NormalizeSafe(previous_sun_direction);
    const FVec3 current_direction = NormalizeSafe(sun_direction);
    f32 direction_cosine =
        previous_direction.x * current_direction.x +
        previous_direction.y * current_direction.y +
        previous_direction.z * current_direction.z;
    direction_cosine = Clamp(direction_cosine, -1.0f, 1.0f);
    const f32 direction_mismatch =
        Clamp(std::acos(direction_cosine) / 3.14159265358979323846f,
              0.0f, 1.0f);

    const auto component_mismatch = [](f32 previous, f32 current) noexcept {
        const f32 previous_abs = previous < 0.0f ? -previous : previous;
        const f32 current_abs = current < 0.0f ? -current : current;
        const f32 positive_scale =
            previous_abs > current_abs ? previous_abs : current_abs;
        if (positive_scale <= std::numeric_limits<f32>::epsilon()) {
            return 0.0f;
        }
        f32 difference = previous - current;
        if (difference < 0.0f) difference = -difference;
        return Clamp(difference / positive_scale, 0.0f, 1.0f);
    };
    const auto color_mismatch = [&component_mismatch](
                                    FVec3 previous, FVec3 current) noexcept {
        f32 result = component_mismatch(previous.x, current.x);
        const f32 green = component_mismatch(previous.y, current.y);
        const f32 blue = component_mismatch(previous.z, current.z);
        if (green > result) result = green;
        if (blue > result) result = blue;
        return result;
    };

    f32 result = direction_mismatch;
    const f32 mismatches[] = {
        color_mismatch(previous_sun_color, sun_color),
        color_mismatch(previous_sky_color, sky_color),
        color_mismatch(previous_lighting.SunTransmittance,
                       lighting.SunTransmittance),
        color_mismatch(previous_lighting.SkyZenithColor,
                       lighting.SkyZenithColor),
        color_mismatch(previous_lighting.GroundColor,
                       lighting.GroundColor)};
    for (const f32 mismatch : mismatches) {
        if (mismatch > result) result = mismatch;
    }
    return Clamp(result, 0.0f, 1.0f);
}

bool VolumetricCloudViewCutDetected(const FMat4& previous_camera_relative_inv_view_proj, FVec3 previous_camera_position, const FMat4& current_camera_relative_inv_view_proj, FVec3 current_camera_position) noexcept {
    if (!IsFiniteCloudMatrix(previous_camera_relative_inv_view_proj) || !IsFiniteCloudMatrix(current_camera_relative_inv_view_proj) || !IsFiniteCloudVector(previous_camera_position) || !IsFiniteCloudVector(current_camera_position)) {
        return true;
    }

    const f32 deltaX =
        current_camera_position.x - previous_camera_position.x;
    const f32 deltaY =
        current_camera_position.y - previous_camera_position.y;
    const f32 deltaZ =
        current_camera_position.z - previous_camera_position.z;
    constexpr f32 kTeleportDistance = 256.0f;
    if (deltaX * deltaX + deltaY * deltaY + deltaZ * deltaZ >
        kTeleportDistance * kTeleportDistance) {
        return true;
    }

    // 中央と四辺の視線で向きと投影の変化を測り、通常の平行移動は無視する。
    constexpr f32 kRaySamples[5][2] = {
        {0.0f, 0.0f}, {-0.85f, 0.0f}, {0.85f, 0.0f},
        {0.0f, -0.85f}, {0.0f, 0.85f}};
    constexpr f32 kMaximumRayAngleCosine = 0.965925826f; // 15 degrees
    for (const auto& sample : kRaySamples) {
        FVec3 previousDirection{};
        FVec3 currentDirection{};
        if (!UnprojectCloudViewDirection(previous_camera_relative_inv_view_proj, sample[0], sample[1], previousDirection) || !UnprojectCloudViewDirection(current_camera_relative_inv_view_proj, sample[0], sample[1], currentDirection)) {
            return true;
        }
        const f32 directionDot =
            previousDirection.x * currentDirection.x +
            previousDirection.y * currentDirection.y +
            previousDirection.z * currentDirection.z;
        if (!std::isfinite(directionDot) ||
            directionDot < kMaximumRayAngleCosine) {
            return true;
        }
    }
    return false;
}

FVolumetricCloudRayInterval IntersectVolumetricCloudLayer(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer) noexcept {
    FVolumetricCloudRayInterval out{};
    const f32 len2 = ray_direction.x * ray_direction.x +
                     ray_direction.y * ray_direction.y +
                     ray_direction.z * ray_direction.z;
    if (len2 <= 1e-12f) return out;
    const f32 invLen = 1.0f / Sqrt(len2);
    const FVec3 dir{ray_direction.x * invLen, ray_direction.y * invLen,
                    ray_direction.z * invLen};
    if (dir.y > -0.001f && dir.y < 0.001f) return out;

    f32 base = layer.base_height;
    f32 top = layer.top_height;
    if (top < base) {
        const f32 swap = base;
        base = top;
        top = swap;
    }
    if (top - base < 0.01f) top = base + 0.01f;

    const f32 ta = (base - ray_origin.y) / dir.y;
    const f32 tb = (top - ray_origin.y) / dir.y;
    out.enter = ta < tb ? ta : tb;
    out.exit = ta > tb ? ta : tb;
    if (out.enter < 0.0f) out.enter = 0.0f;
    out.hit = out.exit > out.enter;
    if (!out.hit) {
        out.enter = 0.0f;
        out.exit = 0.0f;
    }
    return out;
}

FVolumetricCloudRayInterval IntersectVolumetricCloudShell(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer,
    f32 planet_radius, FVec3 world_origin) noexcept {
    FVolumetricCloudRayInterval out{};
    const f32 len2 = ray_direction.x * ray_direction.x +
                     ray_direction.y * ray_direction.y +
                     ray_direction.z * ray_direction.z;
    if (len2 <= 1e-12f) return out;
    const f32 invLen = 1.0f / Sqrt(len2);
    const FVec3 dir{ray_direction.x * invLen, ray_direction.y * invLen,
                    ray_direction.z * invLen};

    if (planet_radius < 100.0f) planet_radius = 100.0f;
    f32 base = layer.base_height;
    f32 top = layer.top_height;
    if (top < base) {
        const f32 swap = base;
        base = top;
        top = swap;
    }
    if (top - base < 0.01f) top = base + 0.01f;

    const f64 localX =
        static_cast<f64>(ray_origin.x) - static_cast<f64>(world_origin.x);
    const f64 localY =
        static_cast<f64>(ray_origin.y) - static_cast<f64>(world_origin.y);
    const f64 localZ =
        static_cast<f64>(ray_origin.z) - static_cast<f64>(world_origin.z);
    const f64 radius = static_cast<f64>(planet_radius);
    const f64 innerAltitude = static_cast<f64>(base);
    const f64 outerAltitude = static_cast<f64>(top);
    const f64 innerC =
        localX * localX + localZ * localZ +
        (localY - innerAltitude) *
        (2.0 * radius + localY + innerAltitude);
    const f64 outerC =
        localX * localX + localZ * localZ +
        (localY - outerAltitude) *
        (2.0 * radius + localY + outerAltitude);
    const f64 centreDot =
        localX * static_cast<f64>(dir.x) +
        localZ * static_cast<f64>(dir.z) +
        (radius + localY) * static_cast<f64>(dir.y);

    auto roots = [&](f64 altitude, f32& nearT, f32& farT) noexcept {
        const f64 c =
            localX * localX + localZ * localZ +
            (localY - altitude) *
            (2.0 * radius + localY + altitude);
        const f64 disc = centreDot * centreDot - c;
        if (disc < 0.0) {
            nearT = 0.0f;
            farT = 0.0f;
            return false;
        }
        const f64 root = std::sqrt(disc > 0.0 ? disc : 0.0);
        const f64 q = -centreDot -
            (centreDot >= 0.0 ? root : -root);
        const f64 rootA = q;
        const f64 rootB = std::fabs(q) > 1e-12
            ? c / q
            : -centreDot + root;
        nearT = static_cast<f32>(rootA < rootB ? rootA : rootB);
        farT = static_cast<f32>(rootA > rootB ? rootA : rootB);
        return true;
    };

    f32 outerNear = 0.0f, outerFar = 0.0f;
    if (!roots(outerAltitude, outerNear, outerFar) || outerFar <= 0.0f) {
        return out;
    }
    f32 innerNear = 0.0f, innerFar = 0.0f;
    const bool hitsInner = roots(innerAltitude, innerNear, innerFar);

    if (innerC < 0.0) {
        if (!hitsInner || innerFar <= 0.0f) return out;
        out.enter = innerFar > 0.0f ? innerFar : 0.0f;
        out.exit = outerFar;
    } else if (outerC <= 0.0) {
        out.enter = 0.0f;
        // 雲底・雲頂の境界上では innerNear が0になり得る。内向きのレイだけ
        // 0を終端として認め、層の反対側まで誤って積分しない。
        out.exit = (centreDot < 0.0 && hitsInner && innerNear >= 0.0f)
                 ? innerNear : outerFar;
    } else {
        if (outerNear <= 0.0f) return out;
        out.enter = outerNear;
        out.exit = (hitsInner && innerNear > out.enter) ? innerNear : outerFar;
    }
    out.hit = out.exit > out.enter;
    if (!out.hit) {
        out.enter = 0.0f;
        out.exit = 0.0f;
    }
    return out;
}

namespace {

/** GPUへ渡す球殻係数と同じ単精度の因数分解式でc項を求める。 */
f32 CloudShellCFromLocalPosition_Internal(
    FVec3 local_position, f32 altitude) noexcept {
    const f32 radialSquared =
        local_position.x * local_position.x +
        local_position.z * local_position.z;
    return radialSquared +
        (local_position.y - altitude) *
        (2.0f * kVolumetricCloudPlanetRadius +
         local_position.y + altitude);
}

/** GPUのsphereRootsFromTermsと同じ単精度・同じq形式で二交点を求める。 */
bool CloudSphereRootsFromTerms_Internal(
    f32 center_dot, f32 shell_c, bool accept_rounded_outer_tangent,
    f32& near_distance, f32& far_distance) noexcept {
    near_distance = 0.0f;
    far_distance = 0.0f;
    const f32 centerDotSquared = center_dot * center_dot;
    const f32 discriminant = centerDotSquared - shell_c;
    const f32 absoluteShellC = shell_c >= 0.0f ? shell_c : -shell_c;
    const f32 discriminantMagnitude =
        centerDotSquared + absoluteShellC > 1.0f
            ? centerDotSquared + absoluteShellC : 1.0f;
    const f32 discriminantTolerance = discriminantMagnitude *
        kCloudShellDiscriminantRelativeToleranceInternal;
    const bool hit = discriminant >= 0.0f ||
        (accept_rounded_outer_tangent &&
         discriminant >= -discriminantTolerance);
    if (!hit) return false;
    const f32 root = Sqrt(discriminant > 0.0f ? discriminant : 0.0f);
    const f32 q = -center_dot -
        (center_dot >= 0.0f ? root : -root);
    const f32 absoluteQ = q >= 0.0f ? q : -q;
    const f32 rootA = q;
    const f32 rootB = absoluteQ > 1.0e-5f
        ? shell_c / q : -center_dot + root;
    near_distance = rootA < rootB ? rootA : rootB;
    far_distance = rootA > rootB ? rootA : rootB;
    return true;
}

/**
 * 正規化済み方向を再正規化せず、GPU主描画と同じ単精度球殻区間を求める。
 * 判別式が負、または正方向に連続区間がない場合はhit=falseを返す。
 */
FVolumetricCloudRayInterval IntersectVolumetricCloudShellGpuMirror_Internal(
    FVec3 shell_local_origin, FVec3 normalized_direction,
    const FVolumetricCloudLayer& layer) noexcept {
    FVolumetricCloudRayInterval out{};
    const FVec3 fromCenter{
        shell_local_origin.x,
        shell_local_origin.y + kVolumetricCloudPlanetRadius,
        shell_local_origin.z};
    const f32 centerDot =
        fromCenter.x * normalized_direction.x +
        fromCenter.y * normalized_direction.y +
        fromCenter.z * normalized_direction.z;
    const f32 innerC = CloudShellCFromLocalPosition_Internal(
        shell_local_origin, layer.base_height);
    const f32 outerC = CloudShellCFromLocalPosition_Internal(
        shell_local_origin, layer.top_height);

    f32 outerNear = 0.0f;
    f32 outerFar = 0.0f;
    if (!CloudSphereRootsFromTerms_Internal(
            centerDot, outerC, true, outerNear, outerFar) ||
        outerFar <= 0.0f) {
        return out;
    }
    f32 innerNear = 0.0f;
    f32 innerFar = 0.0f;
    const bool hitsInner = CloudSphereRootsFromTerms_Internal(
        centerDot, innerC, false, innerNear, innerFar);
    if (innerC < 0.0f) {
        if (!hitsInner || innerFar <= 0.0f) return out;
        out.enter = innerFar > 0.0f ? innerFar : 0.0f;
        out.exit = outerFar;
    } else if (outerC <= 0.0f) {
        out.enter = 0.0f;
        out.exit = centerDot < 0.0f && hitsInner && innerNear >= 0.0f
            ? innerNear : outerFar;
    } else {
        if (outerNear <= 0.0f) return out;
        out.enter = outerNear;
        out.exit = hitsInner && innerNear > out.enter
            ? innerNear : outerFar;
    }
    out.hit = out.exit > out.enter;
    if (!out.hit) {
        out.enter = 0.0f;
        out.exit = 0.0f;
    }
    return out;
}

/** 正方向と終端距離で切った球殻区間を、近い順の空き要素へ追加する。 */
void AppendCloudShellInterval_Internal(
    f32 candidate_enter, f32 candidate_exit, f32 ray_end,
    render_internal::FVolumetricCloudShellIntervalSetInternal& out) noexcept
{
    if (out.interval_count >= 2u || ray_end <= 0.0f) return;
    const f32 clippedEnter = candidate_enter > 0.0f
        ? candidate_enter : 0.0f;
    const f32 clippedExit = candidate_exit < ray_end
        ? candidate_exit : ray_end;
    if (clippedExit <= clippedEnter) return;
    FVolumetricCloudRayInterval& interval =
        out.intervals[out.interval_count++];
    interval.enter = clippedEnter;
    interval.exit = clippedExit;
    interval.hit = true;
}

/** 始点から最初の惑星表面までの距離で、雲を積分できる光路終端を切る。 */
f32 ResolveCloudPlanetRayEnd_Internal(
    FVec3 shell_local_origin, f32 center_dot,
    f32 maximum_distance) noexcept
{
    if (maximum_distance <= 0.0f) return 0.0f;
    const f32 groundC = CloudShellCFromLocalPosition_Internal(
        shell_local_origin, 0.0f);
    if (groundC < 0.0f) return 0.0f;
    f32 groundNear = 0.0f;
    f32 groundFar = 0.0f;
    if (!CloudSphereRootsFromTerms_Internal(
            center_dot, groundC, false, groundNear, groundFar)) {
        return maximum_distance;
    }
    if (groundNear > 0.0f) {
        return groundNear < maximum_distance
            ? groundNear : maximum_distance;
    }
    if (groundC <= 0.0f && center_dot < 0.0f) return 0.0f;
    return maximum_distance;
}

/** GPUへ実装する二区間契約のCPU参照。 */
render_internal::FVolumetricCloudShellIntervalSetInternal
IntersectVolumetricCloudShellIntervalsGpuMirror_Internal(
    FVec3 shell_local_origin, FVec3 normalized_direction,
    const FVolumetricCloudLayer& layer,
    f32 maximum_distance) noexcept
{
    render_internal::FVolumetricCloudShellIntervalSetInternal out{};
    const FVec3 fromCenter{
        shell_local_origin.x,
        shell_local_origin.y + kVolumetricCloudPlanetRadius,
        shell_local_origin.z};
    const f32 centerDot =
        fromCenter.x * normalized_direction.x +
        fromCenter.y * normalized_direction.y +
        fromCenter.z * normalized_direction.z;
    const f32 innerC = CloudShellCFromLocalPosition_Internal(
        shell_local_origin, layer.base_height);
    const f32 outerC = CloudShellCFromLocalPosition_Internal(
        shell_local_origin, layer.top_height);
    const f32 rayEnd = ResolveCloudPlanetRayEnd_Internal(
        shell_local_origin, centerDot, maximum_distance);
    if (rayEnd <= 0.0f) return out;

    f32 outerNear = 0.0f;
    f32 outerFar = 0.0f;
    if (!CloudSphereRootsFromTerms_Internal(
            centerDot, outerC, true, outerNear, outerFar) ||
        outerFar <= 0.0f) {
        return out;
    }
    f32 innerNear = 0.0f;
    f32 innerFar = 0.0f;
    if (CloudSphereRootsFromTerms_Internal(
            centerDot, innerC, false, innerNear, innerFar)) {
        AppendCloudShellInterval_Internal(
            outerNear, innerNear, rayEnd, out);
        AppendCloudShellInterval_Internal(
            innerFar, outerFar, rayEnd, out);
    } else {
        AppendCloudShellInterval_Internal(
            outerNear, outerFar, rayEnd, out);
    }
    return out;
}

/** 正の区間幅を固定刻みで末尾まで覆うセル数へ切り上げる。 */
u32 CloudFineCellCount_Internal(
    f32 interval_span, f32 nominal_step, u32 maximum_samples) noexcept {
    const f32 safeStep = nominal_step > 1.0e-4f
        ? nominal_step : 1.0e-4f;
    u32 count = static_cast<u32>(Ceil(interval_span / safeStep));
    if (count < 1u) count = 1u;
    if (count > maximum_samples) count = maximum_samples;
    return count;
}

/** 表示状態に依存せず物理雲帯へ予約する視線標本予算。 */
struct FCloudPhysicalBandBudgetsInternal {
    u32 lower = 1u;
    u32 upper = 0u;
};

/** 球殻の接線光路が層厚の平方根に比例する性質から帯域別予算を求める。 */
FCloudPhysicalBandBudgetsInternal ResolveCloudPhysicalBandBudgets_Internal(
    const FVolumetricCloudLayer& lower,
    const FVolumetricCloudUpperLayer& upper,
    bool has_upper, u32 maximum_samples) noexcept {
    FCloudPhysicalBandBudgetsInternal out{};
    out.lower = maximum_samples > 0u ? maximum_samples : 1u;
    if (!has_upper || out.lower <= 1u) return out;

    const u32 minimumBandBudget = out.lower / 2u < kVolumetricCloudMinViewSteps
        ? out.lower / 2u : kVolumetricCloudMinViewSteps;
    const u32 weightedBudget = out.lower - 2u * minimumBandBudget;
    const f32 lowerThickness = lower.top_height - lower.base_height;
    const f32 upperThickness = upper.top_height - upper.base_height;
    const f32 lowerWeight = Sqrt(
        lowerThickness > 1.0e-4f ? lowerThickness : 1.0e-4f);
    const f32 upperWeight = Sqrt(
        upperThickness > 1.0e-4f ? upperThickness : 1.0e-4f);
    const f32 weightSum = lowerWeight + upperWeight > 1.0e-4f
        ? lowerWeight + upperWeight : 1.0e-4f;
    u32 weightedLowerBudget = static_cast<u32>(Floor(
        static_cast<f32>(weightedBudget) * lowerWeight / weightSum + 0.5f));
    if (weightedLowerBudget > weightedBudget) {
        weightedLowerBudget = weightedBudget;
    }
    const u32 lowerBudget = minimumBandBudget + weightedLowerBudget;
    out.upper = out.lower - lowerBudget;
    out.lower = lowerBudget;
    return out;
}

} // namespace

bool render_internal::ResolveVolumetricCloudSphereRoots_Internal(
    f32 center_dot, f32 shell_c, bool accept_rounded_outer_tangent,
    f32& near_distance, f32& far_distance) noexcept {
    return CloudSphereRootsFromTerms_Internal(
        center_dot, shell_c, accept_rounded_outer_tangent,
        near_distance, far_distance);
}

FVolumetricCloudRayInterval
render_internal::ResolveVolumetricCloudShellInterval_Internal(
    FVec3 shell_local_origin, FVec3 normalized_direction,
    const FVolumetricCloudLayer& layer) noexcept {
    return IntersectVolumetricCloudShellGpuMirror_Internal(
        shell_local_origin, normalized_direction, layer);
}

render_internal::FVolumetricCloudShellIntervalSetInternal
render_internal::ResolveVolumetricCloudShellIntervals_Internal(
    FVec3 shell_local_origin, FVec3 normalized_direction,
    const FVolumetricCloudLayer& layer,
    f32 maximum_distance) noexcept
{
    return IntersectVolumetricCloudShellIntervalsGpuMirror_Internal(
        shell_local_origin, normalized_direction,
        layer, maximum_distance);
}

render_internal::FVolumetricCloudShellIntervalSetInternal
render_internal::ResolveVolumetricCloudIntervalEnvelopePair_Internal(
    const FVolumetricCloudRayInterval* candidates,
    u32 candidate_count) noexcept
{
    FVolumetricCloudShellIntervalSetInternal out{};
    constexpr u32 maximumCandidateCount = 8u;
    if (candidates == nullptr || candidate_count == 0u ||
        candidate_count > maximumCandidateCount)
        return out;

    FVolumetricCloudRayInterval sorted[maximumCandidateCount]{};
    u32 validCount = 0u;
    for (u32 candidateIndex = 0u;
         candidateIndex < candidate_count; ++candidateIndex)
    {
        const FVolumetricCloudRayInterval candidate =
            candidates[candidateIndex];
        if (!candidate.hit ||
            !CloudDensityIntegrationValueIsFinite_Internal(
                candidate.enter) ||
            !CloudDensityIntegrationValueIsFinite_Internal(
                candidate.exit) ||
            candidate.exit <= candidate.enter)
            continue;

        u32 insertionIndex = validCount;
        while (insertionIndex > 0u)
        {
            const FVolumetricCloudRayInterval& previous =
                sorted[insertionIndex - 1u];
            if (previous.enter < candidate.enter ||
                (previous.enter == candidate.enter &&
                 previous.exit <= candidate.exit))
                break;
            sorted[insertionIndex] = previous;
            --insertionIndex;
        }
        sorted[insertionIndex] = candidate;
        ++validCount;
    }
    if (validCount == 0u) return out;

    const f32 envelopeStart = sorted[0].enter;
    f32 currentEnd = sorted[0].exit;
    f32 largestGap = 0.0f;
    f32 splitEnd = 0.0f;
    f32 splitStart = 0.0f;
    for (u32 candidateIndex = 1u;
         candidateIndex < validCount; ++candidateIndex)
    {
        const FVolumetricCloudRayInterval& candidate =
            sorted[candidateIndex];
        const f32 gap = candidate.enter > currentEnd
            ? candidate.enter - currentEnd : 0.0f;
        if (gap > largestGap)
        {
            largestGap = gap;
            splitEnd = currentEnd;
            splitStart = candidate.enter;
        }
        if (candidate.exit > currentEnd) currentEnd = candidate.exit;
    }

    out.intervals[0] = FVolumetricCloudRayInterval{
        envelopeStart, largestGap > 0.0f ? splitEnd : currentEnd, true};
    out.interval_count = 1u;
    if (largestGap > 0.0f)
    {
        out.intervals[1] = FVolumetricCloudRayInterval{
            splitStart, currentEnd, true};
        out.interval_count = 2u;
    }
    return out;
}

FVolumetricCloudRayInterval
render_internal::ResolveVolumetricCloudShellOverlapComponent_Internal(
    const FVolumetricCloudShellIntervalSetInternal& intervals,
    u32 component_index, f32 segment_start, f32 segment_end) noexcept
{
    FVolumetricCloudRayInterval out{};
    if (component_index >= intervals.interval_count ||
        component_index >= 2u || segment_end <= segment_start)
        return out;
    const FVolumetricCloudRayInterval& interval =
        intervals.intervals[component_index];
    if (!interval.hit || interval.exit <= interval.enter) return out;
    out.enter = segment_start > interval.enter
        ? segment_start : interval.enter;
    out.exit = segment_end < interval.exit
        ? segment_end : interval.exit;
    out.hit = out.exit > out.enter;
    if (!out.hit)
    {
        out.enter = 0.0f;
        out.exit = 0.0f;
    }
    return out;
}

f32 render_internal::ResolveVolumetricCloudShellOverlapLength_Internal(
    const FVolumetricCloudShellIntervalSetInternal& intervals,
    f32 segment_start, f32 segment_end) noexcept
{
    if (segment_end <= segment_start) return 0.0f;
    f32 overlapLength = 0.0f;
    for (u32 intervalIndex = 0u;
         intervalIndex < intervals.interval_count && intervalIndex < 2u;
         ++intervalIndex) {
        const FVolumetricCloudRayInterval& interval =
            intervals.intervals[intervalIndex];
        if (!interval.hit || interval.exit <= interval.enter) continue;
        const f32 overlapStart = segment_start > interval.enter
            ? segment_start : interval.enter;
        const f32 overlapEnd = segment_end < interval.exit
            ? segment_end : interval.exit;
        if (overlapEnd > overlapStart)
            overlapLength += overlapEnd - overlapStart;
    }
    return overlapLength;
}

void render_internal::ResolveVolumetricCloudPhysicalBandBudgets_Internal(
    const FVolumetricCloudLayer& lower_layer,
    const FVolumetricCloudUpperLayer& upper_layer,
    bool has_upper_layer, u32 maximum_samples,
    u32& lower_budget, u32& upper_budget) noexcept {
    const FCloudPhysicalBandBudgetsInternal budgets =
        ResolveCloudPhysicalBandBudgets_Internal(
            lower_layer, upper_layer, has_upper_layer, maximum_samples);
    lower_budget = budgets.lower;
    upper_budget = budgets.upper;
}

render_internal::FVolumetricCloudRayMarchPlanInternal
render_internal::PlanVolumetricCloudRayMarch_Internal(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& lower_layer,
    const FVolumetricCloudUpperLayer& upper_layer,
    bool has_upper_layer, const FVolumetricCloudRange& requested_range,
    FVec3 world_origin, u32 maximum_samples) noexcept
{
    FVolumetricCloudRayMarchPlanInternal out{};
    const FVolumetricCloudLayer lower =
        SanitizeVolumetricCloudLayer(lower_layer);
    const FVolumetricCloudUpperLayer upper =
        SanitizeVolumetricCloudUpperLayer(upper_layer, lower);
    const FVolumetricCloudRange range =
        SanitizeVolumetricCloudRange(requested_range);
    const bool hasUpper = has_upper_layer &&
        upper.top_height > upper.base_height &&
        upper.base_height >= lower.top_height;
    if (maximum_samples == 0u) {
        maximum_samples = range.ViewSteps > 0u
            ? range.ViewSteps : kVolumetricCloudMaxViewMarchSamples;
    }
    if (maximum_samples < kVolumetricCloudMinViewSteps) {
        maximum_samples = kVolumetricCloudMinViewSteps;
    }
    if (maximum_samples > kVolumetricCloudReferenceViewSteps) {
        maximum_samples = kVolumetricCloudReferenceViewSteps;
    }
    out.maximum_samples = maximum_samples;
    const f32 len2 = ray_direction.x * ray_direction.x +
                     ray_direction.y * ray_direction.y +
                     ray_direction.z * ray_direction.z;
    if (!(len2 > 1e-12f)) return out;
    const f32 invLen = 1.0f / Sqrt(len2);
    const FVec3 dir{ray_direction.x * invLen, ray_direction.y * invLen,
                    ray_direction.z * invLen};

    const FVec3 shellLocalOrigin{
        ray_origin.x - world_origin.x,
        ray_origin.y - world_origin.y,
        ray_origin.z - world_origin.z};
    out.maximum_distance = ResolveVolumetricCloudViewDistance_Internal(
        shellLocalOrigin, lower, upper, hasUpper, range.MaxDistance);
    if (out.maximum_distance > range.MaxDistance) {
        out.maximum_distance = range.MaxDistance;
    }

    const FVolumetricCloudGroundHorizon groundHorizon =
        ResolveVolumetricCloudGroundHorizon(
            ray_origin, lower, world_origin);
    const f32 elevation =
        dir.x * groundHorizon.local_up.x +
        dir.y * groundHorizon.local_up.y +
        dir.z * groundHorizon.local_up.z;
    if (groundHorizon.ground_cutoff >= -1.0f &&
        elevation < groundHorizon.ground_cutoff) {
        return out;
    }

    const FVolumetricCloudShellIntervalSetInternal lowerIntervals =
        IntersectVolumetricCloudShellIntervalsGpuMirror_Internal(
            shellLocalOrigin, dir, lower, out.maximum_distance);
    FVolumetricCloudShellIntervalSetInternal upperIntervals{};
    if (hasUpper) {
        const FVolumetricCloudLayer upperShell{
            upper.base_height, upper.top_height,
            lower.horizontal_noise_scale};
        upperIntervals =
            IntersectVolumetricCloudShellIntervalsGpuMirror_Internal(
                shellLocalOrigin, dir, upperShell,
                out.maximum_distance);
    }
    for (u32 physicalBandId = 0u; physicalBandId < 2u;
         ++physicalBandId) {
        const FVolumetricCloudShellIntervalSetInternal& shellIntervals =
            physicalBandId == 0u ? lowerIntervals : upperIntervals;
        for (u32 shellIntervalIndex = 0u;
             shellIntervalIndex < shellIntervals.interval_count &&
             shellIntervalIndex < 2u && out.interval_count < 4u;
             ++shellIntervalIndex) {
            const FVolumetricCloudRayInterval& shellInterval =
                shellIntervals.intervals[shellIntervalIndex];
            if (!shellInterval.hit ||
                shellInterval.exit <= shellInterval.enter) {
                continue;
            }
            FVolumetricCloudRayMarchPlanInternal::FInterval& interval =
                out.intervals[out.interval_count++];
            interval.enter = shellInterval.enter;
            interval.exit = shellInterval.exit;
            interval.physical_band_id = physicalBandId;
        }
    }
    if (out.interval_count == 0u) return out;

    // GPUの四区間並べ替えと同じく、物理雲帯ではなく入口距離で処理順を決める。
    for (u32 intervalIndex = 1u;
         intervalIndex < out.interval_count; ++intervalIndex) {
        const FVolumetricCloudRayMarchPlanInternal::FInterval current =
            out.intervals[intervalIndex];
        u32 insertionIndex = intervalIndex;
        while (insertionIndex > 0u &&
               current.enter < out.intervals[insertionIndex - 1u].enter) {
            out.intervals[insertionIndex] =
                out.intervals[insertionIndex - 1u];
            --insertionIndex;
        }
        out.intervals[insertionIndex] = current;
    }

    f32 scale = lower.horizontal_noise_scale;
    if (scale < 0.001f) scale = 0.001f;
    f32 baseFineStep = 0.035f / scale;
    if (baseFineStep < 0.5f) baseFineStep = 0.5f;
    if (baseFineStep > 2.0f) baseFineStep = 2.0f;
    const FCloudPhysicalBandBudgetsInternal bandBudgets =
        ResolveCloudPhysicalBandBudgets_Internal(
            lower, upper, hasUpper, maximum_samples);
    u32 intervalBudgets[4]{};
    for (u32 physicalBandId = 0u; physicalBandId < 2u;
         ++physicalBandId) {
        u32 matchingIndices[2]{4u, 4u};
        u32 matchingCount = 0u;
        f32 matchingSpan = 0.0f;
        for (u32 intervalIndex = 0u;
             intervalIndex < out.interval_count; ++intervalIndex) {
            const FVolumetricCloudRayMarchPlanInternal::FInterval& interval =
                out.intervals[intervalIndex];
            if (interval.physical_band_id != physicalBandId) continue;
            if (matchingCount < 2u)
                matchingIndices[matchingCount] = intervalIndex;
            ++matchingCount;
            matchingSpan += interval.exit - interval.enter;
        }
        if (matchingCount == 0u) continue;
        u32 bandBudget = physicalBandId == 0u
            ? bandBudgets.lower : bandBudgets.upper;
        if (matchingCount == 1u) {
            intervalBudgets[matchingIndices[0]] =
                bandBudget > 0u ? bandBudget : 1u;
            continue;
        }
        if (bandBudget < 2u) bandBudget = 2u;
        const u32 distributableBudget = bandBudget - 2u;
        const u32 firstIndex = matchingIndices[0];
        const f32 firstSpan =
            out.intervals[firstIndex].exit -
            out.intervals[firstIndex].enter;
        u32 firstExtra = static_cast<u32>(Floor(
            static_cast<f32>(distributableBudget) * firstSpan /
                (matchingSpan > 1.0e-4f ? matchingSpan : 1.0e-4f) +
            0.5f));
        if (firstExtra > distributableBudget)
            firstExtra = distributableBudget;
        intervalBudgets[firstIndex] = 1u + firstExtra;
        intervalBudgets[matchingIndices[1]] =
            bandBudget - intervalBudgets[firstIndex];
    }
    out.total_fine_cell_count = 0u;
    for (u32 intervalIndex = 0u;
         intervalIndex < out.interval_count; ++intervalIndex) {
        FVolumetricCloudRayMarchPlanInternal::FInterval& interval =
            out.intervals[intervalIndex];
        const f32 span = interval.exit - interval.enter;
        const u32 intervalBudget = intervalBudgets[intervalIndex] > 0u
            ? intervalBudgets[intervalIndex] : 1u;
        const f32 distanceRatio = Clamp(
            interval.enter /
                (out.maximum_distance > 1.0f
                    ? out.maximum_distance : 1.0f),
            0.0f, 1.0f);
        const f32 distanceLod =
            1.0f + range.StepGrowth * distanceRatio;
        const f32 budgetFineStep =
            span / static_cast<f32>(intervalBudget);
        interval.fine_step =
            (baseFineStep > budgetFineStep
                ? baseFineStep : budgetFineStep) * distanceLod;
        interval.fine_cell_count = CloudFineCellCount_Internal(
            span, interval.fine_step, intervalBudget);
        out.total_fine_cell_count += interval.fine_cell_count;
    }
    out.requested_fine_step = out.intervals[0].fine_step;
    out.visibility = EvaluateVolumetricCloudDistanceFade(
        out.intervals[0].enter, out.maximum_distance,
        range.FadeFraction);
    out.hit = true;
    return out;
}

FVolumetricCloudMarchPlan PlanVolumetricCloudRayMarch(
    FVec3 ray_origin, FVec3 ray_direction,
    const FVolumetricCloudLayer& layer, f32 max_distance,
    FVec3 world_origin, u32 maximum_samples) noexcept
{
    FVolumetricCloudMarchPlan out{};
    if (maximum_samples == 0u) {
        maximum_samples = kVolumetricCloudMaxViewMarchSamples;
    }
    if (maximum_samples < kVolumetricCloudMinViewSteps) {
        maximum_samples = kVolumetricCloudMinViewSteps;
    }
    if (maximum_samples > kVolumetricCloudReferenceViewSteps) {
        maximum_samples = kVolumetricCloudReferenceViewSteps;
    }
    out.max_samples = maximum_samples;
    const f32 len2 = ray_direction.x * ray_direction.x +
                     ray_direction.y * ray_direction.y +
                     ray_direction.z * ray_direction.z;
    if (len2 <= 1e-12f) return out;
    const f32 invLen = 1.0f / Sqrt(len2);
    const FVec3 dir{
        ray_direction.x * invLen,
        ray_direction.y * invLen,
        ray_direction.z * invLen};

    const FVolumetricCloudRayInterval interval =
        IntersectVolumetricCloudShell(
            ray_origin, dir, layer, kVolumetricCloudPlanetRadius,
            world_origin);
    if (!interval.hit) return out;

    const FVolumetricCloudRange defaults{};
    max_distance = SanitizeCloudScalar(
        max_distance, defaults.MaxDistance,
        kVolumetricCloudMinDistance, kVolumetricCloudMaxDistance);
    out.enter = interval.enter;
    out.exit = interval.exit < max_distance ? interval.exit : max_distance;
    if (out.exit <= out.enter) {
        out.enter = 0.0f;
        out.exit = 0.0f;
        return out;
    }

    f32 scale = layer.horizontal_noise_scale;
    if (scale < 0.001f) scale = 0.001f;
    out.fine_step = 0.035f / scale;
    if (out.fine_step < 0.5f) out.fine_step = 0.5f;
    if (out.fine_step > 2.0f) out.fine_step = 2.0f;
    const f32 span = out.exit - out.enter;
    const u32 fineSampleBudget =
        maximum_samples - maximum_samples / 8u;
    const u32 coarseSampleBudget = fineSampleBudget / 2u;
    const f32 intervalFineCoverage =
        span / static_cast<f32>(fineSampleBudget);
    if (out.fine_step < intervalFineCoverage) {
        out.fine_step = intervalFineCoverage;
    }
    out.coarse_step = out.fine_step * 2.0f;
    const f32 intervalCoverage =
        span / static_cast<f32>(coarseSampleBudget);
    if (out.coarse_step < intervalCoverage) {
        out.coarse_step = intervalCoverage;
    }

    // 従来の公開計画契約は保ちつつ、固定角度ではなく実際の惑星接線で
    // 地面へ遮られる視線だけを棄却する。
    const FVolumetricCloudGroundHorizon groundHorizon =
        ResolveVolumetricCloudGroundHorizon(
            ray_origin, layer, world_origin);
    const f32 elevation =
        dir.x * groundHorizon.local_up.x +
        dir.y * groundHorizon.local_up.y +
        dir.z * groundHorizon.local_up.z;
    if (groundHorizon.ground_cutoff >= -1.0f &&
        elevation < groundHorizon.ground_cutoff) {
        out.enter = 0.0f;
        out.exit = 0.0f;
        return out;
    }
    out.visibility = EvaluateVolumetricCloudDistanceFade(
        out.enter, max_distance, defaults.FadeFraction);
    // GPUは各標本を距離で薄めるため、入口係数だけを使って有効区間を捨てない。
    out.hit = true;
    return out;
}

void CVolumetricClouds::InvalidateCloudHistory_Internal(bool density_field_changed) noexcept
{
    if (m_NoiseFilterResources) {
        ++m_NoiseFilterResources->settings_revision;
    }
    m_HistoryValid = false;
    if (density_field_changed) {
        m_ShadowCacheValid = false;
        m_WorldShadowValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        SetWorldShadowWarmupMask_Internal(0u);
    }
}

void CVolumetricClouds::SetLayer(const FVolumetricCloudLayer& requested) noexcept
{
    const FVolumetricCloudLayer layer = SanitizeVolumetricCloudLayer(requested);
    const FVolumetricCloudUpperLayer upper = SanitizeVolumetricCloudUpperLayer(m_UpperLayer, layer);
    if (CloudLayerEqual(layer, m_Layer) && CloudUpperLayerEqual(upper, m_UpperLayer)) {
        return;
    }
    m_Layer = layer;
    m_UpperLayer = upper;
    InvalidateCloudHistory_Internal(true);
}

void CVolumetricClouds::SetReferenceMode(bool enabled) noexcept
{
    if (enabled == m_ReferenceMode) return;
    m_ReferenceMode = enabled;
    InvalidateCloudHistory_Internal(false);
}

void CVolumetricClouds::SetLighting(const FVolumetricCloudLighting& requested) noexcept
{
    const FVolumetricCloudLighting lighting = SanitizeVolumetricCloudLighting(requested);
    if (CloudLightingEqual(lighting, m_Lighting)) return;
    const bool shadowTransportChanged =
        !CloudLightingShadowTransportEqual_Internal(lighting, m_Lighting);
    const bool historyTransportChanged =
        !CloudLightingHistoryTransportEqual_Internal(lighting, m_Lighting);
    m_Lighting = lighting;
    // 影キャッシュへ焼くのは光側消散だけであり、空色・地面色・太陽透過率は採取時に掛ける。
    // 物理空の放射輝度は次の雲追跡で連続反映するため、毎フレーム履歴を破棄しない。
    if (historyTransportChanged) {
        InvalidateCloudHistory_Internal(shadowTransportChanged);
    }
}

void CVolumetricClouds::SetWeather(const FVolumetricCloudWeather& requested) noexcept
{
    const FVolumetricCloudWeather weather = SanitizeVolumetricCloudWeather(requested);
    if (CloudWeatherEqual(weather, m_Weather)) return;
    m_Weather = weather;
    InvalidateCloudHistory_Internal(true);
}

void CVolumetricClouds::SetRange(const FVolumetricCloudRange& requested) noexcept
{
    const FVolumetricCloudRange range = SanitizeVolumetricCloudRange(requested);
    if (CloudRangeEqual(range, m_Range)) return;
    m_Range = range;
    // 最大距離は非一様キャッシュの外周写像を決めるため、古い座標の値を再利用しない。
    InvalidateCloudHistory_Internal(true);
}

void CVolumetricClouds::SetUpperLayer(const FVolumetricCloudUpperLayer& requested) noexcept
{
    const FVolumetricCloudUpperLayer upper = SanitizeVolumetricCloudUpperLayer(requested, m_Layer);
    if (CloudUpperLayerEqual(upper, m_UpperLayer)) return;
    m_UpperLayer = upper;
    InvalidateCloudHistory_Internal(true);
}

EShaderStatus CVolumetricClouds::FCompiledShaders::Status() const noexcept {
    IRhiShader* const mandatory[] = {
        cloud.Get(),
        noise.Get(),
        noise_filter.Get(),
        weather.Get(),
        detail.Get(),
        curl.Get(),
        composite_vertex.Get(),
        composite_pixel.Get(),
        composite_atmosphere_pixel.Get(),
        resolve.Get(),
    };
    bool compiling = false;
    bool failed = false;
    for (IRhiShader* shader : mandatory) {
        if (shader == nullptr) {
            failed = true;
            continue;
        }
        const EShaderStatus status = shader->Status();
        failed = failed || status == EShaderStatus::Failed;
        compiling = compiling || status == EShaderStatus::Compiling;
    }

    // 任意シェーダーは完了まで待つが、失敗しても必須の雲描画を失敗扱いにしない。
    // 同期経路と非同期経路で同じ縮退結果にし、初期化側が該当機能だけを無効化する。
    if (shadow) {
        const EShaderStatus shadowStatus = shadow->Status();
        compiling = compiling || shadowStatus == EShaderStatus::Compiling;
    }
    if (shadow_finalize) {
        const EShaderStatus ambientStatus = shadow_finalize->Status();
        compiling = compiling || ambientStatus == EShaderStatus::Compiling;
    }
    if (world_shadow) {
        const EShaderStatus world_shadow_status = world_shadow->Status();
        compiling = compiling ||
            world_shadow_status == EShaderStatus::Compiling;
    }
    if (compiling) return EShaderStatus::Compiling;
    return failed ? EShaderStatus::Failed : EShaderStatus::Ready;
}

namespace {

TResult<TUniquePtr<IRhiShader>> CreateCloudShaderHandle(
    IRhiDevice& device, EShaderStage stage, const char* source,
    const char* entry, const char* name, bool compile_async) noexcept {
    FShaderDesc desc{};
    desc.stage = stage;
    desc.hlsl_source = source;
    desc.entry_point = entry;
    desc.debug_name = name;
    desc.compile_async = compile_async;
    return CreateRhiShader(device, desc);
}

TResult<CVolumetricClouds::FCompiledShaders> CreateCloudShaderSet(
    IRhiDevice& device, bool compile_async,
    bool include_optional_shadows) noexcept {
    if (compile_async && !device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 580,
            "Volumetric-cloud backend-managed asynchronous compilation "
            "is unsupported");
    }

    auto compile = [&device, compile_async](
                       EShaderStage stage, const char* source,
                       const char* entry, const char* name) noexcept {
        return CreateCloudShaderHandle(
            device, stage, source, entry, name, compile_async);
    };

    CVolumetricClouds::FCompiledShaders shaders{};
#define ACS_CREATE_CLOUD_SHADER(member, stage, source, entry, name)           \
    do {                                                                      \
        auto result = compile(stage, source, entry, name);                    \
        if (result.IsErr()) {                                                 \
            return Err<CVolumetricClouds::FCompiledShaders>(result.Error());  \
        }                                                                     \
        shaders.member = Move(result.Value());                                \
    } while (false)
    ACS_CREATE_CLOUD_SHADER(cloud, EShaderStage::Compute,
                            kCloudCS, "CSCloud", "Clouds.CS");
    ACS_CREATE_CLOUD_SHADER(noise, EShaderStage::Compute,
                            kNoiseGenCS, "CSNoise", "Clouds.NoiseCS");
    ACS_CREATE_CLOUD_SHADER(
        noise_filter, EShaderStage::Compute,
        kNoiseFilterCS, "CSNoiseFilter", "Clouds.NoiseFilterCS");
    ACS_CREATE_CLOUD_SHADER(weather, EShaderStage::Compute,
                            kWeatherGenCS, "CSWeather", "Clouds.WeatherCS");
    ACS_CREATE_CLOUD_SHADER(detail, EShaderStage::Compute,
                            kDetailGenCS, "CSDetail", "Clouds.DetailCS");
    ACS_CREATE_CLOUD_SHADER(curl, EShaderStage::Compute,
                            kCurlGenCS, "CSCurl", "Clouds.CurlCS");
    ACS_CREATE_CLOUD_SHADER(composite_vertex, EShaderStage::Vertex,
                            kCloudCompVS, "VSMain", "Clouds.CompVS");
    ACS_CREATE_CLOUD_SHADER(composite_pixel, EShaderStage::Pixel,
                            kCloudCompPS, "PSMain", "Clouds.CompPS");
    ACS_CREATE_CLOUD_SHADER(
        composite_atmosphere_pixel, EShaderStage::Pixel,
        kCloudCompAtmosPS, "PSMainAtmos", "Clouds.CompAtmosPS");
    ACS_CREATE_CLOUD_SHADER(resolve, EShaderStage::Compute,
                            kCloudResolveCS, "CSResolve", "Clouds.ResolveCS");
    if (include_optional_shadows &&
        kVolumetricCloudShadowCacheEnabled) {
        auto shadow_result = compile(
            EShaderStage::Compute, kCloudCS,
            "CSCloudShadow", "Clouds.ShadowCacheCS");
        if (shadow_result.IsOk()) {
            shaders.shadow = Move(shadow_result.Value());
        }
    }
    if (include_optional_shadows &&
        kVolumetricCloudWorldShadowEnabled) {
        auto world_shadow_result = compile(EShaderStage::Compute, kCloudCS, "CSCloudWorldShadow", "Clouds.WorldShadowCS");
        if (world_shadow_result.IsOk()) {
            shaders.world_shadow = Move(world_shadow_result.Value());
        }
    }
#undef ACS_CREATE_CLOUD_SHADER
    return TResult<CVolumetricClouds::FCompiledShaders>(
        OkInit, Move(shaders));
}

} // namespace

TResult<CVolumetricClouds::FCompiledShaders>
CVolumetricClouds::CompileShadersCpu() noexcept {
#if !WITH_RENDER_DILIGENT
    auto compile = [](EShaderStage stage, const char* source,
                      const char* entry, const char* name) noexcept
        -> TResult<TUniquePtr<IRhiShader>> {
        FShaderDesc desc{};
        desc.stage = stage;
        desc.hlsl_source = source;
        desc.entry_point = entry;
        desc.debug_name = name;
        auto shader = MakeUnique<FDx12Shader>();
        if (!shader) {
            return ACS_ERR(
                Memory, 577,
                "Volumetric-cloud shader allocation failed");
        }
        const FHrResult result = shader->Init(desc);
        if (result.IsErr()) {
            return ACS_ERR_OS(
                Render, 578,
                "Volumetric-cloud shader CPU compile failed",
                static_cast<u32>(result.hr));
        }
        TUniquePtr<IRhiShader> compiled(
            shader.Release(), shader.GetAllocator());
        return TResult<TUniquePtr<IRhiShader>>(
            OkInit, Move(compiled));
    };

    FCompiledShaders shaders{};
#define ACS_COMPILE_CLOUD_SHADER(member, stage, source, entry, name)          \
    do {                                                                      \
        auto result = compile(stage, source, entry, name);                    \
        if (result.IsErr())                                                   \
            return Err<FCompiledShaders>(result.Error());                    \
        shaders.member = Move(result.Value());                               \
    } while (false)
    ACS_COMPILE_CLOUD_SHADER(cloud, EShaderStage::Compute,
                             kCloudCS, "CSCloud", "Clouds.CS");
    ACS_COMPILE_CLOUD_SHADER(noise, EShaderStage::Compute,
                             kNoiseGenCS, "CSNoise", "Clouds.NoiseCS");
    ACS_COMPILE_CLOUD_SHADER(
        noise_filter, EShaderStage::Compute,
        kNoiseFilterCS, "CSNoiseFilter", "Clouds.NoiseFilterCS");
    ACS_COMPILE_CLOUD_SHADER(weather, EShaderStage::Compute,
                             kWeatherGenCS, "CSWeather", "Clouds.WeatherCS");
    ACS_COMPILE_CLOUD_SHADER(detail, EShaderStage::Compute,
                             kDetailGenCS, "CSDetail", "Clouds.DetailCS");
    ACS_COMPILE_CLOUD_SHADER(curl, EShaderStage::Compute,
                             kCurlGenCS, "CSCurl", "Clouds.CurlCS");
    ACS_COMPILE_CLOUD_SHADER(composite_vertex, EShaderStage::Vertex,
                             kCloudCompVS, "VSMain", "Clouds.CompVS");
    ACS_COMPILE_CLOUD_SHADER(composite_pixel, EShaderStage::Pixel,
                             kCloudCompPS, "PSMain", "Clouds.CompPS");
    ACS_COMPILE_CLOUD_SHADER(
        composite_atmosphere_pixel, EShaderStage::Pixel,
        kCloudCompAtmosPS, "PSMainAtmos", "Clouds.CompAtmosPS");
    ACS_COMPILE_CLOUD_SHADER(resolve, EShaderStage::Compute,
                             kCloudResolveCS, "CSResolve", "Clouds.ResolveCS");
    if (kVolumetricCloudShadowCacheEnabled) {
        auto shadow_result = compile(
            EShaderStage::Compute, kCloudCS,
            "CSCloudShadow", "Clouds.ShadowCacheCS");
        if (shadow_result.IsOk()) {
            shaders.shadow = Move(shadow_result.Value());
        }
    }
    if (kVolumetricCloudWorldShadowEnabled) {
        auto world_shadow_result = compile(EShaderStage::Compute, kCloudCS, "CSCloudWorldShadow", "Clouds.WorldShadowCS");
        if (world_shadow_result.IsOk()) {
            shaders.world_shadow = Move(world_shadow_result.Value());
        }
    }
#undef ACS_COMPILE_CLOUD_SHADER
    return TResult<FCompiledShaders>(OkInit, Move(shaders));
#else
    return ACS_ERR(
        Render, 579,
        "Volumetric-cloud CPU compilation is available only on raw DX12");
#endif
}

TResult<void> CVolumetricClouds::Init(
    IRhiDevice& device, EFormat hdr_format) noexcept {
    if (InitializationPending()) {
        return ACS_ERR(
            Render, 582,
            "Volumetric-cloud staged initialization is already active");
    }
    auto shader_result = CreateCloudShaderSet(device, false, true);
    if (shader_result.IsErr()) return Err<void>(shader_result.Error());
    return InitWithCompiledShaders(
        device, Move(shader_result.Value()), hdr_format);
}

TResult<CVolumetricClouds::FCompiledShaders>
CVolumetricClouds::BeginCompileShadersAsync(IRhiDevice& device) noexcept {
    return CreateCloudShaderSet(device, true, true);
}

bool CVolumetricClouds::InitializationPending() const noexcept {
    if (m_Ready || !m_NoiseFilterResources) return false;
    const EAsyncInitializationState state =
        m_NoiseFilterResources->initialization_state;
    return state == EAsyncInitializationState::MandatoryShaders ||
        state == EAsyncInitializationState::ShadowShader ||
        state == EAsyncInitializationState::WorldShadowShader;
}

EShaderStatus
CVolumetricClouds::PendingMandatoryShaderStatus_Internal() const noexcept {
    IRhiShader* const mandatory[] = {
        m_CloudCs.Get(),
        m_NoiseCs.Get(),
        m_NoiseFilterResources
            ? m_NoiseFilterResources->shader.Get() : nullptr,
        m_WeatherCs.Get(),
        m_DetailCs.Get(),
        m_CurlCs.Get(),
        m_CompVs.Get(),
        m_CompPs.Get(),
        m_CompAtmosPs.Get(),
        m_ResolveCs.Get(),
    };
    bool compiling = false;
    bool failed = false;
    for (IRhiShader* shader : mandatory) {
        if (shader == nullptr) {
            failed = true;
            continue;
        }
        const EShaderStatus status = shader->Status();
        failed = failed || status == EShaderStatus::Failed;
        compiling = compiling || status == EShaderStatus::Compiling;
    }
    if (compiling) return EShaderStatus::Compiling;
    return failed ? EShaderStatus::Failed : EShaderStatus::Ready;
}

CVolumetricClouds::FCompiledShaders
CVolumetricClouds::TakePendingShaders_Internal() noexcept {
    FCompiledShaders shaders{};
    shaders.cloud = Move(m_CloudCs);
    shaders.noise = Move(m_NoiseCs);
    if (m_NoiseFilterResources) {
        shaders.noise_filter =
            Move(m_NoiseFilterResources->shader);
        m_NoiseFilterResources.Reset();
    }
    shaders.weather = Move(m_WeatherCs);
    shaders.detail = Move(m_DetailCs);
    shaders.curl = Move(m_CurlCs);
    shaders.composite_vertex = Move(m_CompVs);
    shaders.composite_pixel = Move(m_CompPs);
    shaders.composite_atmosphere_pixel = Move(m_CompAtmosPs);
    shaders.resolve = Move(m_ResolveCs);
    shaders.shadow = Move(m_ShadowCs);
    shaders.world_shadow = Move(m_WorldShadowCs);
    return shaders;
}

u8 CVolumetricClouds::ShadowCacheWarmupMask_Internal() const noexcept {
    return m_NoiseFilterResources
        ? m_NoiseFilterResources->shadow_cache_warmup_mask : 0u;
}

void CVolumetricClouds::SetShadowCacheWarmupMask_Internal(u8 mask) noexcept {
    if (m_NoiseFilterResources) {
        m_NoiseFilterResources->shadow_cache_warmup_mask = mask;
    }
}

u8 CVolumetricClouds::WorldShadowWarmupMask_Internal() const noexcept {
    return m_NoiseFilterResources
        ? m_NoiseFilterResources->world_shadow_warmup_mask : 0u;
}

void CVolumetricClouds::SetWorldShadowWarmupMask_Internal(u8 mask) noexcept {
    if (m_NoiseFilterResources) {
        m_NoiseFilterResources->world_shadow_warmup_mask = mask;
    }
}

bool CVolumetricClouds::IsInitializedForDevice(
    const IRhiDevice& device) const noexcept {
    return m_Ready && m_NoiseFilterResources &&
        m_NoiseFilterResources->resource_device == &device;
}

bool CVolumetricClouds::RecordedFramePending() const noexcept {
    return m_NoiseFilterResources &&
        m_NoiseFilterResources->recorded_frame.active;
}

u64 CVolumetricClouds::RecordedFrameSubmissionId() const noexcept {
    return RecordedFramePending()
        ? m_NoiseFilterResources->recorded_frame.submission_id : 0u;
}

bool CVolumetricClouds::RecordedCloudFramePending() const noexcept {
    return m_NoiseFilterResources &&
        m_NoiseFilterResources->recorded_frame.active &&
        m_NoiseFilterResources->recorded_frame.cloud_frame_recorded &&
        m_NoiseFilterResources->recorded_frame.settings_revision ==
            m_NoiseFilterResources->settings_revision;
}

IRhiTexture* CVolumetricClouds::ResolvedDepth() const noexcept {
    return m_HistoryValid
        ? m_HistoryDepth[m_ResolvedIndex].Get() : nullptr;
}

IRhiTexture* CVolumetricClouds::ResolvedDepth(
    const IRhiCommandList& command_list) const noexcept {
    if (RecordedCloudFramePending() &&
        m_NoiseFilterResources->recorded_frame.command_list == &command_list) {
        const FNoiseFilterResources::FRecordedFrameState& recorded =
            m_NoiseFilterResources->recorded_frame;
        return recorded.history_valid
            ? m_HistoryDepth[recorded.resolved_index].Get() : nullptr;
    }
    return ResolvedDepth();
}

void CVolumetricClouds::ResolveRecordedFrameSubmission(
    bool submitted) noexcept {
    // ID付き候補を旧bool通知で確定すると、遅延した古い通知が新しい候補へ
    // 適用される。旧経路はRenderComputeのIDなし候補だけを扱う。
    if (!m_NoiseFilterResources ||
        !m_NoiseFilterResources->recorded_frame.active ||
        m_NoiseFilterResources->recorded_frame.submission_id != 0u) {
        return;
    }
    (void)ResolveRecordedFrameSubmission_Internal(submitted);
}

bool CVolumetricClouds::ResolveRecordedFrameSubmission(
    u64 submission_id, bool submitted) noexcept {
    if (!m_NoiseFilterResources ||
        !m_NoiseFilterResources->recorded_frame.active) {
        return false;
    }

    if (submission_id == 0u ||
        m_NoiseFilterResources->recorded_frame.submission_id != submission_id) {
        return false;
    }
    return ResolveRecordedFrameSubmission_Internal(submitted);
}

bool CVolumetricClouds::ResolveRecordedFrameSubmission_Internal(
    bool submitted) noexcept {
    if (!m_NoiseFilterResources ||
        !m_NoiseFilterResources->recorded_frame.active) {
        return false;
    }

    FNoiseFilterResources::FRecordedFrameState& recorded =
        m_NoiseFilterResources->recorded_frame;
    if (!submitted) {
        m_LastFrameWorkload.submitted = false;
        m_LastFrameWorkload.submission_index = 0u;
        m_LastFrameWorkload.history_reused = false;
        m_LastFrameWorkload.history_invalidated = false;
        m_LastFrameWorkload.skip_reason =
            EVolumetricCloudFrameSkipReason::SubmissionFailed;
        recorded = {};
        return true;
    }

    const bool settingsUnchanged =
        recorded.settings_revision ==
        m_NoiseFilterResources->settings_revision;

    // 密度場は層や天候の係数に依存しない一回限りの基礎資源なので、記録後に
    // 設定が変わっても実際に提出できた生成段階だけは確定して再実行を避ける。
    m_NoiseBaked = recorded.noise_baked;
    m_WeatherBaked = recorded.weather_baked;
    m_DetailBaked = recorded.detail_baked;
    m_CurlBaked = recorded.curl_baked;
    m_NoiseFilterResources->density_bake_stage =
        recorded.density_bake_stage;
    m_ShadowCacheDispatchCount = recorded.shadow_cache_dispatch_count;
    m_WorldShadowDispatchCount = recorded.world_shadow_dispatch_count;
    m_LastFrameWorkload.submitted = true;

    if (!settingsUnchanged) {
        // GPU上の履歴と影は変更前の設定で書かれている。新設定へ座標や世代だけを
        // 混ぜず、次回に全位相を同じ設定で作り直す。
        m_HistoryValid = false;
        m_ShadowCacheValid = false;
        m_WorldShadowValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        SetWorldShadowWarmupMask_Internal(0u);
        m_LastFrameWorkload.submission_index = 0u;
        m_LastFrameWorkload.history_reused = false;
        m_LastFrameWorkload.history_invalidated =
            recorded.cloud_frame_recorded;
        recorded = {};
        return true;
    }

    m_NoiseFilterResources->shadow_cache_warmup_mask =
        recorded.shadow_cache_warmup_mask;
    m_NoiseFilterResources->world_shadow_warmup_mask =
        recorded.world_shadow_warmup_mask;
    m_NoiseFilterResources->world_shadow_mapping_initialized =
        recorded.world_shadow_mapping_initialized;

    m_ShadowGridMinQ = recorded.shadow_grid_minimum_material_xz;
    m_ShadowGridCenterQ = recorded.shadow_grid_center_material_xz;
    m_ShadowGridInitialized = recorded.shadow_grid_initialized;
    m_WorldShadowMapMinReferenceXz =
        recorded.world_shadow_map_minimum_reference_xz;
    m_WorldShadowReferenceHeight =
        recorded.world_shadow_reference_height;
    m_WorldShadowSunDirection = recorded.world_shadow_sun_direction;
    m_WorldShadowWorldOrigin = recorded.world_shadow_world_origin;
    m_WorldShadowCloudBaseAltitude =
        recorded.world_shadow_cloud_base_altitude;
    m_ShadowCacheValid = recorded.shadow_cache_valid;
    m_WorldShadowValid = recorded.world_shadow_valid;
    m_PrevCameraRelativeViewProj =
        recorded.previous_camera_relative_view_projection;
    m_PrevCameraRelativeInvViewProj =
        recorded.previous_camera_relative_inverse_view_projection;
    m_PrevCamPos = recorded.previous_camera_position;
    m_WorldOrigin = recorded.world_origin;
    m_PrevSunDir = recorded.previous_sun_direction;
    m_PrevSunColor = recorded.previous_sun_color;
    m_PrevSkyColor = recorded.previous_sky_color;
    m_NoiseFilterResources->previous_lighting = recorded.current_lighting;
    m_PrevWindOffset = recorded.previous_wind_offset;
    m_PrevWindSpeed = recorded.previous_wind_speed;
    m_PrevCoverage = recorded.previous_coverage;
    m_PrevDensity = recorded.previous_density;
    m_PrevTime = recorded.previous_time;
    m_FrameIndex = recorded.frame_index;
    m_TemporalPhase = recorded.temporal_phase;
    m_ResolvedIndex = recorded.resolved_index;
    m_HistoryValid = recorded.history_valid;
    m_WorkloadSubmissionIndex = recorded.workload_submission_index;

    if (recorded.cloud_frame_recorded) {
        m_LastFrameWorkload.submission_index =
            recorded.workload_submission_index;
    }
    recorded = {};
    return true;
}

TResult<void> CVolumetricClouds::BeginInitializationAsync(
    IRhiDevice& device, EFormat hdr_format) noexcept {
    if (m_Ready) {
        return ACS_ERR(
            Render, 583,
            "Volumetric-cloud renderer is already initialized");
    }
    if (InitializationPending()) {
        return ACS_ERR(
            Render, 584,
            "Volumetric-cloud staged initialization is already active");
    }
    if (m_NoiseFilterResources) {
        return ACS_ERR(
            Render, 585,
            "Volumetric-cloud renderer owns incomplete initialization data");
    }
    if (!device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 586,
            "Volumetric-cloud staged initialization requires asynchronous "
            "shader compilation");
    }

    auto noise_filter = MakeUnique<FNoiseFilterResources>();
    if (!noise_filter) {
        return ACS_ERR(
            Memory, 587,
            "雲の段階初期化に必要な形状フィルター所有領域を確保できません");
    }
    auto shaders = CreateCloudShaderSet(device, true, false);
    if (shaders.IsErr()) {
        return Err<void>(shaders.Error());
    }

    FCompiledShaders pending = Move(shaders.Value());
    m_CloudCs = Move(pending.cloud);
    m_NoiseCs = Move(pending.noise);
    noise_filter->shader = Move(pending.noise_filter);
    m_NoiseFilterResources = Move(noise_filter);
    m_WeatherCs = Move(pending.weather);
    m_DetailCs = Move(pending.detail);
    m_CurlCs = Move(pending.curl);
    m_CompVs = Move(pending.composite_vertex);
    m_CompPs = Move(pending.composite_pixel);
    m_CompAtmosPs = Move(pending.composite_atmosphere_pixel);
    m_ResolveCs = Move(pending.resolve);
    m_HdrFormat = hdr_format;
    m_NoiseFilterResources->resource_device = &device;
    m_NoiseFilterResources->initialization_state =
        EAsyncInitializationState::MandatoryShaders;
    ACS_LOG_INFO(
        "CVolumetricClouds: 雲本体の非同期コンパイルを開始しました");
    return Ok();
}

TResult<bool> CVolumetricClouds::AdvanceInitialization(
    IRhiDevice& device) noexcept {
    if (m_Ready) {
        if (IsInitializedForDevice(device)) {
            return TResult<bool>(OkInit, true);
        }
        return ACS_ERR(
            Render, 592,
            "Volumetric-cloud initialized device changed");
    }
    if (!InitializationPending() || !m_NoiseFilterResources) {
        return ACS_ERR(
            Render, 588,
            "Volumetric-cloud staged initialization has not started");
    }
    if (m_NoiseFilterResources->resource_device != &device) {
        return ACS_ERR(
            Render, 589,
            "Volumetric-cloud staged initialization device changed");
    }
    EAsyncInitializationState& state =
        m_NoiseFilterResources->initialization_state;

    if (state == EAsyncInitializationState::MandatoryShaders) {
        const EShaderStatus status =
            PendingMandatoryShaderStatus_Internal();
        if (status == EShaderStatus::Compiling) {
            return TResult<bool>(OkInit, false);
        }
        if (status != EShaderStatus::Ready) {
            Shutdown();
            return ACS_ERR(
                Render, 590,
                "Volumetric-cloud mandatory asynchronous compilation failed");
        }

        if (kVolumetricCloudShadowCacheEnabled) {
            auto shadow = CreateCloudShaderHandle(
                device, EShaderStage::Compute, kCloudCS,
                "CSCloudShadow", "Clouds.ShadowCacheCS", true);
            if (shadow.IsOk()) {
                m_ShadowCs = Move(shadow.Value());
                state = EAsyncInitializationState::ShadowShader;
                ACS_LOG_INFO(
                    "CVolumetricClouds: 雲本体の完了後に光キャッシュの"
                    "非同期コンパイルを開始しました");
                return TResult<bool>(OkInit, false);
            }
            ACS_LOG_WARN(
                "CVolumetricClouds: 光キャッシュの非同期投入に失敗したため、"
                "正確な光積分へ戻します: %s",
                shadow.Error().message);
        }
        state = EAsyncInitializationState::ShadowShader;
    }

    if (state == EAsyncInitializationState::ShadowShader) {
        if (m_ShadowCs) {
            const EShaderStatus status = m_ShadowCs->Status();
            if (status == EShaderStatus::Compiling) {
                return TResult<bool>(OkInit, false);
            }
            if (status != EShaderStatus::Ready) {
                m_ShadowCs.Reset();
                ACS_LOG_WARN(
                    "CVolumetricClouds: 光キャッシュのコンパイルに失敗したため、"
                    "正確な光積分へ戻します");
            }
        }

        if (kVolumetricCloudWorldShadowEnabled) {
            auto world_shadow = CreateCloudShaderHandle(
                device, EShaderStage::Compute, kCloudCS,
                "CSCloudWorldShadow", "Clouds.WorldShadowCS", true);
            if (world_shadow.IsOk()) {
                m_WorldShadowCs = Move(world_shadow.Value());
                state = EAsyncInitializationState::WorldShadowShader;
                ACS_LOG_INFO(
                    "CVolumetricClouds: 光キャッシュの完了後に立体物用雲影の"
                    "非同期コンパイルを開始しました");
                return TResult<bool>(OkInit, false);
            }
            ACS_LOG_WARN(
                "CVolumetricClouds: 立体物用雲影の非同期投入に失敗したため、"
                "直接光の雲遮蔽を無効にします: %s",
                world_shadow.Error().message);
        }
        state = EAsyncInitializationState::WorldShadowShader;
    }

    if (m_WorldShadowCs) {
        const EShaderStatus status = m_WorldShadowCs->Status();
        if (status == EShaderStatus::Compiling) {
            return TResult<bool>(OkInit, false);
        }
        if (status != EShaderStatus::Ready) {
            m_WorldShadowCs.Reset();
            ACS_LOG_WARN(
                "CVolumetricClouds: 立体物用雲影のコンパイルに失敗したため、"
                "直接光の雲遮蔽を無効にします");
        }
    }

    FCompiledShaders completed = TakePendingShaders_Internal();
    auto initialized = InitWithCompiledShaders(
        device, Move(completed), m_HdrFormat);
    if (initialized.IsErr()) {
        return Err<bool>(initialized.Error());
    }
    return TResult<bool>(OkInit, true);
}

TResult<void> CVolumetricClouds::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat hdr_format) noexcept {
    if (InitializationPending()) {
        return ACS_ERR(
            Render, 591,
            "Volumetric-cloud staged initialization is already active");
    }
    if (!shaders.cloud || !shaders.noise || !shaders.noise_filter ||
        !shaders.weather ||
        !shaders.detail || !shaders.curl || !shaders.composite_vertex ||
        !shaders.composite_pixel ||
        !shaders.composite_atmosphere_pixel || !shaders.resolve) {
        return ACS_ERR(
            Render, 580,
            "Volumetric-cloud compiled shader set is incomplete");
    }
    if (m_Ready && !IsInitializedForDevice(device)) {
        return ACS_ERR(
            Render, 593,
            "Volumetric-cloud resources belong to another device");
    }

    CVolumetricClouds candidate;
    // 公開設定は初期化前にも受け付ける。候補へ全設定を引き継いでから資源を作り、
    // 初回初期化と再初期化のどちらでも利用側の天候や照明を既定値へ戻さない。
    candidate.m_Layer = m_Layer;
    candidate.m_ReferenceMode = m_ReferenceMode;
    candidate.m_Lighting = m_Lighting;
    candidate.m_Weather = m_Weather;
    candidate.m_Range = m_Range;
    candidate.m_UpperLayer = m_UpperLayer;
    auto result = candidate.InitCandidateWithCompiledShaders(
        device, Move(shaders), hdr_format);
    if (result.IsErr()) return result;

    // Candidate construction above owns every mandatory resource until the
    // final non-failing commit. A failed rebuild therefore preserves the
    // complete previously published cloud renderer.
    if (m_Ready) device.WaitIdle();
    Shutdown();
    *this = Move(candidate);
    return Ok();
}

TResult<void> CVolumetricClouds::InitCandidateWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat hdr_format) noexcept {
    m_HdrFormat = hdr_format;
    m_CloudCs = Move(shaders.cloud);
    {   FComputePipelineDesc pd{}; pd.cs = m_CloudCs.Get();
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "CloudCB";
        pd.uav_slots = 2; pd.uav_names[0] = "cloudOut"; pd.uav_names[1] = "cloudDepthOut";
        pd.srv_slots = 6;
        pd.srv_names[0] = "shapeNoise";
        pd.srv_names[1] = "shapeOccupancy";
        pd.srv_names[2] = "weatherMap";
        pd.srv_names[3] = "detailNoise";
        pd.srv_names[4] = "curlNoise";
        pd.srv_names[5] = "cloudShadowCache";
        pd.static_sampler_count = 6;
        for (u32 i = 0; i < 5; ++i) {
            pd.static_samplers[i].filter = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
            pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
            pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
        }
        // 最大値階層を線形補間すると真の最大値を下回るため、占有だけpoint採取する。
        pd.static_samplers[1].filter = ESamplerFilter::Point;
        pd.static_samplers[5].filter = ESamplerFilter::Linear;
        pd.static_samplers[5].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[5].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[5].address_w = ESamplerAddress::Clamp;
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CloudPipe = Move(r.Value()); }
    // 任意機能である周囲光透過率と太陽円盤4光路を、一セル一グループで生成する。
    // 作成できない場合は自己影キャッシュだけを無効化し、正確な積分へ戻す。
    {
        bool shadowOk = kVolumetricCloudShadowCacheEnabled &&
            shaders.shadow;
        if (shadowOk) {
            m_ShadowCs = Move(shaders.shadow);
        }
        const auto createShadowSamplingPipeline =
            [&device](IRhiShader* shader) noexcept {
                FComputePipelineDesc pd{};
                pd.cs = shader;
                pd.cbuffer_slots = 1;
                pd.cbuffer_names[0] = "CloudCB";
                pd.srv_slots = 5;
                pd.srv_names[0] = "shapeNoise";
                pd.srv_names[1] = "shapeOccupancy";
                pd.srv_names[2] = "weatherMap";
                pd.srv_names[3] = "detailNoise";
                pd.srv_names[4] = "curlNoise";
                // 現在のRHIは登録番号の連続配置を要求する。u0/u1には無害な
                // 代替テクスチャを割り当て、実際の出力はu2へ書く。
                pd.uav_slots = 3;
                pd.uav_names[0] = "cloudOut";
                pd.uav_names[1] = "cloudDepthOut";
                pd.uav_names[2] = "cloudShadowOut";
                pd.static_sampler_count = 5;
                for (u32 i = 0; i < 5; ++i) {
                    pd.static_samplers[i].filter = ESamplerFilter::Linear;
                    pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
                    pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
                    pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
                }
                pd.static_samplers[1].filter = ESamplerFilter::Point;
                return CreateRhiComputePipeline(device, pd);
            };
        if (shadowOk) {
            auto pipeResult =
                createShadowSamplingPipeline(m_ShadowCs.Get());
            if (pipeResult.IsErr()) {
                shadowOk = false;
            } else {
                m_ShadowPipe = Move(pipeResult.Value());
            }
        }
        if (shadowOk) {
            FTextureDesc td{};
            td.width = kVolumetricCloudShadowCacheWidth;
            td.height = 4u * kVolumetricCloudShadowCacheHeight;
            td.depth = kVolumetricCloudShadowCacheDepth;
            td.format = EFormat::R16G16B16A16_Float;
            td.is_uav = true;
            auto textureResult = CreateRhiTexture(device, td);
            if (textureResult.IsErr()) {
                shadowOk = false;
            } else {
                m_ShadowTex = Move(textureResult.Value());
            }
        }
        if (!shadowOk) {
            m_ShadowTex.Reset();
            m_ShadowPipe.Reset();
            m_ShadowCs.Reset();
            if (kVolumetricCloudShadowCacheEnabled) {
                ACS_LOG_WARN(
                    "CVolumetricClouds: optional cloud-light cache unavailable; "
                    "exact lighting fallback remains active");
            }
        }
        m_ShadowCacheAvailable = shadowOk;
        m_ShadowCacheValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        m_ShadowGridInitialized = false;
        m_ShadowCacheDispatchCount = 0;
    }
    // 立体物用の雲影は独立した二次元透過率地図へ生成する。作成できない場合も
    // 雲そのものは描き、PBR側は透過率1の代替処理へ戻す。
    {
        bool worldShadowOk =
            kVolumetricCloudWorldShadowEnabled && shaders.world_shadow;
        if (worldShadowOk) {
            m_WorldShadowCs = Move(shaders.world_shadow);
            FComputePipelineDesc pd{};
            pd.cs = m_WorldShadowCs.Get();
            pd.cbuffer_slots = 1;
            pd.cbuffer_names[0] = "CloudCB";
            pd.srv_slots = 5;
            pd.srv_names[0] = "shapeNoise";
            pd.srv_names[1] = "shapeOccupancy";
            pd.srv_names[2] = "weatherMap";
            pd.srv_names[3] = "detailNoise";
            pd.srv_names[4] = "curlNoise";
            pd.uav_slots = 1;
            pd.uav_names[0] = "cloudOut";
            pd.static_sampler_count = 5;
            for (u32 i = 0; i < 5; ++i) {
                pd.static_samplers[i].filter = ESamplerFilter::Linear;
                pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
            }
            pd.static_samplers[1].filter = ESamplerFilter::Point;
            auto pipelineResult = CreateRhiComputePipeline(device, pd);
            if (pipelineResult.IsErr()) {
                worldShadowOk = false;
            } else {
                m_WorldShadowPipe = Move(pipelineResult.Value());
            }
        }
        if (worldShadowOk) {
            FTextureDesc td{};
            td.width = kVolumetricCloudWorldShadowMapResolution;
            td.height = kVolumetricCloudWorldShadowMapResolution;
            td.format = EFormat::R16G16B16A16_Float;
            td.is_uav = true;
            auto textureResult = CreateRhiTexture(device, td);
            if (textureResult.IsErr()) {
                worldShadowOk = false;
            } else {
                m_WorldShadowTex = Move(textureResult.Value());
            }
        }
        if (!worldShadowOk) {
            m_WorldShadowTex.Reset();
            m_WorldShadowPipe.Reset();
            m_WorldShadowCs.Reset();
            if (kVolumetricCloudWorldShadowEnabled) {
                ACS_LOG_WARN("CVolumetricClouds: 立体物用の雲影を利用できないため、直接光は雲で遮らずに描画します");
            }
        }
        m_WorldShadowAvailable = worldShadowOk;
        m_WorldShadowValid = false;
        SetWorldShadowWarmupMask_Internal(0u);
        m_WorldShadowDispatchCount = 0u;
    }
    // Perlin-Worley完成形状を生成し、128角のまま各軸へ探索用最大値を作る。
    {
        m_NoiseCs = Move(shaders.noise);
        m_NoiseFilterResources = MakeUnique<FNoiseFilterResources>();
        if (!m_NoiseFilterResources) {
            return ACS_ERR(
                Memory, 581,
                "雲形状フィルター資源の所有領域を確保できません");
        }
        m_NoiseFilterResources->shader = Move(shaders.noise_filter);
        m_NoiseFilterResources->resource_device = &device;
    }
    {
        FComputePipelineDesc pd{};
        pd.cs = m_NoiseCs.Get();
        pd.uav_slots = 2;
        pd.uav_names[0] = "shapeSourceOut";
        pd.uav_names[1] = "shapeOccupancySourceOut";
        auto r = CreateRhiComputePipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_NoisePipe = Move(r.Value());
    }
    {
        FComputePipelineDesc pd{};
        pd.cs = m_NoiseFilterResources->shader.Get();
        pd.srv_slots = 1;
        pd.srv_names[0] = "shapeFilterSource";
        pd.uav_slots = 1;
        pd.uav_names[0] = "shapeFilterOut";
        auto r = CreateRhiComputePipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_NoiseFilterResources->pipeline = Move(r.Value());
    }
    {
        FTextureDesc td{};
        td.width = 128;
        td.height = 128;
        td.depth = 128;
        td.format = EFormat::R8G8B8A8_UNorm;
        td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_NoiseFilterResources->source_texture = Move(r.Value());
    }
    {
        FTextureDesc td{};
        td.width = 128;
        td.height = 128;
        td.depth = 128;
        td.format = EFormat::R8G8B8A8_UNorm;
        td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_NoiseFilterResources->filtered_texture = Move(r.Value());
    }
    {
        FTextureDesc td{};
        td.width = 128;
        td.height = 128;
        td.depth = 128;
        td.format = EFormat::R16G16B16A16_Float;
        td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_ShapeTex = Move(r.Value());
    }
    {
        m_WeatherCs = Move(shaders.weather);
    }
    {
        FComputePipelineDesc pd{}; pd.cs = m_WeatherCs.Get();
        pd.uav_slots = 1; pd.uav_names[0] = "weatherOut";
        auto r = CreateRhiComputePipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_WeatherPipe = Move(r.Value());
    }
    {
        FTextureDesc td{}; td.width = 512; td.height = 512;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_WeatherTex = Move(r.Value());
    }
    {
        m_DetailCs = Move(shaders.detail);
    }
    {
        FComputePipelineDesc pd{}; pd.cs = m_DetailCs.Get();
        pd.uav_slots = 1; pd.uav_names[0] = "detailOut";
        auto r = CreateRhiComputePipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_DetailPipe = Move(r.Value());
    }
    {
        FTextureDesc td{}; td.width = 64; td.height = 64; td.depth = 64;
        td.format = EFormat::R16G16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_DetailTex = Move(r.Value());
    }
    {
        m_CurlCs = Move(shaders.curl);
    }
    {
        FComputePipelineDesc pd{}; pd.cs = m_CurlCs.Get();
        pd.uav_slots = 1; pd.uav_names[0] = "curlOut";
        auto r = CreateRhiComputePipeline(device, pd);
        if (r.IsErr()) return Err<void>(r.Error());
        m_CurlPipe = Move(r.Value());
    }
    {
        FTextureDesc td{}; td.width = 128; td.height = 128;
        td.format = EFormat::R16G16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return Err<void>(r.Error());
        m_CurlTex = Move(r.Value());
    }
    m_CompVs = Move(shaders.composite_vertex);
    m_CompPs = Move(shaders.composite_pixel);
    m_CompAtmosPs = Move(shaders.composite_atmosphere_pixel);
    m_ResolveCs = Move(shaders.resolve);
    {   FComputePipelineDesc pd{};
        pd.cs = m_ResolveCs.Get();
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "CloudCB";
        pd.srv_slots = 4;
        pd.srv_names[0] = "cloudLow";
        pd.srv_names[1] = "cloudDepth";
        pd.srv_names[2] = "historyColor";
        pd.srv_names[3] = "historyDepth";
        pd.uav_slots = 2;
        pd.uav_names[0] = "historyColorOut";
        pd.uav_names[1] = "historyDepthOut";
        pd.static_sampler_count = 4;
        for (u32 i = 0; i < 4; ++i) {
            pd.static_samplers[i].filter = (i == 2) ? ESamplerFilter::Linear : ESamplerFilter::Point;
            pd.static_samplers[i].address_u = ESamplerAddress::Clamp;
            pd.static_samplers[i].address_v = ESamplerAddress::Clamp;
        }
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ResolvePipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_CompVs.Get(); pd.ps = m_CompPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AlphaBlend;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "CloudCB";
        pd.texture_slots = 3;
        pd.texture_names[0] = "cloudTex";
        pd.texture_names[1] = "sceneDepth";
        pd.texture_names[2] = "cloudDepth";
        pd.static_sampler_count = 3;
        pd.static_samplers[0].filter = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Point;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[2].filter = ESamplerFilter::Point;
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CompPipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_CompVs.Get(); pd.ps = m_CompAtmosPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AlphaBlend;
        pd.cbuffer_slots = 2;
        pd.cbuffer_names[0] = "CloudCB";
        pd.cbuffer_names[1] = "CloudAtmosphereCB";
        pd.texture_slots = 5;
        pd.texture_names[0] = "cloudTex";
        pd.texture_names[1] = "sceneDepth";
        pd.texture_names[2] = "cloudDepth";
        pd.texture_names[3] = "atmosphereVolume";
        pd.texture_names[4] = "atmosphereTransmittance";
        pd.static_sampler_count = 5;
        pd.static_samplers[0].filter = ESamplerFilter::Linear;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Point;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[2].filter = ESamplerFilter::Point;
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[3].filter = ESamplerFilter::Linear;
        pd.static_samplers[3].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[3].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[3].address_w = ESamplerAddress::Clamp;
        pd.static_samplers[4].filter = ESamplerFilter::Linear;
        pd.static_samplers[4].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[4].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[4].address_w = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CompAtmosPipe = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = CBSize<FCloudCb>(); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_Cb = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = CBSize<FCloudAtmosphereCb>(); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_CompAtmosCb = Move(r.Value()); }
    m_Ready = true;
    ACS_LOG_INFO("CVolumetricClouds: compute raymarch, bilateral resolve, and temporal reprojection initialized");
    return Ok();
}

bool CVolumetricClouds::EnsureSize(IRhiDevice& device, u32 scW, u32 scH,
                                   f32 render_scale,
                                   bool reference_mode) noexcept {
    if (!IsInitializedForDevice(device) || RecordedFramePending()) {
        return false;
    }
    const u32 fw = scW > 0 ? scW : 1;
    const u32 fh = scH > 0 ? scH : 1;
    const FVolumetricCloudTraceResolution traceResolution =
        ResolveVolumetricCloudTraceResolution(fw, fh, render_scale, reference_mode);
    const u32 hw = traceResolution.width;
    const u32 hh = traceResolution.height;
    if (m_CloudTex && m_CloudDepth && m_HistoryColor[0] && m_HistoryColor[1] &&
        m_HistoryDepth[0] && m_HistoryDepth[1] &&
        m_W == hw && m_H == hh && m_FullW == fw && m_FullH == fh) return true;

    auto make_texture = [&device](u32 w, u32 h, EFormat format, bool uav, bool rt,
                                  TUniquePtr<IRhiTexture>& out) noexcept -> bool {
        FTextureDesc td{};
        td.width = w; td.height = h; td.format = format;
        td.is_uav = uav; td.is_render_target = rt;
        auto r = CreateRhiTexture(device, td);
        if (r.IsErr()) return false;
        out = Move(r.Value());
        return true;
    };

    TUniquePtr<IRhiTexture> lowColor, lowDepth;
    TUniquePtr<IRhiTexture> historyColor[2], historyDepth[2];
    if (!make_texture(hw, hh, EFormat::R16G16B16A16_Float, true, false, lowColor) ||
        !make_texture(hw, hh, EFormat::R32G32_Float, true, false, lowDepth) ||
        !make_texture(fw, fh, EFormat::R16G16B16A16_Float, true, false, historyColor[0]) ||
        !make_texture(fw, fh, EFormat::R16G16B16A16_Float, true, false, historyColor[1]) ||
        !make_texture(fw, fh, EFormat::R32G32_Float, true, false, historyDepth[0]) ||
        !make_texture(fw, fh, EFormat::R32G32_Float, true, false, historyDepth[1])) {
        return false; // 旧サイズを保持。呼び側はこのframeをCSky fallbackにする。
    }

    // Resource creation above is transactional: only after every replacement
    // exists do we wait for in-flight frames that may still reference the old
    // cloud textures.  Initial allocation has no old GPU ownership to drain,
    // and the same-size fast path returned before doing any work.
    const bool replacingExistingTextures =
        m_CloudTex || m_CloudDepth ||
        m_HistoryColor[0] || m_HistoryColor[1] ||
        m_HistoryDepth[0] || m_HistoryDepth[1];
    if (replacingExistingTextures) device.WaitIdle();

    m_CloudTex = Move(lowColor);
    m_CloudDepth = Move(lowDepth);
    for (u32 i = 0; i < 2; ++i) {
        m_HistoryColor[i] = Move(historyColor[i]);
        m_HistoryDepth[i] = Move(historyDepth[i]);
    }
    m_W = hw; m_H = hh; m_FullW = fw; m_FullH = fh;
    // 画面履歴の再確保は影キャッシュの更新位相を変えない。位相を先頭へ戻すと
    // 同じ偶奇位置を重ねて更新し、別の位置が四世代以上古くなる。
    m_TemporalPhase = 0;
    m_ResolvedIndex = 0; m_HistoryValid = false;
    m_WorldShadowValid = false;
    SetWorldShadowWarmupMask_Internal(0u);
    m_WorldOrigin = FVec3{};
    m_PrevCameraRelativeViewProj = FMat4::Identity();
    m_PrevCameraRelativeInvViewProj = FMat4::Identity();
    m_PrevCamPos = FVec3{};
    m_PrevSunDir = FVec3{};
    m_PrevSunColor = FVec3{};
    m_PrevSkyColor = FVec3{};
    if (m_NoiseFilterResources) {
        m_NoiseFilterResources->previous_lighting = FVolumetricCloudLighting{};
    }
    m_PrevWindOffset = 0.0f; m_PrevWindSpeed = 0.0f;
    m_PrevCoverage = -1.0f; m_PrevDensity = -1.0f; m_PrevTime = -1.0f;
    return true;
}

u32 CVolumetricClouds::EnvironmentLightingSignature(
        f32 coverage, f32 density, f32 wind) const noexcept {
    auto canonical = [](f32 value) noexcept {
        return value == 0.0f ? 0.0f : value;
    };
    const f32 safe_coverage = canonical(
        SanitizeCloudScalar(coverage, 0.0f, 0.0f, 1.0f));
    const f32 safe_density = canonical(
        SanitizeCloudScalar(density, 1.0f, 0.05f, 8.0f));
    const f32 safe_wind = canonical(
        SanitizeCloudScalar(wind, 0.0f, -20.0f, 20.0f));

    u32 hash = 2166136261u;
    const auto add_float = [&hash, canonical](f32 value) noexcept {
        hash = HashCloudEnvironmentFloat(hash, canonical(value));
    };
    const auto add_radiance = [&hash](f32 value) noexcept {
        hash = HashCloudEnvironmentQuantizedFloat(hash, value, kCloudEnvironmentRadianceSignatureStep);
    };
    add_float(m_Layer.base_height);
    add_float(m_Layer.top_height);
    add_float(m_Layer.horizontal_noise_scale);
    hash = HashCloudEnvironmentQuantizedFloat(hash, safe_coverage, kCloudEnvironmentCoverageSignatureStep);
    hash = HashCloudEnvironmentQuantizedFloat(hash, safe_density, kCloudEnvironmentDensitySignatureStep);
    hash = HashCloudEnvironmentQuantizedFloat(hash, safe_wind, kCloudEnvironmentWindSignatureStep);
    add_float(m_Lighting.ViewExtinction);
    add_float(m_Lighting.LightExtinction);
    add_float(m_Lighting.SunScatter);
    add_float(m_Lighting.SunScatteringLuminanceScale);
    add_float(m_Lighting.PowderStrength);
    add_float(m_Lighting.PhaseForward);
    add_float(m_Lighting.PhaseBackward);
    add_float(m_Lighting.PhaseBlend);
    add_float(m_Lighting.PhaseMin);
    add_float(m_Lighting.PhaseMax);
    add_float(m_Lighting.MultiScatterContribution);
    add_float(m_Lighting.MultiScatterOcclusion);
    add_float(m_Lighting.MultiScatterEccentricity);
    add_float(m_Lighting.AmbientAtBase);
    add_float(m_Lighting.AmbientAtTop);
    add_float(m_Lighting.GroundContribution);
    add_float(m_Lighting.GroundColor.x);
    add_float(m_Lighting.GroundColor.y);
    add_float(m_Lighting.GroundColor.z);
    // 環境cubemap shaderが実際に使う固定方向光と空の放射輝度も追跡する。
    // 日周や天候の連続補間は呼び側の固定frame間隔でまとめて反映される。
    add_radiance(m_PrevSunColor.x);
    add_radiance(m_PrevSunColor.y);
    add_radiance(m_PrevSunColor.z);
    add_radiance(m_PrevSkyColor.x);
    add_radiance(m_PrevSkyColor.y);
    add_radiance(m_PrevSkyColor.z);
    add_radiance(m_Lighting.SkyZenithColor.x);
    add_radiance(m_Lighting.SkyZenithColor.y);
    add_radiance(m_Lighting.SkyZenithColor.z);
    add_radiance(m_Lighting.SunTransmittance.x);
    add_radiance(m_Lighting.SunTransmittance.y);
    add_radiance(m_Lighting.SunTransmittance.z);
    add_float(m_Range.MaxDistance);
    add_float(m_Range.FadeFraction);
    add_float(m_Range.StepGrowth);
    hash = HashCloudEnvironmentWord(hash, m_Range.ViewSteps);
    add_float(m_UpperLayer.base_height);
    add_float(m_UpperLayer.top_height);
    add_float(m_UpperLayer.coverage_scale);
    add_float(m_UpperLayer.density_scale);
    // 環境cubemapも現在高度の局所視程を使う。層外で焼いたIBLを雲中へ
    // 持ち越さないよう、進入と退出による見える距離の変化を署名へ含める。
    const bool has_upper_layer =
        m_UpperLayer.top_height > m_UpperLayer.base_height &&
        m_UpperLayer.base_height >= m_Layer.top_height;
    const FVec3 world_origin =
        RebaseVolumetricCloudWorldOrigin(m_PrevCamPos);
    const FVec3 shell_local_origin{m_PrevCamPos.x - world_origin.x, m_PrevCamPos.y - world_origin.y, m_PrevCamPos.z - world_origin.z};
    const f32 current_view_distance = ResolveVolumetricCloudViewDistance_Internal(shell_local_origin, m_Layer, m_UpperLayer, has_upper_layer, m_Range.MaxDistance);
    hash = HashCloudEnvironmentQuantizedFloat(hash, current_view_distance, kCloudEnvironmentViewDistanceSignatureStep);
    return hash != 0u ? hash : 1u;
}

u32 CVolumetricClouds::EnvironmentLightingUpdateSignature(f32 coverage, f32 density, f32 wind, u64 submission_index) const noexcept {
    // 連続時刻を直接混ぜず、固定間隔ごとの世代だけを設定署名へ加える。
    const u64 updateGeneration = submission_index / kVolumetricCloudEnvironmentRefreshInterval;
    u32 hash = EnvironmentLightingSignature(coverage, density, wind);
    hash = HashCloudEnvironmentWord(hash, static_cast<u32>(updateGeneration));
    hash = HashCloudEnvironmentWord(hash, static_cast<u32>(updateGeneration >> 32u));
    return hash != 0u ? hash : 1u;
}

u32 CVolumetricClouds::RenderedEnvironmentLightingUpdateSignature() const noexcept {
    if (!m_HistoryValid || !m_LastFrameWorkload.submitted || m_LastFrameWorkload.submission_index == 0u) {
        return 0u;
    }
    return EnvironmentLightingUpdateSignature(m_PrevCoverage, m_PrevDensity, m_PrevWindSpeed, m_LastFrameWorkload.submission_index);
}

bool CVolumetricClouds::IsEnvironmentLightingRefreshFrame(u64 submission_index) noexcept {
    return submission_index != 0u && submission_index % kVolumetricCloudEnvironmentRefreshInterval == 0u;
}

void CVolumetricClouds::RenderCompute(IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color, f32 coverage, f32 density, f32 wind, f32 time, u64 submission_id) noexcept {
    // 既存の呼び出し元との互換性を保つ。新しい呼び出し元は、精度を保てる
    // ビュー行列から作った camera_relative_inv_view_proj を直接渡す。
    const FMat4 cameraRelativeInverseViewProjection = inv_view_proj * FMat4::Translation(FVec3{-cam_pos.x, -cam_pos.y, -cam_pos.z});
    RenderComputeCameraRelative(cl, cameraRelativeInverseViewProjection, cam_pos, sun_dir, sun_color, sky_color, coverage, density, wind, time, submission_id);
}

void CVolumetricClouds::RenderComputeCameraRelative(IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color, f32 coverage, f32 density, f32 wind, f32 time, u64 submission_id) noexcept {
    // 同じ命令一覧をSubmitする前に再度呼ばれても、段階生成を一提出へ再集中させない。
    // 呼び側はResolveRecordedFrameSubmission()で前回結果を確定してから次を記録する。
    if (RecordedFramePending()) return;
    const bool historyWasAvailable = m_HistoryValid;
    m_LastFrameWorkload = {};
    m_LastFrameWorkload.attempted = true;
    m_LastFrameWorkload.history_was_available =
        historyWasAvailable;
    if (!m_Ready || !m_CloudTex || !m_CloudDepth || !m_HistoryColor[0] ||
        !m_HistoryColor[1] || !m_HistoryDepth[0] || !m_HistoryDepth[1] ||
        !m_CloudPipe || !m_ResolvePipe || !m_Cb ||
        !m_WeatherPipe || !m_WeatherTex ||
        !m_DetailPipe || !m_DetailTex || !m_CurlPipe || !m_CurlTex) {
        m_ShadowCacheValid = false;
        m_WorldShadowValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        SetWorldShadowWarmupMask_Internal(0u);
        m_LastFrameWorkload.skip_reason =
            EVolumetricCloudFrameSkipReason::ResourcesNotReady;
        return;
    }
    // 壊れたカメラ変換からは有効な視線を作れず、履歴全体へ非数が広がるため、
    // このフレームの雲だけを止めて通常の空を残す。
    if (!IsFiniteCloudMatrix(camera_relative_inv_view_proj) || !IsFiniteCloudVector(cam_pos)) {
        m_HistoryValid = false;
        m_ShadowCacheValid = false;
        m_WorldShadowValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        SetWorldShadowWarmupMask_Internal(0u);
        m_LastFrameWorkload.skip_reason =
            EVolumetricCloudFrameSkipReason::InvalidCamera;
        m_LastFrameWorkload.history_invalidated =
            historyWasAvailable;
        return;
    }
    const FMat4 cameraRelativeViewProj =
        Inverse(camera_relative_inv_view_proj);
    if (!IsFiniteCloudMatrix(cameraRelativeViewProj)) {
        m_HistoryValid = false;
        m_ShadowCacheValid = false;
        m_WorldShadowValid = false;
        SetShadowCacheWarmupMask_Internal(0u);
        SetWorldShadowWarmupMask_Internal(0u);
        m_LastFrameWorkload.skip_reason =
            EVolumetricCloudFrameSkipReason::InvalidProjection;
        m_LastFrameWorkload.history_invalidated =
            historyWasAvailable;
        return;
    }
    const FVolumetricCloudLightBasis lightBasis =
        ResolveVolumetricCloudLightBasis(sun_dir);
    const FVec3 safeSun = lightBasis.direction;
    const f32 fallbackCoverage =
        m_HistoryValid && std::isfinite(m_PrevCoverage)
            ? m_PrevCoverage : 0.5f;
    const f32 fallbackDensity =
        m_HistoryValid && std::isfinite(m_PrevDensity)
            ? m_PrevDensity : 1.0f;
    const f32 fallbackWind =
        m_HistoryValid && std::isfinite(m_PrevWindSpeed)
            ? m_PrevWindSpeed : 0.0f;
    const f32 fallbackTime =
        m_HistoryValid && std::isfinite(m_PrevTime)
            ? m_PrevTime : 0.0f;
    const f32 finiteCoverage = std::isfinite(coverage)
                             ? coverage : fallbackCoverage;
    const f32 finiteDensity = std::isfinite(density)
                            ? density : fallbackDensity;
    const f32 finiteWind = std::isfinite(wind) ? wind : fallbackWind;
    const f32 finiteTime = std::isfinite(time) ? time : fallbackTime;
    // Bound even finite hostile values before time*wind is evaluated. Normal
    // editor sessions remain many orders of magnitude inside this interval.
    const f32 safeTime = finiteTime < -10000000.0f ? -10000000.0f
                       : (finiteTime > 10000000.0f ? 10000000.0f
                                                   : finiteTime);
    const f32 safeCoverage = finiteCoverage < 0.0f ? 0.0f
                           : (finiteCoverage > 1.0f ? 1.0f : finiteCoverage);
    const f32 safeDensity = finiteDensity < 0.05f ? 0.05f
                          : (finiteDensity > 8.0f ? 8.0f : finiteDensity);
    const f32 safeWind = finiteWind < -20.0f ? -20.0f
                       : (finiteWind > 20.0f ? 20.0f : finiteWind);
    const FVec3 safeSunColor = SanitizeCloudRadiance(
        sun_color, m_HistoryValid ? m_PrevSunColor
                                  : FVec3{1.0f, 1.0f, 1.0f});
    const FVec3 safeSkyColor = SanitizeCloudRadiance(
        sky_color, m_HistoryValid ? m_PrevSkyColor
                                  : FVec3{0.2f, 0.25f, 0.3f});

    // GPU命令の記録と提出成功を分離する。以下は候補状態だけを進め、Submit失敗時は
    // 本体へ一切公開しないため、同じ密度段階と影位相を安全に再記録できる。
    // 外部IDを内部で生成するとRendererのIDと同じ数値を再利用できるため、未指定の
    // 互換経路は0のまま保持し、Submit直後のbool入口だけで同期確定する。
    FNoiseFilterResources::FRecordedFrameState& recorded =
        m_NoiseFilterResources->recorded_frame;
    recorded = {};
    recorded.command_list = &cl;
    recorded.previous_camera_relative_view_projection =
        m_PrevCameraRelativeViewProj;
    recorded.previous_camera_relative_inverse_view_projection =
        m_PrevCameraRelativeInvViewProj;
    recorded.previous_camera_position = m_PrevCamPos;
    recorded.world_origin = m_WorldOrigin;
    recorded.previous_sun_direction = m_PrevSunDir;
    recorded.previous_sun_color = m_PrevSunColor;
    recorded.previous_sky_color = m_PrevSkyColor;
    recorded.current_lighting = m_Lighting;
    recorded.previous_wind_offset = m_PrevWindOffset;
    recorded.previous_wind_speed = m_PrevWindSpeed;
    recorded.previous_coverage = m_PrevCoverage;
    recorded.previous_density = m_PrevDensity;
    recorded.previous_time = m_PrevTime;
    recorded.shadow_grid_minimum_material_xz = m_ShadowGridMinQ;
    recorded.shadow_grid_center_material_xz = m_ShadowGridCenterQ;
    recorded.world_shadow_map_minimum_reference_xz =
        m_WorldShadowMapMinReferenceXz;
    recorded.world_shadow_reference_height =
        m_WorldShadowReferenceHeight;
    recorded.world_shadow_sun_direction = m_WorldShadowSunDirection;
    recorded.world_shadow_world_origin = m_WorldShadowWorldOrigin;
    recorded.world_shadow_cloud_base_altitude =
        m_WorldShadowCloudBaseAltitude;
    recorded.shadow_cache_dispatch_count = m_ShadowCacheDispatchCount;
    recorded.world_shadow_dispatch_count = m_WorldShadowDispatchCount;
    recorded.workload_submission_index = m_WorkloadSubmissionIndex;
    recorded.submission_id = submission_id;
    recorded.settings_revision =
        m_NoiseFilterResources->settings_revision;
    recorded.frame_index = m_FrameIndex;
    recorded.temporal_phase = m_TemporalPhase;
    recorded.resolved_index = m_ResolvedIndex;
    recorded.shadow_cache_warmup_mask =
        ShadowCacheWarmupMask_Internal();
    recorded.world_shadow_warmup_mask =
        WorldShadowWarmupMask_Internal();
    recorded.density_bake_stage =
        m_NoiseFilterResources->density_bake_stage;
    recorded.noise_baked = m_NoiseBaked;
    recorded.weather_baked = m_WeatherBaked;
    recorded.detail_baked = m_DetailBaked;
    recorded.curl_baked = m_CurlBaked;
    recorded.world_shadow_mapping_initialized =
        m_NoiseFilterResources->world_shadow_mapping_initialized;
    recorded.shadow_grid_initialized = m_ShadowGridInitialized;
    recorded.shadow_cache_valid = m_ShadowCacheValid;
    recorded.world_shadow_valid = m_WorldShadowValid;
    recorded.history_valid = m_HistoryValid;
    recorded.active = true;

    const FVec3 worldOrigin = RebaseVolumetricCloudWorldOrigin(cam_pos);
    const FVec2 worldShadowCenterReferenceXz = ProjectVolumetricCloudWorldShadowReferenceXZ(cam_pos, safeSun, worldOrigin.y);
    const FVec2 nextWorldShadowMapMinReferenceXz = VolumetricCloudWorldShadowMapMinimum(worldShadowCenterReferenceXz);
    bool worldShadowMappingChanged = false;
    constexpr f32 worldShadowSafeRadius =
        kVolumetricCloudWorldShadowMapExtent * 0.25f;
    if (!recorded.world_shadow_mapping_initialized) {
        recorded.world_shadow_map_minimum_reference_xz =
            nextWorldShadowMapMinReferenceXz;
        recorded.world_shadow_mapping_initialized = true;
        worldShadowMappingChanged = true;
    } else {
        const FVec2 currentWorldShadowCenter{
            recorded.world_shadow_map_minimum_reference_xz.x +
                kVolumetricCloudWorldShadowMapExtent * 0.5f,
            recorded.world_shadow_map_minimum_reference_xz.y +
                kVolumetricCloudWorldShadowMapExtent * 0.5f};
        const f32 worldShadowCenterDeltaX =
            worldShadowCenterReferenceXz.x - currentWorldShadowCenter.x;
        const f32 worldShadowCenterDeltaZ =
            worldShadowCenterReferenceXz.y - currentWorldShadowCenter.y;
        if (Abs(worldShadowCenterDeltaX) > worldShadowSafeRadius ||
            Abs(worldShadowCenterDeltaZ) > worldShadowSafeRadius) {
            recorded.world_shadow_map_minimum_reference_xz =
                nextWorldShadowMapMinReferenceXz;
            worldShadowMappingChanged = true;
        }
    }
    recorded.world_shadow_reference_height = worldOrigin.y;
    recorded.world_shadow_sun_direction = safeSun;
    recorded.world_shadow_world_origin = worldOrigin;
    recorded.world_shadow_cloud_base_altitude = m_Layer.base_height;
    // 一つのワールド移流距離を天候、形状、侵食、渦、時間再投影で共有する。
    // 雑音の周波数とは分離し、各領域が風によって互いに滑ることを防ぐ。
    const f32 windOffset = ResolveVolumetricCloudAdvectionDistance(
        safeTime, safeWind);

    const FVolumetricCloudDensityFrameTerms densityFrameTerms =
        ResolveVolumetricCloudDensityFrameTerms(m_Layer, windOffset);
    // 独立領域の相対移動だけを変え、基準領域のワールド固定と風移流は維持する。
    const FVolumetricCloudEvolutionFrameTerms evolutionFrameTerms =
        ResolveVolumetricCloudEvolutionFrameTerms(safeTime, safeWind);
    const FVec2 cameraQ = VolumetricCloudMaterialXZ(cam_pos, windOffset);
    bool shadowGridChanged = false;
    if (!recorded.shadow_grid_initialized) {
        const auto mapping = CenterVolumetricCloudShadowCache(cameraQ);
        recorded.shadow_grid_minimum_material_xz = mapping.min_material_xz;
        recorded.shadow_grid_center_material_xz = mapping.center_material_xz;
        recorded.shadow_grid_initialized = true;
        shadowGridChanged = true;
    } else {
        const f32 dx = cameraQ.x -
            recorded.shadow_grid_center_material_xz.x;
        const f32 dz = cameraQ.y -
            recorded.shadow_grid_center_material_xz.y;
        if (Abs(dx) > kVolumetricCloudShadowCacheSafeRadius ||
            Abs(dz) > kVolumetricCloudShadowCacheSafeRadius) {
            const auto mapping = CenterVolumetricCloudShadowCache(cameraQ);
            recorded.shadow_grid_minimum_material_xz =
                mapping.min_material_xz;
            recorded.shadow_grid_center_material_xz =
                mapping.center_material_xz;
            shadowGridChanged = true;
        }
    }
    if (shadowGridChanged) {
        // 新しい物質座標格子へ切り替えた直後は、旧格子の値を一部でも公開しない。
        recorded.shadow_cache_valid = false;
        recorded.shadow_cache_warmup_mask = 0u;
        recorded.history_valid = false;
    }
    if (worldShadowMappingChanged) {
        // 固定地図の座標が変わった場合も、四つの偶奇位置が揃うまで外部公開しない。
        recorded.world_shadow_valid = false;
        recorded.world_shadow_warmup_mask = 0u;
    }
    const bool shadowResourcesReady =
        m_ShadowCacheAvailable &&
        m_ShadowCs && m_ShadowPipe &&
        m_ShadowTex;
    if (!shadowResourcesReady) {
        recorded.shadow_cache_valid = false;
        recorded.shadow_cache_warmup_mask = 0u;
    }
    const bool worldShadowResourcesReady =
        m_WorldShadowAvailable && m_WorldShadowCs &&
        m_WorldShadowPipe && m_WorldShadowTex;

    // 履歴の無効化: サイズ変更は EnsureSize で扱う。ここでは視点の不連続と、
    // 見える品質設定の不連続だけを拒否する。連続する時刻と風移流は前フレームの
    // 位置へ戻せるため、全画面を破棄せず画素ごとの深度・被覆判定へ渡す。
    // 同一画面の静止高速経路に使う厳しい移動量は、この後で独立して判定する。
    const f32 cameraDx = cam_pos.x - m_PrevCamPos.x;
    const f32 cameraDy = cam_pos.y - m_PrevCamPos.y;
    const f32 cameraDz = cam_pos.z - m_PrevCamPos.z;
    const f32 cameraDeltaSquared =
        cameraDx*cameraDx + cameraDy*cameraDy + cameraDz*cameraDz;
    f32 matrixDelta = 0.0f;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            f32 d = cameraRelativeViewProj.m[r][c] -
                m_PrevCameraRelativeViewProj.m[r][c];
            if (d < 0.0f) d = -d;
            if (d > matrixDelta) matrixDelta = d;
        }
    }
    f32 coverageDelta = safeCoverage - m_PrevCoverage;
    if (coverageDelta < 0.0f) coverageDelta = -coverageDelta;
    f32 densityDelta = safeDensity - m_PrevDensity;
    if (densityDelta < 0.0f) densityDelta = -densityDelta;
    f32 windSpeedDelta = safeWind - m_PrevWindSpeed;
    if (windSpeedDelta < 0.0f) windSpeedDelta = -windSpeedDelta;
    bool historyValid = recorded.history_valid;
    if (historyValid) {
        if (VolumetricCloudViewCutDetected(m_PrevCameraRelativeInvViewProj, m_PrevCamPos, camera_relative_inv_view_proj, cam_pos)) {
            historyValid = false;
        }
        // The soft-snapped tangent origin changes continuously only inside a
        // transition band.  Treat it like ordinary camera motion and let the
        // reprojection depth/alpha tests reject individual stale pixels.
        // Invalidating the whole frame for every sub-unit origin change turns
        // those bands into visibly noisy strips during editor pans.
        if (coverageDelta > 0.001f || densityDelta > 0.001f || windSpeedDelta > 0.001f) historyValid = false;
    }
    // 照明の変化は密度の位置や深度を変えない。CPUで求めた放射輝度・太陽角度の差だけを
    // GPUへ渡し、履歴を捨てずに現在フレームへ連続収束させる。視点切替や雲形状変更は
    // 上の判定で別途履歴を無効化する。
    const f32 lightingMismatch = historyValid
        ? VolumetricCloudLightingTemporalMismatch(
              m_NoiseFilterResources->previous_lighting,
              m_PrevSunDir, m_PrevSunColor,
              m_PrevSkyColor, m_Lighting, safeSun, safeSunColor,
              safeSkyColor)
        : 1.0f;
    // 視点の不連続または設定変更では、空間分散した決定論的な位相列を先頭へ戻す。
    // 交互書き込みの所有権は m_FrameIndex のままなので、再構成のリセットが
    // 資源選択や dispatch の仕事量を変えることはない。
    if (!historyValid) recorded.temporal_phase = 0u;
    // 画面履歴を失っても影の更新世代は独立して残る。影が一部でも存在する場合は
    // 前回の対流状態を使い、視点移動だけで変化量を0へ戻さない。
    const bool previousShadowStateAvailable =
        render_internal::CloudTemporalValueIsFinite_Internal(m_PrevTime) &&
        (recorded.shadow_cache_valid || recorded.world_shadow_valid ||
         recorded.shadow_cache_warmup_mask != 0u ||
         recorded.world_shadow_warmup_mask != 0u);
    // 画面履歴の対流差は影キャッシュの準備状態に依存させない。影がまだ温まって
    // いない起動直後でも、前フレームの雲形状変化を画面再構成へ正しく伝える。
    const bool previousEvolutionStateAvailable =
        historyValid &&
        render_internal::CloudTemporalValueIsFinite_Internal(m_PrevTime);
    const FVolumetricCloudEvolutionFrameTerms previousEvolutionFrameTerms =
        previousEvolutionStateAvailable
            ? ResolveVolumetricCloudEvolutionFrameTerms(m_PrevTime, m_PrevWindSpeed)
            : evolutionFrameTerms;
    const FVolumetricCloudEvolutionFrameTerms previousShadowEvolutionFrameTerms =
        previousShadowStateAvailable
            ? ResolveVolumetricCloudEvolutionFrameTerms(m_PrevTime, m_PrevWindSpeed)
            : evolutionFrameTerms;
    const render_internal::FVolumetricCloudShadowTemporalDecision shadowTemporalDecision = render_internal::ResolveVolumetricCloudShadowTemporalDecision(recorded.frame_index, evolutionFrameTerms, previousShadowEvolutionFrameTerms, windOffset, m_PrevWindOffset);

    const bool bakeShapeNoiseThisFrame =
        !recorded.noise_baked && m_NoisePipe && m_NoiseFilterResources &&
        m_NoiseFilterResources->pipeline &&
        m_NoiseFilterResources->source_texture &&
        m_NoiseFilterResources->filtered_texture && m_ShapeTex;
    const bool bakeWeatherThisFrame =
        recorded.noise_baked && !recorded.weather_baked &&
        m_WeatherPipe && m_WeatherTex;
    const bool bakeDetailNoiseThisFrame =
        recorded.noise_baked && recorded.weather_baked &&
        !recorded.detail_baked &&
        m_DetailPipe && m_DetailTex;
    const bool bakeCurlNoiseThisFrame =
        recorded.noise_baked && recorded.weather_baked &&
        recorded.detail_baked && !recorded.curl_baked &&
        m_CurlPipe && m_CurlTex;
    const bool preparingDensityFields =
        bakeShapeNoiseThisFrame || bakeWeatherThisFrame ||
        bakeDetailNoiseThisFrame || bakeCurlNoiseThisFrame;
    const bool densityFieldsReady =
        recorded.noise_baked && recorded.weather_baked &&
        recorded.detail_baked && recorded.curl_baked;
    // 雲は風移流とは別に対流変形するため、自己影は毎フレーム更新する。
    // 生成したばかりの密度場と同じフレームへ影積分を集中させず、次のフレームから
    // 四つの偶奇位置へ分ける。これにより初回だけGPUを長時間占有しない。
    const bool rebuildShadowCacheThisFrame =
        shadowResourcesReady && densityFieldsReady;
    const bool rebuildWorldShadowThisFrame =
        worldShadowResourcesReady &&
        safeSun.y > kVolumetricCloudWorldShadowMinimumSunY &&
        safeCoverage > 0.001f && densityFieldsReady;
    if (!rebuildWorldShadowThisFrame) {
        recorded.world_shadow_valid = false;
        recorded.world_shadow_warmup_mask = 0u;
    }
    const bool cloudMediumChanged =
        previousShadowStateAvailable &&
        (coverageDelta != 0.0f || densityDelta != 0.0f);
    f32 selfSunDirectionStepDistance = 0.0f;
    f32 worldSunDirectionStepDistance = 0.0f;
    bool selfSunProjectionStepResolved = true;
    bool worldSunProjectionStepResolved = true;
    if (previousShadowStateAvailable) {
        f32 highestCloudAltitude = m_Layer.top_height;
        if (m_UpperLayer.top_height > m_UpperLayer.base_height &&
            m_UpperLayer.top_height > highestCloudAltitude) {
            highestCloudAltitude = m_UpperLayer.top_height;
        }
        const f32 selfShadowVerticalSpan =
            highestCloudAltitude - m_Layer.base_height;
        const f32 worldShadowVerticalSpan =
            highestCloudAltitude > recorded.world_shadow_reference_height
                ? highestCloudAltitude -
                    recorded.world_shadow_reference_height
                : 0.0f;
        selfSunProjectionStepResolved =
            render_internal::ResolveVolumetricCloudSunProjectionDelta_Internal(
                safeSun, m_PrevSunDir, selfShadowVerticalSpan,
                selfSunDirectionStepDistance);
        worldSunProjectionStepResolved =
            render_internal::ResolveVolumetricCloudSunProjectionDelta_Internal(
                safeSun, m_PrevSunDir, worldShadowVerticalSpan,
                worldSunDirectionStepDistance);
    }
    const f32 maximumPartialShadowAge = static_cast<f32>(
        render_internal::kCloudShadowTemporalPhaseCount - 1u);
    const bool selfSunDirectionDiscontinuity =
        previousShadowStateAvailable &&
        (!selfSunProjectionStepResolved ||
         selfSunDirectionStepDistance * maximumPartialShadowAge >=
             kVolumetricCloudShadowCacheCellSize);
    const bool worldSunDirectionDiscontinuity =
        previousShadowStateAvailable &&
        (!worldSunProjectionStepResolved ||
         worldSunDirectionStepDistance * maximumPartialShadowAge >=
             kVolumetricCloudWorldShadowMapTexelSize);
    // 自己影は物質座標で風移流が相殺される。対流・媒質・太陽方向の一段差から、
    // 最大3世代古い値でも一セル未満に収まる場合だけ部分更新する。
    const bool selfShadowTemporalDiscontinuity =
        rebuildShadowCacheThisFrame &&
        (shadowTemporalDecision.self_shadow_requires_full_refresh ||
         cloudMediumChanged || selfSunDirectionDiscontinuity);
    // 立体物用雲影は固定ワールド地図なので、対流形状に加えて一段の
    // 移流距離も比較し、最大3世代で一画素以上の差を残さない。
    const bool worldShadowTemporalDiscontinuity =
        rebuildWorldShadowThisFrame &&
        (shadowTemporalDecision.world_shadow_requires_full_refresh ||
         cloudMediumChanged || worldSunDirectionDiscontinuity ||
         worldShadowMappingChanged);
    // 画面履歴の破棄は影の物質座標や密度場を変えない。視点移動だけを理由に
    // 96x96列の全積分へ戻すと、一瞬粗くなる周期的なGPU停止を作る。
    const bool refreshAllSelfShadows =
        m_ReferenceMode || selfShadowTemporalDiscontinuity;
    const bool refreshAllWorldShadows =
        m_ReferenceMode || worldShadowTemporalDiscontinuity;
    const u32 shadowUpdateDivisor = refreshAllSelfShadows
        ? 1u : kVolumetricCloudShadowTemporalDivisor;
    const u32 worldShadowUpdateDivisor = refreshAllWorldShadows
        ? 1u : kVolumetricCloudShadowTemporalDivisor;
    const u32 shadowUpdateOffsetX = shadowUpdateDivisor == 1u
        ? 0u : shadowTemporalDecision.partial_update_offset_x;
    const u32 shadowUpdateOffsetY = shadowUpdateDivisor == 1u
        ? 0u : shadowTemporalDecision.partial_update_offset_y;
    const u32 worldShadowUpdateOffsetX =
        worldShadowUpdateDivisor == 1u
            ? 0u : shadowTemporalDecision.partial_update_offset_x;
    const u32 worldShadowUpdateOffsetY =
        worldShadowUpdateDivisor == 1u
            ? 0u : shadowTemporalDecision.partial_update_offset_y;
    const u8 shadowWarmupMaskAfterUpdate = rebuildShadowCacheThisFrame
        ? render_internal::ResolveVolumetricCloudShadowWarmupMask_Internal(
            recorded.shadow_cache_warmup_mask,
            shadowTemporalDecision.phase,
            refreshAllSelfShadows)
        : recorded.shadow_cache_warmup_mask;
    const u8 worldShadowWarmupMaskAfterUpdate = rebuildWorldShadowThisFrame
        ? render_internal::ResolveVolumetricCloudShadowWarmupMask_Internal(
            recorded.world_shadow_warmup_mask,
            shadowTemporalDecision.phase,
            refreshAllWorldShadows)
        : recorded.world_shadow_warmup_mask;
    const bool shadowCacheReadyAfterUpdate =
        recorded.shadow_cache_valid ||
        shadowWarmupMaskAfterUpdate ==
            render_internal::kCloudShadowTemporalCompleteMask;
    const bool worldShadowReadyAfterUpdate =
        recorded.world_shadow_valid ||
        worldShadowWarmupMaskAfterUpdate ==
            render_internal::kCloudShadowTemporalCompleteMask;
    FVolumetricCloudFrameWorkloadPlan workloadPlan{};
    workloadPlan.trace_width = m_W;
    workloadPlan.trace_height = m_H;
    workloadPlan.output_width = m_FullW;
    workloadPlan.output_height = m_FullH;
    const u32 viewSteps = m_ReferenceMode ? kVolumetricCloudReferenceViewSteps : (m_Range.ViewSteps > 0u ? m_Range.ViewSteps : kVolumetricCloudViewSteps);
    workloadPlan.maximum_view_steps = viewSteps;
    workloadPlan.shadow_update_divisor = shadowUpdateDivisor;
    workloadPlan.bake_shape_noise = bakeShapeNoiseThisFrame;
    workloadPlan.bake_weather = bakeWeatherThisFrame;
    workloadPlan.bake_detail_noise = bakeDetailNoiseThisFrame;
    workloadPlan.bake_curl_noise = bakeCurlNoiseThisFrame;
    workloadPlan.rebuild_shadow_cache = rebuildShadowCacheThisFrame;
    workloadPlan.rebuild_world_shadow = rebuildWorldShadowThisFrame;
    FVolumetricCloudFrameWorkloadInternalOptions workloadOptions{};
    workloadOptions.render_cloud = !preparingDensityFields;
    workloadOptions.shape_bake_dispatches =
        bakeShapeNoiseThisFrame ? 1u : 0u;
    workloadOptions.world_shadow_update_divisor =
        worldShadowUpdateDivisor;
    m_LastFrameWorkload = PlanVolumetricCloudFrameWorkload_Internal(
        workloadPlan, workloadOptions);
    m_LastFrameWorkload.attempted = true;
    m_LastFrameWorkload.history_was_available =
        historyWasAvailable;
    m_LastFrameWorkload.history_reused =
        !preparingDensityFields && historyValid;
    m_LastFrameWorkload.history_invalidated =
        historyWasAvailable && !historyValid;

    FCloudCb cb{};
    // 視線復元には、カメラ位置を含めずに反転した高精度な行列を使う。
    cb.invViewProj = camera_relative_inv_view_proj;
    cb.prevCameraRelativeViewProj = historyValid
        ? m_PrevCameraRelativeViewProj : cameraRelativeViewProj;
    cb.camPos = FVec4{ cam_pos.x, cam_pos.y, cam_pos.z, 0.0f };
    cb.prevCamPos = historyValid
                  ? FVec4{m_PrevCamPos.x, m_PrevCamPos.y, m_PrevCamPos.z, 0.0f}
                  : cb.camPos;
    cb.sunDir = FVec4{ safeSun.x, safeSun.y, safeSun.z, 0.0f };
    cb.sunCol = FVec4{ safeSunColor.x, safeSunColor.y, safeSunColor.z, 0.0f };
    cb.skyCol = FVec4{ safeSkyColor.x, safeSkyColor.y, safeSkyColor.z, 0.0f };
    cb.params = FVec4{ safeCoverage, safeDensity, windOffset, safeTime };
    cb.dims   = FVec4{ static_cast<f32>(m_W), static_cast<f32>(m_H),
                       static_cast<f32>(m_FullW), static_cast<f32>(m_FullH) };
    const bool temporalSuperResolution =
        m_LastFrameWorkload.temporal_super_resolution;
    // Ultra writes every quarter-dimension texel on every frame, independent
    // of camera motion/history validity. temporal.w selects exact 4x4 subpixel
    // rays and the full-resolution sixteen-phase resolve; it never changes the
    // ray-march dispatch dimensions.
    const u32 temporalFrame =
        (recorded.frame_index & 4080u) |
        (recorded.temporal_phase & 15u);
    // 参照描画では履歴も時間方向の再構成も使わない。そのフレームだけで完結させる
    // (再構成の影響を混ぜたままだと、ライティングの良し悪しを判断できない)。
    cb.temporal = FVec4{
        (historyValid && !m_ReferenceMode) ? 1.0f : 0.0f,
        m_PrevWindOffset,
        static_cast<f32>(temporalFrame),
        (temporalSuperResolution && !m_ReferenceMode)
            ? static_cast<f32>(kVolumetricCloudUltraTraceDivisor)
            : 0.0f };
    cb.layer = FVec4{m_Layer.base_height, m_Layer.top_height,
                     m_Layer.horizontal_noise_scale,
                     kVolumetricCloudReferenceExtinctionPerMeter};
    // 下層と重ならず正の厚さを持つ上層だけを採取対象にする。
    const bool hasUpperLayer =
        m_UpperLayer.top_height > m_UpperLayer.base_height &&
        m_UpperLayer.base_height >= m_Layer.top_height;
    // 画面と環境光で同じ密度形状と物理消散を使う。
    const FCloudSamplingTerms samplingTerms =
        ResolveVolumetricCloudSamplingTerms_Internal(
            safeCoverage,m_Layer.horizontal_noise_scale,m_UpperLayer);
    const bool temporalHistoryStationary = historyValid &&
        cameraDeltaSquared <= 0.0025f && matrixDelta <= 0.002f;
    cb.worldOrigin = FVec4{
        worldOrigin.x, worldOrigin.y, worldOrigin.z,
        temporalHistoryStationary ? 1.0f : 0.0f};
    const f32 invShadowExtent =
        1.0f / kVolumetricCloudShadowCacheExtent;
    // 最大描画距離の変更時だけ変わる外周写像をCPUで一度求め、視線標本ごとの導出を避ける。
    const render_internal::FVolumetricCloudAmbientCacheMapTerms ambientCacheMapTerms = render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(m_Range.MaxDistance);
    cb.shadowGrid = FVec4{
        recorded.shadow_grid_minimum_material_xz.x,
        recorded.shadow_grid_minimum_material_xz.y,
        invShadowExtent, invShadowExtent};
    cb.shadowState = FVec4{
        shadowCacheReadyAfterUpdate ? 1.0f : 0.0f,
        kSkyPhysicalSunAngularRadiusRadians,
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheWidth),
        ambientCacheMapTerms.guard_coefficient};
    const FVolumetricCloudGroundHorizon groundHorizon =
        ResolveVolumetricCloudGroundHorizon(
            cam_pos, m_Layer, worldOrigin);
    cb.groundHorizon = FVec4{
        groundHorizon.local_up.x,
        groundHorizon.local_up.y,
        groundHorizon.local_up.z,
        groundHorizon.ground_cutoff};
    cb.cloudFrameTerms = FVec4{
        densityFrameTerms.wind_world.x,
        densityFrameTerms.wind_world.y,
        densityFrameTerms.shape_scale,
        densityFrameTerms.inverse_layer_height};
    cb.cloudEvolution = FVec4{
        evolutionFrameTerms.shape_phase.x,
        evolutionFrameTerms.shape_phase.y,
        evolutionFrameTerms.fine_phase.x,
        evolutionFrameTerms.fine_phase.y};
    cb.cloudPreviousEvolution = FVec4{
        previousEvolutionFrameTerms.shape_phase.x,
        previousEvolutionFrameTerms.shape_phase.y,
        previousEvolutionFrameTerms.fine_phase.x,
        previousEvolutionFrameTerms.fine_phase.y};
    cb.cloudWeatherControl = FVec4{
        m_Weather.CloudType,
        m_Weather.CloudTypeInfluence,
        m_Weather.Precipitation,
        m_Weather.PrecipitationInfluence};
    cb.cloudShadowUpdate = FVec4{
        static_cast<f32>(shadowUpdateOffsetX),
        static_cast<f32>(shadowUpdateOffsetY),
        static_cast<f32>(shadowUpdateDivisor),
        refreshAllSelfShadows ? 1.0f : 0.0f};
    cb.cloudWorldShadowMap = FVec4{
        recorded.world_shadow_map_minimum_reference_xz.x,
        recorded.world_shadow_map_minimum_reference_xz.y,
        1.0f / kVolumetricCloudWorldShadowMapExtent,
        recorded.world_shadow_reference_height};
    cb.cloudWorldShadowUpdate = FVec4{
        static_cast<f32>(worldShadowUpdateOffsetX),
        static_cast<f32>(worldShadowUpdateOffsetY),
        static_cast<f32>(worldShadowUpdateDivisor),
        refreshAllWorldShadows ? 1.0f : 0.0f};
    cb.cloudLightingHistory = FVec4{lightingMismatch, 0.0f, 0.0f, 0.0f};
    cb.cloudLightTangent = FVec4{
        lightBasis.tangent.x,
        lightBasis.tangent.y,
        lightBasis.tangent.z,
        0.0f};
    cb.cloudLightBitangent = FVec4{
        lightBasis.bitangent.x,
        lightBasis.bitangent.y,
        lightBasis.bitangent.z,
        0.0f};
    cb.cloudCoverage = samplingTerms.coverage;
    cb.cloudCoverageReciprocals = samplingTerms.coverageReciprocals;
    // カメラ、再基準化原点、上下層は一回の視線計算中に不変である。各縮小画素で
    // 同じ二次方程式項を組み直さず、曲面区間のc項をCPUで一度だけ求める。
    const FVec3 shellLocalOrigin{
        cam_pos.x - worldOrigin.x,
        cam_pos.y - worldOrigin.y,
        cam_pos.z - worldOrigin.z};
    const f32 shellRadialXzSquared =
        shellLocalOrigin.x * shellLocalOrigin.x +
        shellLocalOrigin.z * shellLocalOrigin.z;
    const auto shellC =
        [shellLocalOrigin, shellRadialXzSquared](f32 altitude) noexcept {
            return shellRadialXzSquared +
                (shellLocalOrigin.y - altitude) *
                (2.0f * kVolumetricCloudPlanetRadius +
                 shellLocalOrigin.y + altitude);
        };
    cb.cloudShellRayOrigin = FVec4{
        shellLocalOrigin.x,
        shellLocalOrigin.y + kVolumetricCloudPlanetRadius,
        shellLocalOrigin.z,
        shellC(m_Layer.base_height)};
    // 下層と上層を独立した区間にし、層間の晴天域を視線と雲影の標本予算へ含めない。
    const f32 upperBaseShellC = hasUpperLayer ? shellC(m_UpperLayer.base_height) : 0.0f;
    const f32 upperTopShellC = hasUpperLayer ? shellC(m_UpperLayer.top_height) : 0.0f;
    cb.cloudShellTerms = FVec4{shellC(m_Layer.top_height), upperBaseShellC, upperTopShellC, 0.0f};
    cb.cloudUpperLayer = hasUpperLayer
        ? FVec4{m_UpperLayer.base_height, m_UpperLayer.top_height,
                1.0f / (m_UpperLayer.top_height - m_UpperLayer.base_height),
                1.0f}
        : FVec4{0.0f, 0.0f, 0.0f, 0.0f};
    cb.cloudUpperTerms = samplingTerms.upperTerms;
    cb.cloudLightingExtinction = FVec4{
        m_Lighting.ViewExtinction, m_Lighting.LightExtinction,
        m_Lighting.SunScatter, m_Lighting.PowderStrength};
    cb.cloudLightingPhase = FVec4{
        m_Lighting.PhaseForward, m_Lighting.PhaseBackward,
        m_Lighting.PhaseBlend, m_Lighting.MultiScatterContribution};
    cb.cloudLightingMulti = FVec4{
        m_Lighting.MultiScatterOcclusion, m_Lighting.PhaseMin,
        m_Lighting.PhaseMax, m_Lighting.GroundContribution};
    // 刻み数は負荷計測とシェーダーで同じ値を使う。参照描画は利用側の上限指定を無視する。
    cb.cloudLightingAmbient = FVec4{
        m_Lighting.AmbientAtBase, m_Lighting.AmbientAtTop,
        static_cast<f32>(viewSteps),
        m_ReferenceMode ? 1.0f : 0.0f};
    cb.cloudLightingGround = FVec4{
        m_Lighting.GroundColor.x, m_Lighting.GroundColor.y,
        m_Lighting.GroundColor.z,
        m_Lighting.SunScatteringLuminanceScale};
    cb.cloudSunTransmittance = FVec4{
        m_Lighting.SunTransmittance.x, m_Lighting.SunTransmittance.y,
        m_Lighting.SunTransmittance.z, 0.0f};
    const bool splitSkyAmbient =
        m_Lighting.SkyZenithColor.x > 0.0f || m_Lighting.SkyZenithColor.y > 0.0f
        || m_Lighting.SkyZenithColor.z > 0.0f;
    cb.cloudSkyZenith = FVec4{
        m_Lighting.SkyZenithColor.x, m_Lighting.SkyZenithColor.y,
        m_Lighting.SkyZenithColor.z, splitSkyAmbient ? 1.0f : 0.0f};
    cb.cloudMultiPhase = FVec4{
        m_Lighting.MultiScatterEccentricity, 0.0f, 0.0f, 0.0f};
    // setter で正規化済みの距離を CPU 側の保持値と同じまま GPU へ渡す。
    const f32 maxDistance = m_Range.MaxDistance;
    const f32 fadeFraction = m_Range.FadeFraction;
    const f32 currentViewDistance = ResolveVolumetricCloudViewDistance_Internal(shellLocalOrigin, m_Layer, m_UpperLayer, hasUpperLayer, maxDistance);
    cb.cloudRange = FVec4{
        maxDistance,
        maxDistance * (1.0f - fadeFraction),
        m_Range.StepGrowth,
        currentViewDistance};
    m_Cb->Update(&cb, sizeof(cb));
    // 初回だけ三帯域の密度形状と、その和集合である二値支持域を生成する。
    // 支持域用の二つの8bit体積だけを交互に読み書きし、密度形状を最大値で壊さない。
    if (bakeShapeNoiseThisFrame) {
        const u8 bakeStage = recorded.density_bake_stage;
        if (bakeStage == 0u) {
            cl.SetComputePipeline(*m_NoisePipe);
            cl.BindUav(0, *m_ShapeTex);
            cl.BindUav(1, *m_NoiseFilterResources->source_texture);
            cl.Dispatch(32, 32, 32);   // 128/4
            recorded.density_bake_stage = 1u;
        } else {
            cl.SetComputePipeline(*m_NoiseFilterResources->pipeline);
            if (bakeStage == 1u) {
                cl.SetTexture(0, *m_NoiseFilterResources->source_texture);
                cl.BindUav(0, *m_NoiseFilterResources->filtered_texture);
            } else if (bakeStage == 2u) {
                cl.SetTexture(0, *m_NoiseFilterResources->filtered_texture);
                cl.BindUav(0, *m_NoiseFilterResources->source_texture);
            } else {
                cl.SetTexture(0, *m_NoiseFilterResources->source_texture);
                cl.BindUav(0, *m_NoiseFilterResources->filtered_texture);
            }
            // 各軸の周期最大値を別々のGPU提出へ分け、入力と出力を交互にする。
            cl.Dispatch(128, 128, 1);
            if (bakeStage >= 3u) {
                recorded.density_bake_stage = 4u;
                recorded.noise_baked = true;
            } else {
                recorded.density_bake_stage =
                    static_cast<u8>(bakeStage + 1u);
            }
        }
    }
    if (bakeWeatherThisFrame) {
        cl.SetComputePipeline(*m_WeatherPipe);
        cl.BindUav(0, *m_WeatherTex);
        cl.Dispatch(64, 64, 1);    // 512/8
        recorded.weather_baked = true;
    }
    if (bakeDetailNoiseThisFrame) {
        cl.SetComputePipeline(*m_DetailPipe);
        cl.BindUav(0, *m_DetailTex);
        cl.Dispatch(16, 16, 16);   // 64/4
        recorded.detail_baked = true;
    }
    if (bakeCurlNoiseThisFrame) {
        cl.SetComputePipeline(*m_CurlPipe);
        cl.BindUav(0, *m_CurlTex);
        cl.Dispatch(16, 16, 1);    // 128/8
        recorded.curl_baked = true;
    }
    if (preparingDensityFields) {
        // 一段ずつ生成した3D密度場を同じGPU提出内で後続処理へ渡さない。
        // 次のフレームで依存する段階だけを進め、通常の空は応答を保ったまま残す。
        m_LastFrameWorkload.skip_reason =
            EVolumetricCloudFrameSkipReason::PreparingDensityFields;
        return;
    }
    if (rebuildShadowCacheThisFrame) {
        const u32 updateWidth = CloudCeilDivisor(
            kVolumetricCloudShadowCacheWidth - shadowUpdateOffsetX,
            shadowUpdateDivisor);
        const u32 updateDepth = CloudCeilDivisor(
            kVolumetricCloudShadowCacheDepth - shadowUpdateOffsetY,
            shadowUpdateDivisor);
        // 一つのGPUグループが一つの水平セルを担当する。16スレッドで周囲光を
        // 面積積分し、その先頭4スレッドが太陽円盤の各光路も生成する。
        cl.SetComputePipeline(*m_ShadowPipe);
        cl.SetConstantBuffer(0, *m_Cb);
        cl.SetTexture(0, *m_ShapeTex);
        cl.SetTexture(1, *m_NoiseFilterResources->filtered_texture);
        cl.SetTexture(2, *m_WeatherTex);
        cl.SetTexture(3, *m_DetailTex);
        cl.SetTexture(4, *m_CurlTex);
        cl.BindUav(0, *m_CloudTex);
        cl.BindUav(1, *m_CloudDepth);
        // 現在の計算RHIはUAV登録番号を連続させるため、u0/u1へ有効な
        // 代替テクスチャを割り当て、周囲光と太陽光路をu2へ書く。
        cl.BindUav(2, *m_ShadowTex);
        cl.Dispatch(updateWidth, 1u, updateDepth);
        recorded.shadow_cache_warmup_mask =
            shadowWarmupMaskAfterUpdate;
        recorded.shadow_cache_valid = shadowCacheReadyAfterUpdate;
        ++recorded.shadow_cache_dispatch_count;
    }
    if (rebuildWorldShadowThisFrame) {
        cl.SetComputePipeline(*m_WorldShadowPipe);
        cl.SetConstantBuffer(0, *m_Cb);
        cl.SetTexture(0, *m_ShapeTex);
        cl.SetTexture(1, *m_NoiseFilterResources->filtered_texture);
        cl.SetTexture(2, *m_WeatherTex);
        cl.SetTexture(3, *m_DetailTex);
        cl.SetTexture(4, *m_CurlTex);
        cl.BindUav(0, *m_WorldShadowTex);
        const u32 updateWidth = CloudCeilDivisor(
            kVolumetricCloudWorldShadowMapResolution -
                worldShadowUpdateOffsetX,
            worldShadowUpdateDivisor);
        const u32 updateHeight = CloudCeilDivisor(
            kVolumetricCloudWorldShadowMapResolution -
                worldShadowUpdateOffsetY,
            worldShadowUpdateDivisor);
        cl.Dispatch((updateWidth + 7u) / 8u, (updateHeight + 7u) / 8u, 1u);
        recorded.world_shadow_warmup_mask =
            worldShadowWarmupMaskAfterUpdate;
        recorded.world_shadow_valid = worldShadowReadyAfterUpdate;
        ++recorded.world_shadow_dispatch_count;
    }
    // 影キャッシュが未完成でも雲本体は止めない。shadowState.x が正確な
    // 直接積分へ戻し、完成した四位相だけを次のdispatchから採取する。
    cl.SetComputePipeline(*m_CloudPipe);
    cl.SetConstantBuffer(0, *m_Cb);
    if (m_ShapeTex) cl.SetTexture(0, *m_ShapeTex);   // shape noise SRV (UAV→SRV は Dispatch の TRANSITION commit)
    if (m_NoiseFilterResources && m_NoiseFilterResources->filtered_texture)
        cl.SetTexture(1, *m_NoiseFilterResources->filtered_texture);
    if (m_WeatherTex) cl.SetTexture(2, *m_WeatherTex);
    if (m_DetailTex) cl.SetTexture(3, *m_DetailTex);
    if (m_CurlTex) cl.SetTexture(4, *m_CurlTex);
    if (m_ShadowTex && shadowCacheReadyAfterUpdate) {
        cl.SetTexture(5, *m_ShadowTex);
    } else if (m_ShapeTex) {
        // 同じ3次元資源次元を持つ代替テクスチャ。shadowState.xが採取を止めるため、
        // RG形式とRGBA形式の成分差は読み取り経路へ到達しない。
        cl.SetTexture(5, *m_ShapeTex);
    }
    cl.BindUav(0, *m_CloudTex);
    cl.BindUav(1, *m_CloudDepth);
    cl.Dispatch((m_W + 7u) / 8u, (m_H + 7u) / 8u, 1);

    // Scaled UAV → full-resolution temporal color/depth.  One compute pass
    // shares reconstruction/history reads and writes both formats as UAVs,
    // avoiding the mixed-MRT fullscreen draw overhead. Ping-pong keeps input
    // SRVs and output UAVs disjoint.
    const u32 cur = recorded.frame_index & 1u;
    const u32 prev = cur ^ 1u;

    cl.SetComputePipeline(*m_ResolvePipe);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_CloudTex);
    cl.SetTexture(1, *m_CloudDepth);
    cl.SetTexture(2, *m_HistoryColor[prev]);
    cl.SetTexture(3, *m_HistoryDepth[prev]);
    cl.BindUav(0, *m_HistoryColor[cur]);
    cl.BindUav(1, *m_HistoryDepth[cur]);
    cl.Dispatch((m_FullW + 7u) / 8u, (m_FullH + 7u) / 8u, 1);

    if (recorded.workload_submission_index != kCloudWorkloadMaximum) {
        ++recorded.workload_submission_index;
    }
    recorded.cloud_frame_recorded = true;
    recorded.resolved_index = cur;
    ++recorded.frame_index;
    recorded.temporal_phase =
        (recorded.temporal_phase + 1u) & 15u;
    recorded.history_valid = true;
    recorded.previous_camera_relative_view_projection =
        cameraRelativeViewProj;
    recorded.previous_camera_relative_inverse_view_projection =
        camera_relative_inv_view_proj;
    recorded.previous_camera_position = cam_pos;
    recorded.world_origin = worldOrigin;
    recorded.previous_sun_direction = safeSun;
    recorded.previous_sun_color = safeSunColor;
    recorded.previous_sky_color = safeSkyColor;
    recorded.previous_wind_offset = windOffset;
    recorded.previous_wind_speed = safeWind;
    recorded.previous_coverage = safeCoverage;
    recorded.previous_density = safeDensity;
    recorded.previous_time = safeTime;
}

TResult<TUniquePtr<IRhiTexture>>
CVolumetricClouds::BuildEnvironmentCubemap(
        IRhiDevice& device, IRhiCommandList& cl,
        IRhiTexture& base_environment) noexcept {
    if (!IsInitializedForDevice(device) || RecordedFramePending() ||
        !m_HistoryValid || !m_CloudPipe || !m_Cb
        || !m_ShapeTex || !m_NoiseFilterResources
        || !m_NoiseFilterResources->filtered_texture
        || !m_WeatherTex || !m_DetailTex || !m_CurlTex
        || !m_NoiseBaked || !m_WeatherBaked || !m_DetailBaked
        || !m_CurlBaked) {
        return Err<TUniquePtr<IRhiTexture>>(
            ACS_ERR(Render, 730,
                    "cloud environment source is not ready"));
    }
    if (!base_environment.IsCubemap()
        || base_environment.ArraySize() < 6u) {
        return Err<TUniquePtr<IRhiTexture>>(
            ACS_ERR(Render, 731,
                    "cloud environment requires a base cubemap"));
    }

    FTextureDesc cloud_color_desc{};
    cloud_color_desc.width = kVolumetricCloudEnvironmentCubeSize;
    cloud_color_desc.height = kVolumetricCloudEnvironmentCubeSize;
    cloud_color_desc.format = EFormat::R16G16B16A16_Float;
    cloud_color_desc.is_uav = true;
    auto cloud_color_result = CreateRhiTexture(device, cloud_color_desc);
    if (cloud_color_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(cloud_color_result.Error());
    }
    TUniquePtr<IRhiTexture> cloud_color =
        Move(cloud_color_result.Value());

    FTextureDesc cloud_depth_desc{};
    cloud_depth_desc.width = kVolumetricCloudEnvironmentCubeSize;
    cloud_depth_desc.height = kVolumetricCloudEnvironmentCubeSize;
    cloud_depth_desc.format = EFormat::R32G32_Float;
    cloud_depth_desc.is_uav = true;
    auto cloud_depth_result = CreateRhiTexture(device, cloud_depth_desc);
    if (cloud_depth_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(cloud_depth_result.Error());
    }
    TUniquePtr<IRhiTexture> cloud_depth =
        Move(cloud_depth_result.Value());

    FTextureDesc environment_desc{};
    environment_desc.width = kVolumetricCloudEnvironmentCubeSize;
    environment_desc.height = kVolumetricCloudEnvironmentCubeSize;
    environment_desc.format = EFormat::R16G16B16A16_Float;
    environment_desc.array_size = 6u;
    environment_desc.is_cubemap = true;
    environment_desc.is_render_target = true;
    environment_desc.per_slice_rtv = true;
    auto environment_result = CreateRhiTexture(device, environment_desc);
    if (environment_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(environment_result.Error());
    }
    TUniquePtr<IRhiTexture> environment =
        Move(environment_result.Value());

    FBufferDesc cloud_cb_desc{};
    cloud_cb_desc.size = CBSize<FCloudCb>();
    cloud_cb_desc.usage = EBufferUsage::Uniform;
    cloud_cb_desc.cpu_writable = true;
    // raw DX12のupload bufferは同一frame slot内の再更新をGPU実行まで保持しない。
    // 6面を別bufferにして、各dispatchが対応する逆view-projectionを確実に読む。
    TUniquePtr<IRhiBuffer> cloud_cb[6];
    for (u32 face = 0u; face < 6u; ++face) {
        auto cloud_cb_result = CreateRhiBuffer(device, cloud_cb_desc);
        if (cloud_cb_result.IsErr()) {
            return Err<TUniquePtr<IRhiTexture>>(cloud_cb_result.Error());
        }
        cloud_cb[face] = Move(cloud_cb_result.Value());
    }

    FBufferDesc composite_cb_desc{};
    composite_cb_desc.size = CBSize<FCloudEnvironmentCompositeCb>();
    composite_cb_desc.usage = EBufferUsage::Uniform;
    composite_cb_desc.cpu_writable = true;
    TUniquePtr<IRhiBuffer> composite_cb[6];
    for (u32 face = 0u; face < 6u; ++face) {
        auto composite_cb_result = CreateRhiBuffer(
            device, composite_cb_desc);
        if (composite_cb_result.IsErr()) {
            return Err<TUniquePtr<IRhiTexture>>(
                composite_cb_result.Error());
        }
        composite_cb[face] = Move(composite_cb_result.Value());
    }

    FShaderDesc vertex_desc{};
    vertex_desc.stage = EShaderStage::Vertex;
    vertex_desc.hlsl_source = kCloudEnvironmentCompositeHLSL;
    vertex_desc.entry_point = "VSMain";
    vertex_desc.debug_name = "CloudEnvironmentComposite.VS";
    auto vertex_result = CreateRhiShader(device, vertex_desc);
    if (vertex_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(vertex_result.Error());
    }
    TUniquePtr<IRhiShader> vertex = Move(vertex_result.Value());

    FShaderDesc pixel_desc{};
    pixel_desc.stage = EShaderStage::Pixel;
    pixel_desc.hlsl_source = kCloudEnvironmentCompositeHLSL;
    pixel_desc.entry_point = "PSMain";
    pixel_desc.debug_name = "CloudEnvironmentComposite.PS";
    auto pixel_result = CreateRhiShader(device, pixel_desc);
    if (pixel_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(pixel_result.Error());
    }
    TUniquePtr<IRhiShader> pixel = Move(pixel_result.Value());

    FPipelineDesc pipeline_desc{};
    pipeline_desc.vs = vertex.Get();
    pipeline_desc.ps = pixel.Get();
    pipeline_desc.topology = EPrimitiveTopology::TriangleList;
    pipeline_desc.rt_format = EFormat::R16G16B16A16_Float;
    pipeline_desc.depth_format = EFormat::Unknown;
    pipeline_desc.depth_test = false;
    pipeline_desc.depth_write = false;
    pipeline_desc.cull_mode = ECullMode::None;
    pipeline_desc.blend_mode = EBlendMode::Opaque;
    pipeline_desc.cbuffer_slots = 1u;
    pipeline_desc.cbuffer_names[0] = "CloudEnvironmentCompositeCB";
    pipeline_desc.texture_slots = 2u;
    pipeline_desc.texture_names[0] = "baseEnvironment";
    pipeline_desc.texture_names[1] = "cloudEnvironment";
    pipeline_desc.static_sampler_count = 2u;
    pipeline_desc.static_samplers[0].filter = ESamplerFilter::Linear;
    pipeline_desc.static_samplers[0].address_u = ESamplerAddress::Clamp;
    pipeline_desc.static_samplers[0].address_v = ESamplerAddress::Clamp;
    pipeline_desc.static_samplers[0].address_w = ESamplerAddress::Clamp;
    pipeline_desc.static_samplers[1].filter = ESamplerFilter::Linear;
    pipeline_desc.static_samplers[1].address_u = ESamplerAddress::Clamp;
    pipeline_desc.static_samplers[1].address_v = ESamplerAddress::Clamp;
    pipeline_desc.layout_count = 0u;
    pipeline_desc.vertex_stride = 0u;
    auto pipeline_result = CreateRhiPipeline(device, pipeline_desc);
    if (pipeline_result.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(pipeline_result.Error());
    }
    TUniquePtr<IRhiPipeline> pipeline = Move(pipeline_result.Value());

    const FVolumetricCloudLightBasis light_basis =
        ResolveVolumetricCloudLightBasis(m_PrevSunDir);
    const FVec3 safe_sun = light_basis.direction;
    const FVec3 safe_sun_color = SanitizeCloudRadiance(
        m_PrevSunColor, FVec3{1.0f, 1.0f, 1.0f});
    const FVec3 safe_sky_color = SanitizeCloudRadiance(
        m_PrevSkyColor, FVec3{0.2f, 0.25f, 0.3f});
    const f32 safe_coverage = SanitizeCloudScalar(
        m_PrevCoverage, 0.0f, 0.0f, 1.0f);
    const f32 safe_density = SanitizeCloudScalar(
        m_PrevDensity, 1.0f, 0.05f, 8.0f);
    const f32 safe_time = SanitizeCloudScalar(
        m_PrevTime, 0.0f, -10000000.0f, 10000000.0f);
    const f32 safe_wind = SanitizeCloudScalar(
        m_PrevWindSpeed, 0.0f, -20.0f, 20.0f);
    const f32 wind_offset = ResolveVolumetricCloudAdvectionDistance(
        safe_time, safe_wind);
    const FVec3 world_origin = RebaseVolumetricCloudWorldOrigin(m_PrevCamPos);
    const FVolumetricCloudDensityFrameTerms density_terms =
        ResolveVolumetricCloudDensityFrameTerms(m_Layer, wind_offset);
    const FVolumetricCloudEvolutionFrameTerms evolution_terms =
        ResolveVolumetricCloudEvolutionFrameTerms(safe_time, safe_wind);

    FCloudCb cb{};
    cb.camPos = FVec4{
        m_PrevCamPos.x, m_PrevCamPos.y, m_PrevCamPos.z, 0.0f};
    cb.prevCamPos = cb.camPos;
    cb.sunDir = FVec4{safe_sun.x, safe_sun.y, safe_sun.z, 0.0f};
    cb.sunCol = FVec4{
        safe_sun_color.x, safe_sun_color.y, safe_sun_color.z, 0.0f};
    cb.skyCol = FVec4{
        safe_sky_color.x, safe_sky_color.y, safe_sky_color.z, 0.0f};
    cb.params = FVec4{
        safe_coverage, safe_density, wind_offset, safe_time};
    cb.dims = FVec4{
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize),
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize),
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize),
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize)};
    // 時間履歴を持たない一回完結の積分なので、固定した区間中央を採取する。
    cb.temporal = FVec4{0.0f, wind_offset, 0.0f, 0.0f};
    cb.layer = FVec4{
        m_Layer.base_height, m_Layer.top_height,
        m_Layer.horizontal_noise_scale,
        kVolumetricCloudReferenceExtinctionPerMeter};
    // 下層と重ならず正の厚さを持つ上層だけを採取対象にする。
    const bool has_upper_layer =
        m_UpperLayer.top_height > m_UpperLayer.base_height
        && m_UpperLayer.base_height >= m_Layer.top_height;
    // 画面と環境光で同じ密度形状と物理消散を使う。
    const FCloudSamplingTerms samplingTerms =
        ResolveVolumetricCloudSamplingTerms_Internal(
            safe_coverage,m_Layer.horizontal_noise_scale,m_UpperLayer);
    cb.worldOrigin = FVec4{
        world_origin.x, world_origin.y, world_origin.z, 0.0f};
    const f32 inverse_shadow_extent =
        1.0f / kVolumetricCloudShadowCacheExtent;
    // 通常描画と環境cubemapで同じ外周写像を使い、照明更新だけ別の格子へ変えない。
    const render_internal::FVolumetricCloudAmbientCacheMapTerms ambient_cache_map_terms = render_internal::ResolveVolumetricCloudAmbientCacheMapTerms_Internal(m_Range.MaxDistance);
    cb.shadowGrid = FVec4{
        m_ShadowGridMinQ.x, m_ShadowGridMinQ.y,
        inverse_shadow_extent, inverse_shadow_extent};
    cb.shadowState = FVec4{
        m_ShadowCacheValid ? 1.0f : 0.0f,
        kSkyPhysicalSunAngularRadiusRadians,
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheWidth),
        ambient_cache_map_terms.guard_coefficient};
    const FVolumetricCloudGroundHorizon ground_horizon =
        ResolveVolumetricCloudGroundHorizon(
            m_PrevCamPos, m_Layer, world_origin);
    cb.groundHorizon = FVec4{
        ground_horizon.local_up.x,
        ground_horizon.local_up.y,
        ground_horizon.local_up.z,
        ground_horizon.ground_cutoff};
    cb.cloudFrameTerms = FVec4{
        density_terms.wind_world.x,
        density_terms.wind_world.y,
        density_terms.shape_scale,
        density_terms.inverse_layer_height};
    cb.cloudEvolution = FVec4{
        evolution_terms.shape_phase.x,
        evolution_terms.shape_phase.y,
        evolution_terms.fine_phase.x,
        evolution_terms.fine_phase.y};
    cb.cloudPreviousEvolution = cb.cloudEvolution;
    // 画面に見える雲種と降水形状を、環境光の採取にもそのまま反映する。
    cb.cloudWeatherControl = FVec4{
        m_Weather.CloudType,
        m_Weather.CloudTypeInfluence,
        m_Weather.Precipitation,
        m_Weather.PrecipitationInfluence};
    cb.cloudShadowUpdate = FVec4{0.0f, 0.0f, 1.0f, 1.0f};
    cb.cloudWorldShadowMap = FVec4{
        m_WorldShadowMapMinReferenceXz.x,
        m_WorldShadowMapMinReferenceXz.y,
        1.0f / kVolumetricCloudWorldShadowMapExtent,
        m_WorldShadowReferenceHeight};
    cb.cloudWorldShadowUpdate = FVec4{0.0f, 0.0f, 1.0f, 1.0f};
    cb.cloudLightingHistory = FVec4{1.0f, 0.0f, 0.0f, 0.0f};
    cb.cloudLightTangent = FVec4{
        light_basis.tangent.x,
        light_basis.tangent.y,
        light_basis.tangent.z,
        0.0f};
    cb.cloudLightBitangent = FVec4{
        light_basis.bitangent.x,
        light_basis.bitangent.y,
        light_basis.bitangent.z,
        0.0f};
    cb.cloudCoverage = samplingTerms.coverage;
    cb.cloudCoverageReciprocals = samplingTerms.coverageReciprocals;
    const FVec3 shell_local_origin{
        m_PrevCamPos.x - world_origin.x,
        m_PrevCamPos.y - world_origin.y,
        m_PrevCamPos.z - world_origin.z};
    const f32 shell_radial_xz_squared =
        shell_local_origin.x * shell_local_origin.x
        + shell_local_origin.z * shell_local_origin.z;
    const auto shell_c =
        [shell_local_origin, shell_radial_xz_squared](f32 altitude) noexcept {
            return shell_radial_xz_squared
                + (shell_local_origin.y - altitude)
                * (2.0f * kVolumetricCloudPlanetRadius
                   + shell_local_origin.y + altitude);
        };
    cb.cloudShellRayOrigin = FVec4{
        shell_local_origin.x,
        shell_local_origin.y + kVolumetricCloudPlanetRadius,
        shell_local_origin.z,
        shell_c(m_Layer.base_height)};
    // 環境キューブマップも主描画と同じ二つの曲面区間を使う。
    const f32 upper_base_shell_c = has_upper_layer ? shell_c(m_UpperLayer.base_height) : 0.0f;
    const f32 upper_top_shell_c = has_upper_layer ? shell_c(m_UpperLayer.top_height) : 0.0f;
    cb.cloudShellTerms = FVec4{shell_c(m_Layer.top_height), upper_base_shell_c, upper_top_shell_c, 0.0f};
    cb.cloudUpperLayer = has_upper_layer
        ? FVec4{
            m_UpperLayer.base_height,
            m_UpperLayer.top_height,
            1.0f /
                (m_UpperLayer.top_height - m_UpperLayer.base_height),
            1.0f}
        : FVec4{0.0f, 0.0f, 0.0f, 0.0f};
    cb.cloudUpperTerms = samplingTerms.upperTerms;
    cb.cloudLightingExtinction = FVec4{
        m_Lighting.ViewExtinction,
        m_Lighting.LightExtinction,
        m_Lighting.SunScatter,
        m_Lighting.PowderStrength};
    cb.cloudLightingPhase = FVec4{
        m_Lighting.PhaseForward,
        m_Lighting.PhaseBackward,
        m_Lighting.PhaseBlend,
        m_Lighting.MultiScatterContribution};
    cb.cloudLightingMulti = FVec4{
        m_Lighting.MultiScatterOcclusion,
        m_Lighting.PhaseMin,
        m_Lighting.PhaseMax,
        m_Lighting.GroundContribution};
    u32 environment_view_steps = m_Range.ViewSteps > 0u
        ? m_Range.ViewSteps : kVolumetricCloudEnvironmentViewSteps;
    if (environment_view_steps < 32u) environment_view_steps = 32u;
    if (environment_view_steps > 96u) environment_view_steps = 96u;
    cb.cloudLightingAmbient = FVec4{
        m_Lighting.AmbientAtBase,
        m_Lighting.AmbientAtTop,
        static_cast<f32>(environment_view_steps),
        1.0f};
    cb.cloudLightingGround = FVec4{
        m_Lighting.GroundColor.x,
        m_Lighting.GroundColor.y,
        m_Lighting.GroundColor.z,
        m_Lighting.SunScatteringLuminanceScale};
    cb.cloudSunTransmittance = FVec4{
        m_Lighting.SunTransmittance.x,
        m_Lighting.SunTransmittance.y,
        m_Lighting.SunTransmittance.z,
        0.0f};
    const bool split_sky_ambient =
        m_Lighting.SkyZenithColor.x > 0.0f
        || m_Lighting.SkyZenithColor.y > 0.0f
        || m_Lighting.SkyZenithColor.z > 0.0f;
    cb.cloudSkyZenith = FVec4{
        m_Lighting.SkyZenithColor.x,
        m_Lighting.SkyZenithColor.y,
        m_Lighting.SkyZenithColor.z,
        split_sky_ambient ? 1.0f : 0.0f};
    cb.cloudMultiPhase = FVec4{
        m_Lighting.MultiScatterEccentricity,
        0.0f, 0.0f, 0.0f};
    const f32 current_view_distance = ResolveVolumetricCloudViewDistance_Internal(shell_local_origin, m_Layer, m_UpperLayer, has_upper_layer, m_Range.MaxDistance);
    cb.cloudRange = FVec4{
        m_Range.MaxDistance,
        m_Range.MaxDistance * (1.0f - m_Range.FadeFraction),
        m_Range.StepGrowth,
        current_view_distance};

    FViewport viewport{};
    viewport.width =
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize);
    viewport.height =
        static_cast<f32>(kVolumetricCloudEnvironmentCubeSize);
    FScissorRect scissor{};
    scissor.right =
        static_cast<i32>(kVolumetricCloudEnvironmentCubeSize);
    scissor.bottom =
        static_cast<i32>(kVolumetricCloudEnvironmentCubeSize);
    const FClearColor black{0.0f, 0.0f, 0.0f, 1.0f};

    for (u32 face = 0u; face < 6u; ++face) {
        cb.invViewProj = CloudEnvironmentInverseViewProjection(
            face, m_Range.MaxDistance);
        cb.prevCameraRelativeViewProj = Inverse(cb.invViewProj);
        cloud_cb[face]->Update(&cb, sizeof(cb));

        cl.SetComputePipeline(*m_CloudPipe);
        cl.SetConstantBuffer(0, *cloud_cb[face]);
        cl.SetTexture(0, *m_ShapeTex);
        cl.SetTexture(1, *m_NoiseFilterResources->filtered_texture);
        cl.SetTexture(2, *m_WeatherTex);
        cl.SetTexture(3, *m_DetailTex);
        cl.SetTexture(4, *m_CurlTex);
        if (m_ShadowTex && m_ShadowCacheValid) {
            cl.SetTexture(5, *m_ShadowTex);
        } else {
            cl.SetTexture(5, *m_ShapeTex);
        }
        cl.BindUav(0, *cloud_color);
        cl.BindUav(1, *cloud_depth);
        cl.Dispatch(
            (kVolumetricCloudEnvironmentCubeSize + 7u) / 8u,
            (kVolumetricCloudEnvironmentCubeSize + 7u) / 8u,
            1u);

        FCloudEnvironmentCompositeCb composite_data{};
        composite_data.face_index = static_cast<i32>(face);
        composite_cb[face]->Update(
            &composite_data, sizeof(composite_data));
        cl.BeginRenderToTextureSlice(*environment, face, 0u, black);
        cl.SetViewport(viewport);
        cl.SetScissor(scissor);
        cl.SetPipeline(*pipeline);
        cl.SetConstantBuffer(0, *composite_cb[face]);
        cl.SetTexture(0, base_environment);
        cl.SetTexture(1, *cloud_color);
        cl.Draw(3u);
    }
    cl.EndRenderToTexture(*environment);

    return TResult<TUniquePtr<IRhiTexture>>(
        OkInit, Move(environment));
}

FVolumetricCloudWorldShadowMap
CVolumetricClouds::WorldShadowMap() const noexcept {
    FVolumetricCloudWorldShadowMap out{};
    out.transmittance =
        m_WorldShadowValid ? m_WorldShadowTex.Get() : nullptr;
    out.minimum_reference_xz = m_WorldShadowMapMinReferenceXz;
    out.inverse_extent =
        1.0f / kVolumetricCloudWorldShadowMapExtent;
    out.reference_height = m_WorldShadowReferenceHeight;
    out.sun_direction = m_WorldShadowSunDirection;
    out.world_origin = m_WorldShadowWorldOrigin;
    out.cloud_base_altitude = m_WorldShadowCloudBaseAltitude;
    out.planet_radius = kVolumetricCloudPlanetRadius;
    out.resolution = kVolumetricCloudWorldShadowMapResolution;
    return out;
}

FVolumetricCloudWorldShadowMap
CVolumetricClouds::WorldShadowMap(
    const IRhiCommandList& command_list) const noexcept {
    if (!RecordedFramePending() ||
        m_NoiseFilterResources->recorded_frame.command_list != &command_list ||
        m_NoiseFilterResources->recorded_frame.settings_revision !=
            m_NoiseFilterResources->settings_revision) {
        return WorldShadowMap();
    }

    const FNoiseFilterResources::FRecordedFrameState& recorded =
        m_NoiseFilterResources->recorded_frame;
    FVolumetricCloudWorldShadowMap out{};
    out.transmittance = recorded.world_shadow_valid
        ? m_WorldShadowTex.Get() : nullptr;
    out.minimum_reference_xz =
        recorded.world_shadow_map_minimum_reference_xz;
    out.inverse_extent =
        1.0f / kVolumetricCloudWorldShadowMapExtent;
    out.reference_height = recorded.world_shadow_reference_height;
    out.sun_direction = recorded.world_shadow_sun_direction;
    out.world_origin = recorded.world_shadow_world_origin;
    out.cloud_base_altitude =
        recorded.world_shadow_cloud_base_altitude;
    out.planet_radius = kVolumetricCloudPlanetRadius;
    out.resolution = kVolumetricCloudWorldShadowMapResolution;
    return out;
}

void CVolumetricClouds::Composite(IRhiCommandList& cl, IRhiTexture& scene_depth,
                                  u32 scW, u32 scH,
                                  IRhiTexture* atmosphere_volume,
                                  IRhiTexture* atmosphere_transmittance,
                                  f32 atmosphere_max_distance) noexcept {
    const bool recordedCloudFrame = RecordedCloudFramePending();
    const FNoiseFilterResources::FRecordedFrameState* recorded =
        recordedCloudFrame ? &m_NoiseFilterResources->recorded_frame : nullptr;
    if (recorded && recorded->command_list != &cl) return;
    const bool historyValid = recorded
        ? recorded->history_valid : m_HistoryValid;
    const u32 resolvedIndex = recorded
        ? recorded->resolved_index : m_ResolvedIndex;
    if (!m_Ready || !historyValid || !m_HistoryColor[resolvedIndex] ||
        !m_HistoryDepth[resolvedIndex] || !m_CompPipe || !m_Cb) return;
    FViewport vp{}; vp.width = static_cast<f32>(scW); vp.height = static_cast<f32>(scH); cl.SetViewport(vp);
    FScissorRect sr{}; sr.right = static_cast<i32>(scW); sr.bottom = static_cast<i32>(scH); cl.SetScissor(sr);
    const bool useAtmosphere = atmosphere_volume != nullptr &&
                               atmosphere_transmittance != nullptr &&
                               m_CompAtmosPipe && m_CompAtmosCb;
    if (useAtmosphere) {
        FCloudAtmosphereCb cb{};
        cb.atmosphereParams = FVec4{
            atmosphere_max_distance > 0.001f
                ? atmosphere_max_distance : 0.001f,
            0.0f, 0.0f, 0.0f};
        m_CompAtmosCb->Update(&cb, sizeof(cb));
        cl.SetPipeline(*m_CompAtmosPipe);
    } else {
        cl.SetPipeline(*m_CompPipe);
    }
    cl.SetConstantBuffer(0, *m_Cb);
    if (useAtmosphere) cl.SetConstantBuffer(1, *m_CompAtmosCb);
    cl.SetTexture(0, *m_HistoryColor[resolvedIndex]); // UAV→SRV transition は RHI が処理
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, *m_HistoryDepth[resolvedIndex]);
    if (useAtmosphere) cl.SetTexture(3, *atmosphere_volume);
    if (useAtmosphere) cl.SetTexture(4, *atmosphere_transmittance);
    cl.Draw(3, 0);
    if ((recordedCloudFrame || m_LastFrameWorkload.submitted) &&
        m_LastFrameWorkload.composite_draws != ~u32{0}) {
        ++m_LastFrameWorkload.composite_draws;
    }
}

void CVolumetricClouds::Shutdown() noexcept {
    // 非同期処理の中止と寿命の回収は各シェーダー所有型の破棄契約へ委ねる。
    // 状態待ちをここへ重ねると、backend障害時に終了スレッドが永久停止する。
    m_ShapeTex.Reset();
    m_NoiseFilterResources.Reset();
    m_NoisePipe.Reset();
    m_NoiseCs.Reset();
    m_NoiseBaked = false;
    m_WeatherTex.Reset(); m_WeatherPipe.Reset(); m_WeatherCs.Reset(); m_WeatherBaked = false;
    m_DetailTex.Reset(); m_DetailPipe.Reset(); m_DetailCs.Reset(); m_DetailBaked = false;
    m_CurlTex.Reset(); m_CurlPipe.Reset(); m_CurlCs.Reset(); m_CurlBaked = false;
    m_CloudTex.Reset(); m_CloudDepth.Reset();
    for (u32 i = 0; i < 2; ++i) { m_HistoryColor[i].Reset(); m_HistoryDepth[i].Reset(); }
    m_Cb.Reset(); m_CompPipe.Reset(); m_CompPs.Reset(); m_CompVs.Reset();
    m_CompAtmosCb.Reset(); m_CompAtmosPipe.Reset(); m_CompAtmosPs.Reset();
    m_ResolvePipe.Reset(); m_ResolveCs.Reset();
    m_ShadowTex.Reset();
    m_ShadowPipe.Reset(); m_ShadowCs.Reset();
    m_WorldShadowTex.Reset();
    m_WorldShadowPipe.Reset(); m_WorldShadowCs.Reset();
    m_CloudPipe.Reset(); m_CloudCs.Reset();
    m_Ready = false; m_HistoryValid = false;
    m_FrameIndex = 0; m_TemporalPhase = 0; m_ResolvedIndex = 0;
    m_W = 0; m_H = 0; m_FullW = 0; m_FullH = 0;
    m_WorldOrigin = FVec3{};
    m_PrevCameraRelativeViewProj = FMat4::Identity();
    m_PrevCameraRelativeInvViewProj = FMat4::Identity();
    m_PrevCamPos = FVec3{};
    m_PrevSunDir = FVec3{}; m_PrevSunColor = FVec3{}; m_PrevSkyColor = FVec3{};
    m_PrevWindOffset = 0.0f; m_PrevWindSpeed = 0.0f;
    m_PrevCoverage = -1.0f; m_PrevDensity = -1.0f; m_PrevTime = -1.0f;
    m_ShadowGridMinQ = FVec2{}; m_ShadowGridCenterQ = FVec2{};
    m_WorldShadowMapMinReferenceXz = FVec2{};
    m_WorldShadowReferenceHeight = 0.0f;
    m_WorldShadowSunDirection = FVec3{0.0f, 1.0f, 0.0f};
    m_WorldShadowWorldOrigin = FVec3{};
    m_WorldShadowCloudBaseAltitude = 0.0f;
    m_ShadowCacheDispatchCount = 0; m_ShadowGridInitialized = false;
    m_ShadowCacheAvailable = false; m_ShadowCacheValid = false;
    m_WorldShadowDispatchCount = 0u;
    m_WorldShadowAvailable = false; m_WorldShadowValid = false;
    m_WorkloadSubmissionIndex = 0u;
    m_LastFrameWorkload = {};
}

} // namespace acs
