// SPDX-License-Identifier: Apache-2.0
// 手続き生成スカイ実装
#include "render/Sky.h"
#if !WITH_RENDER_DILIGENT
#include "render/Dx12/Dx12Shader.h"
#endif
#include "math/Camera.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cstddef>
#include <cmath>
#include <cstring>

namespace acs {

namespace {

/** スカイの HLSL ソース (フルスクリーン三角形 + 視線方向ベースの空と太陽。VB 不要)。 */
const char* kSkyHLSL = R"(
#pragma pack_matrix(row_major)

cbuffer CSky : register(b0) {
    float4x4 inv_view_proj;       // 画面 NDC → ワールドへの逆変換
    float4   camera_pos;          // xyz=eye
    float4   sun_dir;             // xyz=方向 (camera→sun)
    float4   sun_color;           // xyz=色
    float4   sun_params;          // x=radius (1-cos), y=glow, zw=pad
    float4   zenith_color;
    float4   horizon_color;
    float4   ground_color;
    float4   cloud_params0;       // x=coverage(0..1), y=density(sharpness), z=time, w=enabled(0/1)
    float4   cloud_params1;       // xyz=cloud_color, w=wind_speed
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
    float3 sky;
    if (t >= 0.0) {
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
    sky += sun_color.xyz * forwardGlow;

    // 3) 太陽円盤は画素微分で輪郭だけを滑らかにし、光彩は太陽色への混合を0.28へ制限する。
    sky = lerp(sky, sun_color.xyz, saturate(discWeight + haloWeight));

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

    // 5) Dither: 8-bit 出力時のグラデ縞を消す (HDR 出力時は ±1/255 で実質無影響)。
    //    d2 は軸別オフセット + 別の時間位相で d1 と独立化し、足して TPDF にする。
    float d1 = SkyDither(v.pos.xy, cloud_params0.z);
    float d2 = SkyDither(v.pos.xy + float2(113.0, 71.0), cloud_params0.z * 0.37 + 0.5);
    sky += (d1 + d2 - 1.0) * (1.0 / 255.0);

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

    /** 太陽パラメータ (x=radius(1-cos), y=glow, zw=pad)。 */
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

    // Commit cannot fail. Release the old PSO before the resources it refers
    // to, then publish the replacement PSO last.
    m_Pipeline.Reset();
    m_Vs = Move(shaders.vertex);
    m_Ps = Move(shaders.pixel);
    m_Cb = Move(candidate_cb);
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
void CSky::Render(IRhiCommandList& cl, const CCamera& camera) noexcept {
    if (!m_Pipeline || !m_Cb) return;
    FSkyCb cb{};
    cb.inv_view_proj = BuildCameraRelativeInverseViewProjection(camera.View(), camera.Projection());
    const FVec3 eye = camera.Eye();
    cb.camera_pos = FVec4{eye.x, eye.y, eye.z, 1};
    cb.sun_dir    = FVec4{m_SunDir.x, m_SunDir.y, m_SunDir.z, 0};
    cb.sun_color  = FVec4{m_SunColor.x, m_SunColor.y, m_SunColor.z, 1};
    cb.sun_params = FVec4{m_SunRadius, m_SunGlow, 0, 0};
    cb.zenith     = FVec4{m_Zenith.x, m_Zenith.y, m_Zenith.z, 1};
    cb.horizon    = FVec4{m_Horizon.x, m_Horizon.y, m_Horizon.z, 1};
    cb.ground     = FVec4{m_Ground.x, m_Ground.y, m_Ground.z, 1};
    cb.cloud0     = FVec4{m_FallbackCloudCoverage, m_FallbackCloudDensity,
                          m_FallbackCloudTime, m_FallbackCloudsEnabled ? 1.0f : 0.0f};
    cb.cloud1     = FVec4{m_FallbackCloudColor.x, m_FallbackCloudColor.y,
                          m_FallbackCloudColor.z, m_FallbackCloudWind};
    m_Cb->Update(&cb, sizeof(cb));

    cl.SetPipeline(*m_Pipeline);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.Draw(3);    // VB 無し、SV_VertexID で 3 頂点

}

// ===================== CVolumetricClouds (GPU レイマーチ) =====================
namespace {

// 雲レイマーチ compute。視線ごとに雲スラブを march、Worley FBM 密度を coverage/height で remap、
// 太陽へ light-march (Beer) + dual-lobe HG + 有界な内部散乱近似を積分。出力=非premult色+alpha。
// ---- Perlin-Worley 3D noise を init で 1 回焼く ----
// 128^3 RG16F: R=連続した低周波 Perlin 雲体、G=輪郭用 Perlin-Worley。繰り返し可能。
// per-voxel に worley 27-cell を計算するのは «高価» だが init 1 回だけなので実用 (per-frame march では
// この焼き上がりテクスチャを 1 fetch する → 速くて crisp)。
const char* kNoiseGenCS = R"(
RWTexture3D<float2> noiseOut : register(u0);
float3 h33(float3 p){
    // 超越関数を使わない integer-cell hash。初回 128^3 bake を大幅に高速化し、
    // shader compiler 間でも決定的になる（大きな引数の sin hash は決定的でない）。
    p=frac(p*float3(0.1031,0.1030,0.0973));
    p+=dot(p,p.yxz+33.33);
    return frac((p.xxy+p.yxx)*p.zyx);
}
// tileable gradient (Perlin) noise。freq = 整数セル数。
float gnoise(float3 x, float freq){
    float3 p=floor(x), f=frac(x);
    // C2 連続補間により、強い太陽光で見える cubic grid の折れ目を消す。
    float3 u=f*f*f*(f*(f*6.0-15.0)+10.0);
    float n=0.0;
    [unroll] for(int dz=0;dz<2;dz++) [unroll] for(int dy=0;dy<2;dy++) [unroll] for(int dx=0;dx<2;dx++){
        float3 o=float3(dx,dy,dz);
        float3 g=normalize(h33(fmod(p+o,freq))*2.0-1.0 + 1e-4);
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
        float3 fp=h33(fmod(p+o+freq,freq));            // tileable wrap
        float3 d=o+fp-f;
        md=min(md, dot(d,d));
    }
    return 1.0-sqrt(saturate(md));
}
float remap(float v,float a,float b,float c,float d){
    float span=max(b-a,1e-4);
    return c+saturate((v-a)/span)*(d-c);
}
[numthreads(4,4,4)]
void CSNoise(uint3 id : SV_DispatchThreadID){
    if(id.x>=128u||id.y>=128u||id.z>=128u) return;
    float3 uvw=(float3(id)+0.5)/128.0;
    // R は雲全体を支える連続した低周波形状、G は房状の輪郭侵食に分ける。
    // Perlin-Worley だけを雲体にすると、0の空隙が約6割あるため雲が点群へ分断される。
    float perlin2 = gnoise(uvw*2.0,2.0);
    float perlin4 = gnoise(uvw*4.0,4.0);
    float perlin8 = gnoise(uvw*8.0,8.0);
    float perlin16 = gnoise(uvw*16.0,16.0);
    float perlin32 = gnoise(uvw*32.0,32.0);
    float perlinFull = (perlin4*0.5+perlin8*0.25
                       +perlin16*0.125+perlin32*0.0625)*1.0666667;
    float perlinMacro=perlin2*0.70+perlin4*0.30;
    // 独立した Worley FBM を評価せず cellular octave を再利用する。
    // 削除した 96-cell octave は volume の Nyquist 限界を超え、feature あたり
    // 約 1 voxel の aliasing を加えるだけだった。
    float wa = worley(uvw, 6.0);
    float wb = worley(uvw, 12.0);
    float wc = worley(uvw, 24.0);
    float worleyFull = wa*0.625 + wb*0.25 + wc*0.125;
    // Worley は既に 1-最近接距離で反転済みなので、Perlin の膨張下限は
    // 1-Worley（最近接距離）になる。Worley-1 は符号が逆で分布を0.67付近へ圧縮する。
    float fullShape=remap(perlinFull,1.0-worleyFull,1.0,0.0,1.0);
    noiseOut[id]=float2(perlinMacro,fullShape);
}
)";

// Large-scale, artist-facing weather field.  This is deliberately independent
// from the 3D shape texture: reusing the same tile for coverage, shape and
// erosion exposes the tile period immediately in a ground-to-horizon view.
// R=coverage, G=cloud type, B=precipitation, A=low-frequency warp.
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
    float2 i00=fmod(i+period,period);
    float2 i10=fmod(i+float2(1,0)+period,period);
    float2 i01=fmod(i+float2(0,1)+period,period);
    float2 i11=fmod(i+float2(1,1)+period,period);
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
    float storm=weatherFbm(float2(1.0-uv.y,uv.x)+float2(0.31,0.07),
                           float2(103.7,47.2));
    float warp=weatherFbm(uv+float2(0.53,0.23),float2(151.9,73.4));
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
float detailWorley(float3 x,float frequency,float3 seed) {
    float3 cell=floor(x);
    float3 local=frac(x);
    float minDistance=10.0;
    [unroll] for(int z=-1;z<=1;z++)
    [unroll] for(int y=-1;y<=1;y++)
    [unroll] for(int xOffset=-1;xOffset<=1;xOffset++) {
        float3 offset=float3(xOffset,y,z);
        float3 wrapped=fmod(cell+offset+frequency,frequency);
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
float curlValue(float2 p,float frequency,float2 seed) {
    float2 cell=floor(p);
    float2 f=frac(p);
    float2 u=f*f*f*(f*(f*6.0-15.0)+10.0);
    float2 i00=fmod(cell+frequency,frequency);
    float2 i10=fmod(cell+float2(1,0)+frequency,frequency);
    float2 i01=fmod(cell+float2(0,1)+frequency,frequency);
    float2 i11=fmod(cell+float2(1,1)+frequency,frequency);
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
    float4 layer;      // x=world base Y, y=world top Y, z=XZ noise scale, w=world→canonical Y
    float4 worldOrigin;// xyz=discrete curved-shell tangent origin
    float4 shadowGrid; // xy=material-space min XZ, zw=inverse horizontal extents
    float4 shadowState;// x=valid, y=max tau disagreement, z=1/width/depth, w=1/height
    float4 groundHorizon;// xyz=camera local up, w=ground tangent elevation; <-1 disables
    float4 cloudFrameTerms;// xy=world wind, z=shape scale, w=1/(top-base)
    float4 cloudLightTangent;// xyz=CPU-hoisted Duff/Frisvad tangent
    float4 cloudLightBitangent;// xyz=CPU-hoisted Duff/Frisvad bitangent
    float4 cloudCoverage;// xy=weather thresholds, zw=height targets
    // xy=inverse weather transition widths, z=view fine step, w=light step
    float4 cloudCoverageReciprocals;
    // CPU-hoisted camera/layer terms for the two curved-shell quadratics.
    float4 cloudShellRayOrigin;// xyz=camera from planet centre, w=inner c
    float4 cloudShellTerms;// x=outer c
    // 雲を «光を散らす媒質» として扱うための係数。これまでは shader 内の即値だった。
    // x=見る側の消散, y=光の側の消散, z=太陽光の散乱率, w=周囲散乱源の確率を混ぜる割合
    float4 cloudLightingExtinction;
    // x=前方散乱の鋭さ, y=後方散乱, z=前方の混ぜ率, w=多重散乱の寄与
    float4 cloudLightingPhase;
    // x=多重散乱の消散の弱め方, y=位相の下限, z=位相の上限, w=地面からの照り返し
    float4 cloudLightingMulti;
    // x=雲底が空から受ける割合, y=雲頂が受ける割合
    float4 cloudLightingAmbient;
    // xyz=地面の色 (照り返しに使う)
    float4 cloudLightingGround;
    // xyz=太陽光が雲へ届くまでの大気透過率 (低い太陽で赤くなる)
    float4 cloudSunTransmittance;
    // xyz=天頂の空の色, w=1 なら «雲頂は天頂・雲底は地平» で分ける
    float4 cloudSkyZenith;
    // x=多重散乱に使う位相の鋭さ (0 で等方)
    float4 cloudMultiPhase;
    // x=最大距離, y=薄め始める距離, z=遠いレイの刻みを広げる度合い
    float4 cloudRange;
    // x=上層の底, y=上層の天井, z=1/(天井-底), w=1 なら上層あり
    float4 cloudUpperLayer;
    // x=上層の被覆, y=上層の濃さ, z=上層の光学的深さ尺度, w=上層の光採取基準間隔
    float4 cloudUpperTerms;
    // xy=基本形状の位相ずれ, zw=渦と侵食の位相ずれ
    float4 cloudEvolution;
    // x=目標雲種, y=雲種適用率, z=目標降水成分, w=降水適用率
    float4 cloudWeatherControl;
    // xy=更新する偶奇位置, z=各軸の更新間隔, w=1 なら全更新
    float4 cloudShadowUpdate;
    // xy=基準面上の左下XZ, z=1/範囲, w=基準面のワールドY
    float4 cloudWorldShadowMap;
};
RWTexture2D<float4> cloudOut : register(u0);
RWTexture2D<float2> cloudDepthOut : register(u1); // x=不透明度加重ヒット距離, y=アルファ信頼度
// 平均深さと二標本差の書き込み先。
RWTexture3D<float2> cloudShadowOut : register(u2);
Texture3D<float2> shapeNoise     : register(t0);   // (低周波形状, 全帯域形状)
Texture2D    weatherMap          : register(t1);   // coverage/type/precipitation/warp
Texture3D<float2> detailNoise    : register(t2);   // (低周波房, 三帯域侵食)
Texture2D    curlNoise           : register(t3);   // independent world-space curl field
// CSCloud は、現在フレームに生成した平均深さと二標本差を連続した t4 から読む。
Texture3D<float2> cloudShadowCache : register(t4);
SamplerState shapeNoise_sampler  : register(s0);   // wrap (tileable)
SamplerState weatherMap_sampler  : register(s1);   // world-scale wrap
SamplerState detailNoise_sampler : register(s2);   // wrap (tileable)
SamplerState curlNoise_sampler   : register(s3);   // world-scale wrap
SamplerState cloudShadowCache_sampler : register(s4); // clamp (finite cache footprint)

float remapc(float v,float a,float b,float c,float d){ return c + saturate((v-a)/max(b-a,1e-5))*(d-c); }
float hash13(float3 p){ p=frac(p*0.1031); p+=dot(p,p.zyx+31.32); return frac((p.x+p.y)*p.z); }
float hg(float c,float g){ float g2=g*g; return (1.0-g2)/(12.566370*pow(max(1.0+g2-2.0*g*c,1e-3),1.5)); }

// The CPU-hoisted Duff/Frisvad basis is orthonormal, so
// |sun + tangentOffset*a| is
// analytically sqrt(1+a*a). Avoiding a three-component dot in normalize()
// preserves the exact cone sample direction with less work in every dense
// light probe.
float3 cloudConeDirection(
    float3 sun,float3 lightTangent,float3 lightBitangent,
    float coneSin,float coneCos,float2 coneGeometry){
    float3 lateral=coneCos*lightTangent+coneSin*lightBitangent;
    return (sun+lateral*coneGeometry.x)*coneGeometry.y;
}
// x is the unchanged 0.01*(l+1) cone angle; y is its analytically exact
// 1/sqrt(1+x*x) normalization rounded once to float. The eight sample
// directions are fixed by policy, so an indexed constant removes one runtime
// rsqrt from every dense light probe without moving or deleting a probe.
static const float2 CLOUD_CONE_GEOMETRY[8]={
    float2(0.01,0.999950004),
    float2(0.02,0.999800060),
    float2(0.03,0.999550304),
    float2(0.04,0.999200959),
    float2(0.05,0.998752339),
    float2(0.06,0.998204845),
    float2(0.07,0.997558967),
    float2(0.08,0.996815279)};

static const float CLOUD_PLANET_RADIUS=6360000.0;
// 遠距離5点だけをキャッシュで置き換え、信頼度が不足する場所は正確な積分へ戻す。
static const bool CLOUD_MAIN_SHADOW_CACHE_ENABLED=true;
// All marched points are within MAX_DISTANCE (250 km) of the rebased tangent
// origin, so xz^2/(R+y)^2 stays below 0.0016.  The fourth-order expansion of
// sqrt((R+y)^2+xz^2)-R removes a per-density-sample square root while retaining
// sub-centimetre shell-height accuracy over the complete supported domain.
float cloudAltitude(float3 p){
    float3 local=p-worldOrigin.xyz;
    float radialY=max(CLOUD_PLANET_RADIUS+local.y,1.0);
    float radialXz2=dot(local.xz,local.xz);
    float inverseRadialY=1.0/radialY;
    float q=radialXz2*inverseRadialY;
    return local.y+q*(0.5-q*inverseRadialY*0.125);
}
// 上層に居るか。高度だけで決まるので、視線側と光側で同じ判定になる。
bool inUpperCloudBandFromAltitude(float altitude){
    return cloudUpperLayer.w>0.5 && altitude>=cloudUpperLayer.x;
}
// 密度は各層の厚さを 1.6 の基準幅へ写して積分する。上層でも下層の尺度を使うと、
// 光学的深さが上層厚と下層厚の比で変わるため、既に判定した層から尺度を選ぶ。
float cloudOpticalDepthScaleFromBand(bool upperBand){
    float scale=layer.w;
    if(upperBand) scale=cloudUpperTerms.z;
    return scale;
}
// 光円すいの採取間隔も同じ基準幅へ合わせる。CPU で除算済みの値を選び、密度標本ごとの
// 除算を避ける。
float cloudLightStepFromBand(bool upperBand){
    float lightStep=cloudCoverageReciprocals.w;
    if(upperBand) lightStep=cloudUpperTerms.w;
    return lightStep;
}
// 層の中での高さ (0=底, 1=天井)。上層に居るなら上層の中で測る。
//
// 2 層のあいだの隙間は下層の 1.0 に貼り付くので、cloudProfile がそこで 0 を返し、
// 密度も 0 になる。**隙間を別に扱う必要は無い。**
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
bool sphereRootsFromTerms(float b,float c,
                          out float nearT,out float farT){
    nearT=0.0;
    farT=0.0;
    float disc=b*b-c;
    bool hit=disc>=0.0;
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

// Return only the nearest continuous part of the shell.  Ignoring a possible
// far-side re-entry avoids integrating through the planet and gives stable
// depth/reprojection semantics for cameras below, inside, or above the layer.
bool intersectCloudShell(float3 rayDir,out float t0,out float t1){
    t0=0.0;
    t1=0.0;
    float innerC=cloudShellRayOrigin.w;
    float outerC=cloudShellTerms.x;
    float b=dot(cloudShellRayOrigin.xyz,rayDir);
    float outerNear=0.0,outerFar=0.0;
    bool hitsOuter=sphereRootsFromTerms(
        b,outerC,outerNear,outerFar);
    if(hitsOuter && outerFar>0.0){
        float innerNear=0.0,innerFar=0.0;
        bool hitsInner=sphereRootsFromTerms(
            b,innerC,innerNear,innerFar);
        if(innerC<0.0){
            if(hitsInner && innerFar>0.0){
                t0=max(innerFar,0.0);
                t1=outerFar;
            }
        } else if(outerC<=0.0){
            bool headingInward=b<0.0;
            t1=(headingInward && hitsInner && innerNear>0.0)?innerNear:outerFar;
        } else if(outerNear>0.0){
            t0=outerNear;
            t1=(hitsInner && innerNear>t0)?innerNear:outerFar;
        }
    }
    return t1>t0;
}

// 任意の始点から見た最寄りの雲殻区間を求める。立体物用の透過率地図は画素ごとに
// 始点が異なるため、カメラ専用にCPUで先行計算した係数は使わない。
bool intersectCloudShellFromPosition(float3 rayOrigin,float3 rayDir,out float t0,out float t1){
    t0=0.0;
    t1=0.0;
    float3 local=rayOrigin-worldOrigin.xyz;
    float3 centreOffset=float3(local.x,CLOUD_PLANET_RADIUS+local.y,local.z);
    float outerAltitude=cloudUpperLayer.w>0.5
        ?cloudUpperLayer.y:layer.y;
    float innerC=dot(local.xz,local.xz)
        +(local.y-layer.x)
         *(2.0*CLOUD_PLANET_RADIUS+local.y+layer.x);
    float outerC=dot(local.xz,local.xz)
        +(local.y-outerAltitude)
         *(2.0*CLOUD_PLANET_RADIUS+local.y+outerAltitude);
    float b=dot(centreOffset,rayDir);
    float outerNear=0.0,outerFar=0.0;
    bool hitsOuter=sphereRootsFromTerms(b,outerC,outerNear,outerFar);
    if(hitsOuter&&outerFar>0.0){
        float innerNear=0.0,innerFar=0.0;
        bool hitsInner=sphereRootsFromTerms(b,innerC,innerNear,innerFar);
        if(innerC<0.0){
            if(hitsInner&&innerFar>0.0){
                t0=max(innerFar,0.0);
                t1=outerFar;
            }
        }else if(outerC<=0.0){
            bool headingInward=b<0.0;
            t1=(headingInward&&hitsInner&&innerNear>0.0)
                ?innerNear:outerFar;
        }else if(outerNear>0.0){
            t0=outerNear;
            t1=(hitsInner&&innerNear>t0)?innerNear:outerFar;
        }
    }
    return t1>t0;
}
// 短い8点の光円すい内では雲種と降水量を共有できる。二つの雲種補間を明示して、
// 視線標本で一度だけ求めた同じ重みを各光標本へ再利用する。
float2 cloudProfileTypeWeights(float cloudType){
    return float2(
        smoothstep(0.18,0.52,cloudType),
        smoothstep(0.50,0.84,cloudType));
}
// 柔らかく密な底面と列ごとにずれた上面により、切断された水平な棚を避ける。
// 呼び出し元は詳細侵食でも使う正規化高度を保持する。
float cloudProfileFromTypeWeights(
    float h,float2 typeWeights,float precipitation){
    float4 rise=smoothstep(
        float4(0.0,0.0,0.0,0.0),
        float4(0.055,0.09,0.13,0.10),h.xxxx);
    float4 fall=1.0-smoothstep(float4(0.30,0.54,0.76,0.88),float4(0.56,0.84,0.94,0.98),h.xxxx);
    float2 middle=smoothstep(
        float2(0.12,0.16),float2(0.45,0.62),h.xx);
    float stratus=rise.x*fall.x;
    float stratocumulus=rise.y*fall.y
                      *lerp(0.78,1.0,middle.x);
    float cumulus=rise.z*fall.z
                 *lerp(0.64,1.0,middle.y);
    float profile=lerp(stratus,stratocumulus,typeWeights.x);
    profile=lerp(profile,cumulus,typeWeights.y);
    float storm=rise.w*fall.w;
    return saturate(lerp(profile,storm,precipitation*0.72));
}
float cloudProfile(float h,float cloudType,float precipitation){
    return cloudProfileFromTypeWeights(
        h,cloudProfileTypeWeights(cloudType),precipitation);
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
    float slowPhase=dot(cloudEvolution.xy,localPattern);
    float finePhase=dot(cloudEvolution.zw,localPattern.yx);
    float edgeBase=weather.r*(1.0-weather.r);
    float edgeResponse=16.0*edgeBase*edgeBase;
    return clamp(
        slowPhase*0.38+finePhase*0.32,-0.14,0.14)*edgeResponse;
}
// 実際の被覆境界から中心までの位置で、薄い縁を押し下げ、濃い中心を持ち上げる。
// 生の天候値を使うと、雲量しきい値を越えた柱がほぼ同じ中心判定になり、雲頂が平らになる。
// 層雲では小さく、積雲または降水域では大きくし、局所位相で雲頂を緩やかに変化させる。
float cloudColumnHeightShift(float4 weather,float cloudInterior){
    float core=smoothstep(0.08,0.92,saturate(cloudInterior));
    float verticalType=saturate(max(weather.g,weather.b));
    float amplitude=lerp(0.025,0.18,verticalType);
    float evolvingWarp=clamp(
        weather.a-0.5+cloudLocalConvectionPhase(weather)*0.45,-0.5,0.5);
    float signal=clamp(
        (core-0.45)*1.45+evolvingWarp*0.65,-1.0,1.0);
    return signal*amplitude;
}
// 凝結高度までは雲底を固定し、その上だけを柱ごとに成長または圧縮する。
// u^4(1-u)^2 により両端で傾きも連続し、変形の山を雲頂側2/3へ置く。
// 45.5625u^4(1-u)^2 は最大値1で、許容変形量の全範囲でも高さの単調性を保つ。
// 薄い上層雲では変形を 30% に抑える。
float cloudConvectiveHeight(float h,float columnShift,bool upperBand){
    h=saturate(h);
    const float condensationHeight=0.14;
    float upperHeight=saturate((h-condensationHeight)/(1.0-condensationHeight));
    float upperRemainder=1.0-upperHeight;
    float upperHeightSquared=upperHeight*upperHeight;
    float upperInterior=45.5625*upperHeightSquared*upperHeightSquared
                       *upperRemainder*upperRemainder;
    float bandScale=upperBand?0.30:1.0;
    float shiftedUpper=saturate(upperHeight-columnShift*upperInterior*bandScale);
    return h<=condensationHeight?h:lerp(condensationHeight,1.0,shiftedUpper);
}
// bake 済み volume は tile あたり 4..32 cells を既に含む。world frequency を下げ、
// 小さな blob の反復ではなく連続した cloud bank を作る。
float cloudShapeScale(){
    // CPU mirrors clamp(layer.z*0.006,0.00012,0.00045) once per frame.
    return cloudFrameTerms.z;
}
float2 cloudWindWorld(){
    // CPU mirrors params.z*float2(0.9284767,0.3713907) once per frame.
    return cloudFrameTerms.xy;
}
// 高さ方向へ同じ2D被覆を押し出さず、実際の風の鉛直差に相当する緩やかなS字変形を与える。
// 上層雲は薄いため変形を25%へ抑え、層全体が横へ流れすぎないようにする。
float cloudWeatherVerticalBend(float layerHeight,bool upperBand){
    float centeredHeight=saturate(layerHeight)*2.0-1.0;
    float bend=centeredHeight*lerp(
        0.35,1.0,abs(centeredHeight));
    return bend*(upperBand?0.25:1.0);
}
float4 cloudWeatherData(
    float3 p,float layerHeight,bool upperBand){
    float2 xz=p.xz-cloudWindWorld();
    // Pack the two independent 2D rotations so FXC emits one vector multiply
    // and one vector MAD instead of four scalar dot-product paths.
    float4 rotated=
        xz.x*float4(0.8660254,0.5,0.9563048,-0.2923717)
       +xz.y*float4(-0.5,0.8660254,0.2923717,0.9563048);
    float4 weatherUv=
        rotated/float4(65536.0,65536.0,9127.0,9127.0)
       +float4(0.173,0.417,0.619,0.281);
    // 総観規模と地域規模を別方向へ曲げる。下端から上端までの移動量は約400～750 mで、
    // 2.6 km層を直立した2D柱へせず、かつ雲塊そのものを別地点へ飛ばさない範囲に収める。
    float verticalBend=cloudWeatherVerticalBend(
        layerHeight,upperBand);
    weatherUv+=verticalBend*float4(0.004,-0.003,-0.032,0.041);
    // 飛行規模の総観領域と回転した地域領域を混ぜ、短い繰り返し周期を隠す。
    float4 a=weatherMap.SampleLevel(
        weatherMap_sampler,weatherUv.xy,0);
    float4 b=weatherMap.SampleLevel(
        weatherMap_sampler,weatherUv.zw,0);
    float4 weather=lerp(a,b,0.45);
    weather.r=saturate((weather.r-0.045)*1.095);
    // 二領域を混ぜた雲種は主に 0.42～0.66 に収まる。この実分布を全範囲へ広げ、
    // 中央値付近を積雲へ飽和させず、層雲、層積雲、積雲の高さ形状を使い分ける。
    weather.g=smoothstep(0.42,0.66,weather.g);
    weather.b=max(a.b,b.b*0.72);
    // 手続き模様を消さずに目的の天候へ寄せる。被覆の時間変化より先に適用し、
    // 視線密度、自己影、立体物用雲影が同じ雲種と降水成分を共有する。
    weather.g=lerp(weather.g,cloudWeatherControl.x,cloudWeatherControl.y);
    weather.b=lerp(weather.b,cloudWeatherControl.z,cloudWeatherControl.w);
    // 成長後の被覆をこの天候値に収め、占有判定、詳細密度、自己影で共有する。
    weather.r=saturate(
        weather.r+cloudWeatherCoverageEvolution(weather));
    return weather;
}
float3 rotateNoise(float3 q){
    return float3(
        dot(q,float3(0.000,0.800,0.600)),
        dot(q,float3(-0.707,-0.424,0.566)),
        dot(q,float3(0.707,-0.424,0.566)));
}
float2 cloudCurlOffset(float3 p){
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
    return a*0.68+b.yx*0.32;
}
float3 cloudUVW(
    float3 p,float4 weather,float2 cachedCurl,float cachedHeight){
    float shapeScale=cloudShapeScale();
    float2 xz=p.xz-cloudWindWorld();
    float warpAngle=weather.g*6.2831853+weather.a*2.17;
    float warpSin,warpCos;
    sincos(warpAngle,warpSin,warpCos);
    float2 weatherWarp=float2(warpCos,warpSin)
                      *(weather.a-0.5)*190.0;
    float2 curlWarp=cachedCurl*22.0;
    // XZ follows physical world scale.  Y uses normalized altitude so lowering
    // the horizontal frequency cannot collapse the volume into one stretched
    // slice.
    float canonicalY=cachedHeight*0.78
                    +weather.g*0.09+weather.a*0.05+0.07;
    return float3((xz.x+weatherWarp.x+curlWarp.x)*shapeScale,
                  canonicalY,
                  (xz.y+weatherWarp.y+curlWarp.y)*shapeScale);
}
// 低周波 Perlin 雲体の実測範囲は約0.34～0.66である。雲量が増えるほどしきい値を
// 分布中心より下げ、2D天候場の内部で連続した3D雲体を広げる。
float cloudThr(float coverage){ return lerp(0.50,0.34,saturate(coverage)); }
float cloudHeightThresholdTarget(float coverage){
    return cloudThr(min(coverage,0.72));
}
float cloudHeightThresholdFromTarget(
    float thresholdTarget,float profileShape){
    return lerp(0.62,thresholdTarget,profileShape);
}
float cloudHeightThreshold(float coverage,float profileShape){
    return cloudHeightThresholdFromTarget(
        cloudHeightThresholdTarget(coverage),profileShape);
}
// 低周波の天候中心だけを連続した雲体としてわずかに広げる。低周波 Perlin の
// 標準偏差は約0.072なので、その半分を越えてしきい値を下げず、中心を一枚岩にしない。
// 雲底と雲頂では0へ戻し、2D天候場を層全体へ押し出した板も作らない。
float cloudWeatherCoreShapeOffset(
    float weatherMask,float height,bool upperBand){
    float core=smoothstep(0.18,0.82,saturate(weatherMask));
    float body=smoothstep(0.10,0.30,saturate(height))
              *(1.0-smoothstep(0.82,0.98,saturate(height)));
    return core*body*(upperBand?0.015:0.030);
}
// 高さ分布を途中で飽和させず、底面から雲頂まで連続して形状しきい値へ反映する。
// 1.5乗は薄い上下端を強く絞り、中心の密度1は変えない。
float cloudVerticalProfileShape(float sampledProfile){
    float profile=saturate(sampledProfile);
    return profile*sqrt(profile);
}
// 詳細体積の二領域差が基本形状を動かせる最大量。
// 雲頂ほど房状の盛り上がりを強くし、雲底は輪郭が沸騰しない範囲へ抑える。
float cloudBillowMaximumOffset(float height){
    return lerp(0.018,0.130,smoothstep(0.18,0.92,saturate(height)));
}
// 合成値 G から 2・3 段目だけを復元する。生成式は G=0.55R+0.30B+0.15C なので、
// R を除いた値は (G-0.55R)/0.45 で求められ、追加の体積採取を必要としない。
float cloudDetailMiddleBand(float2 detailBands){
    return saturate((detailBands.g-detailBands.r*0.55)*(1.0/0.45));
}
// 雲底では大きな房を保ち、雲頂へ近づくほど中間帯域を混ぜて積雲の段階的な膨らみを作る。
// 二領域の差と合計1の重みを使うため、既存の最大変形量は厳密な上限のまま保たれる。
float cloudBillowOffset(float2 detailA,float2 detailB,float height,float middleVisibility){
    float topMiddleWeight=0.48*smoothstep(0.38,0.90,saturate(height))*saturate(middleVisibility);
    float coarseDifference=detailA.r-detailB.r;
    float middleDifference=cloudDetailMiddleBand(detailA)-cloudDetailMiddleBand(detailB);
    return lerp(coarseDifference,middleDifference,topMiddleWeight)*cloudBillowMaximumOffset(height);
}
// 低周波形状を雲体側へ満たしてから高さ分布で支える。平方根は0と1および空領域を保ち、
// 線形密度の薄い内部だけを持ち上げるため、細部ノイズを雲体そのものにしない。
float cloudDimensionalDensity(float baseDensity,float heightProfile){
    return sqrt(saturate(baseDensity))*saturate(heightProfile);
}
// 2D 天候場を完成密度へ乗算すると、同じ XZ の上下が同じ割合で透けて縦の幕になる。
// 雲縁ほど 3D 基本形状を削り、強い房だけを残すことで被覆境界にも立体的な穴を作る。
float cloudWeatherShapeErosion(float weatherMask){
    // weatherMask は被覆しきい値から既に滑らかに正規化済みなので、再補間しない。
    float edge=1.0-saturate(weatherMask);
    // 境界外は完全に閉じ、内側では急速に弱めて晴天時の雲量を失わない。
    float edgeSquared=edge*edge;
    return edgeSquared*edge*0.65;
}
float cloudWeatheredBaseNoise(float baseNoise,float weatherMask){
    return baseNoise-cloudWeatherShapeErosion(weatherMask);
}
// 採取間隔が各帯域の半周期へ近づく前に細部を消し、別の低周波模様への折り返しを防ぐ。
float cloudShapeFrequencyVisibility(
    float sampleSpacing,float domainScale,float frequency){
    float footprint=max(sampleSpacing,0.0)*cloudShapeScale()
                    *domainScale*frequency;
    return 1.0-smoothstep(0.22,0.52,footprint);
}
// R の連続した低周波雲体を保ち、G の Perlin-Worley は近景の輪郭だけを侵食する。
// 乗算なので G の空隙だけで雲を消さず、遠景では侵食を外して大きな形状だけを残す。
float cloudBaseShapeBand(float2 shapeBands,float sampleSpacing,float domainScale,float height){
    float fineVisibility=cloudShapeFrequencyVisibility(
        sampleSpacing,domainScale,32.0);
    float fineSignal=smoothstep(0.01,0.28,saturate(shapeBands.g));
    float topDetail=smoothstep(0.35,0.90,saturate(height));
    float maximumErosion=fineVisibility*lerp(0.14,0.26,topDetail);
    return shapeBands.r*lerp(1.0-maximumErosion,1.0,fineSignal);
}
// 各変換領域の最小形状が採取できない場合は、その領域の読み出し自体を省略する。
float cloudShapeDomainVisibility(
    float sampleSpacing,float domainScale){
    return cloudShapeFrequencyVisibility(sampleSpacing,domainScale,12.0);
}
// 追加領域は第1領域の支持範囲を越えて雲を発生させず、既存の雲体だけを弱く侵食する。
// 実測90百分位付近までを滑らかに正規化し、値0でも最大侵食率を越えない。
float cloudGovernedShapeErosion(float governedShape,float erosionShape,float maximumErosion){
    float erosionSignal=smoothstep(0.01,0.28,saturate(erosionShape));
    return governedShape*lerp(1.0-saturate(maximumErosion),1.0,erosionSignal);
}
// 二領域を混ぜた被覆値の実測百分位へ合わせ、入力0.1/0.5/0.9が
// およそ10%/52%/90%の正の被覆領域になるようにする。
float cloudWeatherThreshold(float coverage){
    return lerp(0.72,0.36,saturate(coverage));
}
float cloudWeatherMaskFromThreshold(float4 weather,float threshold){
    return smoothstep(threshold,min(threshold+0.14,0.98),weather.r);
}
float cloudWeatherMaskFromTerms(
    float4 weather,float threshold,float inverseTransitionWidth){
    float t=saturate((weather.r-threshold)*inverseTransitionWidth);
    return t*t*(3.0-2.0*t);
}
float cloudWeatherMask(float4 weather,float coverage){
    return cloudWeatherMaskFromThreshold(
        weather,cloudWeatherThreshold(coverage));
}
void cloudBaseShape(
    float3 uvw,float rejectionThreshold,float sampleSpacing,float height,
    out float shapeResult){
    shapeResult=0.0;
    float2 a=shapeNoise.SampleLevel(shapeNoise_sampler,uvw,0);
    // 第1領域だけで雲体を作り、残りは乗算侵食で輪郭と繰り返しだけを崩す。
    // 後段で形状値は増えないため、各採取の前後でしきい値未満なら厳密に棄却できる。
    float shape=cloudBaseShapeBand(a,sampleSpacing,1.0,height);
    float bWeight=0.04*cloudShapeDomainVisibility(sampleSpacing,1.83);
    float cWeight=0.02*cloudShapeDomainVisibility(sampleSpacing,3.17);
    float dWeight=0.01*cloudShapeDomainVisibility(sampleSpacing,4.73);
    [branch] if(shape<rejectionThreshold-1e-5) return;
    [branch] if(bWeight>0.0){
        float3 uvwB=rotateNoise(uvw)*1.83
                   +float3(0.371,0.119,0.733)
                   +float3(cloudEvolution.x,cloudEvolution.y,-cloudEvolution.x);
        float2 b=shapeNoise.SampleLevel(shapeNoise_sampler,uvwB,0);
        shape=cloudGovernedShapeErosion(shape,b.g,bWeight);
    }
    [branch] if(shape<rejectionThreshold-1e-5) return;
    [branch] if(cWeight>0.0){
        float3 uvwC=float3(
            dot(uvw,float3(0.707,0.183,-0.683)),
            dot(uvw,float3(-0.354,0.930,-0.098)),
            dot(uvw,float3(0.612,0.319,0.724)))*3.17
            +float3(0.817,0.293,0.157)
            +float3(-cloudEvolution.y,cloudEvolution.x,cloudEvolution.y);
        float2 c=shapeNoise.SampleLevel(shapeNoise_sampler,uvwC,0);
        shape=cloudGovernedShapeErosion(shape,c.g,cWeight);
    }
    [branch] if(shape<rejectionThreshold-1e-5) return;
    // 四つ目の非整数倍率領域は、近景でだけ共通模様を崩す。遠景では読み出さず、
    // 画素より小さい形状を別の模様へ折り返さない。
    [branch] if(dWeight>0.0){
        float3 uvwD=float3(
            dot(uvw,float3(0.433,-0.782,0.448)),
            dot(uvw,float3(0.862,0.501,0.073)),
            dot(uvw,float3(-0.267,0.355,0.896)))*4.73
            +float3(0.263,0.887,0.491)
            +float3(cloudEvolution.y,-cloudEvolution.x,cloudEvolution.x);
        float2 d=shapeNoise.SampleLevel(shapeNoise_sampler,uvwD,0);
        shape=cloudGovernedShapeErosion(shape,d.g,dWeight);
    }
    shapeResult=saturate(shape);
}
// 光円すいは異なる8地点で形状を積分するため、三領域までで近似する。
// 四つ目の輪郭崩しは、見た目へ直接現れる視線だけが負担する。
void cloudBaseShapeLighting(
    float3 uvw,float rejectionThreshold,float sampleSpacing,float height,
    out float shapeResult){
    shapeResult=0.0;
    float2 a=shapeNoise.SampleLevel(shapeNoise_sampler,uvw,0);
    float shape=cloudBaseShapeBand(a,sampleSpacing,1.0,height);
    float bWeight=0.04*cloudShapeDomainVisibility(sampleSpacing,1.83);
    float cWeight=0.02*cloudShapeDomainVisibility(sampleSpacing,3.17);
    [branch] if(shape<rejectionThreshold-1e-5) return;
    [branch] if(bWeight>0.0){
        float3 uvwB=rotateNoise(uvw)*1.83
                   +float3(0.371,0.119,0.733)
                   +float3(cloudEvolution.x,cloudEvolution.y,-cloudEvolution.x);
        float2 b=shapeNoise.SampleLevel(shapeNoise_sampler,uvwB,0);
        shape=cloudGovernedShapeErosion(shape,b.g,bWeight);
    }
    [branch] if(shape<rejectionThreshold-1e-5) return;
    [branch] if(cWeight>0.0){
        float3 uvwC=float3(
            dot(uvw,float3(0.707,0.183,-0.683)),
            dot(uvw,float3(-0.354,0.930,-0.098)),
            dot(uvw,float3(0.612,0.319,0.724)))*3.17
            +float3(0.817,0.293,0.157)
            +float3(-cloudEvolution.y,cloudEvolution.x,cloudEvolution.y);
        float2 c=shapeNoise.SampleLevel(shapeNoise_sampler,uvwC,0);
        shape=cloudGovernedShapeErosion(shape,c.g,cWeight);
    }
    shapeResult=saturate(shape);
}

// View marching used to evaluate weather, curl and all base-shape lobes once
// for occupancy, then repeat the exact same work for detailed density. Keep
// the macro result local to one ray sample so detail erosion adds only its
// independent high-frequency fetches. heightThreshold is also the exact value
// used to reject the base fetch, so shape/density consumption never rebuilds
// the same profile/coverage expression inside each light probe.
struct CloudMacroSample {
    float4 weather;
    float2 curl;
    float baseNoise;
    float weatherMask;
    // 最終密度の被覆境界から雲柱内部までを表す補間値。
    float densityWeatherMask;
    // 高さ形状が雲体密度を支える割合。しきい値だけでなく完成密度にも適用する。
    float heightProfile;
    float heightThreshold;
    float height;
    // 高度計算で確定した層。後段の密度と積分尺度で再利用し、同じ高度を再計算しない。
    float upperBand;
};
CloudMacroSample sampleCloudMacro(
    float3 p,float4 coverageTerms,float sampleSpacing,
    out float3 sampleUvw,out float densityHeightThreshold){
    CloudMacroSample macro;
    sampleUvw=float3(0,0,0);
    densityHeightThreshold=0.62;
    macro.weather=float4(0,0,0,0);
    macro.curl=float2(0,0);
    macro.baseNoise=0.0;
    macro.weatherMask=0.0;
    macro.densityWeatherMask=0.0;
    macro.heightProfile=0.0;
    macro.heightThreshold=0.62;
    macro.height=0.0;
    macro.upperBand=0.0;
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(
        altitude,upperBand);
    macro.weather=cloudWeatherData(p,layerHeight,upperBand);
    macro.weatherMask=cloudWeatherMaskFromTerms(
        macro.weather,coverageTerms.x,cloudCoverageReciprocals.x);
    macro.upperBand=upperBand?1.0:0.0;
    // 形状による空間棄却も上層の被覆倍率に合わせ、空を一様な二層目で閉じない。
    if(upperBand) macro.weatherMask*=cloudUpperTerms.x;
    if(macro.weatherMask>0.001){
        // 空領域判定の広い被覆ではなく、最終密度と同じ境界から柱内の位置を求める。
        // 後段の密度でもこの値を再利用し、被覆計算を重ねない。
        macro.densityWeatherMask=cloudWeatherMaskFromTerms(macro.weather,coverageTerms.y,cloudCoverageReciprocals.y);
        macro.height=cloudConvectiveHeight(
            layerHeight,
            cloudColumnHeightShift(
                macro.weather,macro.densityWeatherMask),
            upperBand);
        float sampledProfile=cloudProfile(
            macro.height,macro.weather.g,macro.weather.b);
        if(sampledProfile>0.0){
            float profileShape=cloudVerticalProfileShape(
                sampledProfile);
            macro.heightThreshold=cloudHeightThresholdFromTarget(
                coverageTerms.z,profileShape)
                -cloudWeatherCoreShapeOffset(
                    macro.weatherMask,macro.height,upperBand);
            densityHeightThreshold=cloudHeightThresholdFromTarget(
                coverageTerms.w,profileShape)
                -cloudWeatherCoreShapeOffset(
                    macro.densityWeatherMask,macro.height,upperBand);
            macro.heightProfile=saturate(sampledProfile);
            macro.curl=cloudCurlOffset(p);
            sampleUvw=cloudUVW(
                p,macro.weather,macro.curl,macro.height);
            // 後段の詳細体積が外側へ膨らませられる最大量を早期棄却にも含める。
            cloudBaseShape(
                sampleUvw,
                macro.heightThreshold-cloudBillowMaximumOffset(macro.height),
                sampleSpacing,
                macro.height,
                macro.baseNoise);
        }
    }
    return macro;
}
CloudMacroSample sampleCloudMacroLighting(
    float3 p,float weatherCoverage,float sampleSpacing){
    CloudMacroSample macro;
    macro.weather=float4(0,0,0,0);
    macro.curl=float2(0,0);
    macro.baseNoise=0.0;
    macro.weatherMask=0.0;
    macro.densityWeatherMask=0.0;
    macro.heightProfile=0.0;
    macro.heightThreshold=0.62;
    macro.height=0.0;
    macro.upperBand=0.0;
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(
        altitude,upperBand);
    macro.weather=cloudWeatherData(p,layerHeight,upperBand);
    macro.weatherMask=cloudWeatherMask(
        macro.weather,weatherCoverage);
    macro.densityWeatherMask=macro.weatherMask;
    macro.upperBand=upperBand?1.0:0.0;
    if(macro.weatherMask>0.001){
        macro.height=cloudConvectiveHeight(
            layerHeight,
            cloudColumnHeightShift(
                macro.weather,macro.densityWeatherMask),
            upperBand);
        float sampledProfile=cloudProfile(
            macro.height,macro.weather.g,macro.weather.b);
        if(sampledProfile>0.0){
            float profileShape=cloudVerticalProfileShape(
                sampledProfile);
            macro.heightThreshold=cloudHeightThreshold(
                weatherCoverage,profileShape)
                -cloudWeatherCoreShapeOffset(
                    macro.weatherMask,macro.height,upperBand);
            macro.heightProfile=saturate(sampledProfile);
            macro.curl=cloudCurlOffset(p);
            cloudBaseShapeLighting(
                cloudUVW(
                    p,macro.weather,macro.curl,macro.height),
                macro.heightThreshold-cloudBillowMaximumOffset(macro.height),
                sampleSpacing,
                macro.height,
                macro.baseNoise);
        }
    }
    return macro;
}
// 近距離3点では視線標本の天候と渦を共有し、基本形状と高度だけを各地点で採取する。
// 遠距離5点は影キャッシュ、または地点ごとに天候を再採取する専用経路へ任せる。
CloudMacroSample sampleCloudMacroLightingFromSlowFields(float3 p,float slowWeatherMask,float heightThresholdTarget,float4 slowProfileTerms,float2 slowCurl,float3 referenceP,float3 referenceUvw,float referenceHeight,float shapeScale,float sampleSpacing){
    CloudMacroSample macro;
    macro.weather=float4(0.0,0.0,slowProfileTerms.z,0.0);
    macro.curl=slowCurl;
    macro.baseNoise=0.0;
    macro.weatherMask=0.0;
    macro.densityWeatherMask=0.0;
    macro.heightProfile=0.0;
    macro.heightThreshold=0.62;
    macro.height=0.0;
    macro.upperBand=0.0;
    // 短い光円すい内では天候場と設定被覆が同じなので、計算済みの生の被覆を共有する。
    // 上層倍率は密度を確定する関数で適用し、ここでは一時値を呼び出し元へ持ち出さない。
    macro.weatherMask=slowWeatherMask;
    macro.densityWeatherMask=slowWeatherMask;
    // 呼び出し元は視線密度がしきい値を超えた場合だけ到達するため、生の被覆は必ず有効である。
    // 同じ判定を近距離3点ごとに繰り返さず、高さと基本形状だけを採取点ごとに更新する。
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    macro.upperBand=upperBand?1.0:0.0;
    macro.height=cloudConvectiveHeight(heightFractionFromAltitude(altitude,upperBand),slowProfileTerms.w,upperBand);
    float sampledProfile=cloudProfileFromTypeWeights(macro.height,slowProfileTerms.xy,slowProfileTerms.z);
    if(sampledProfile>0.0){
        float profileShape=cloudVerticalProfileShape(sampledProfile);
        macro.heightThreshold=cloudHeightThresholdFromTarget(heightThresholdTarget,profileShape)
            -cloudWeatherCoreShapeOffset(
                macro.weatherMask,macro.height,upperBand);
        macro.heightProfile=saturate(sampledProfile);
        // 共有した天候と渦の変換は光円すい内で線形なので、視線側の座標から採取点を復元する。
        float3 lightingUvw=referenceUvw+float3((p.x-referenceP.x)*shapeScale,(macro.height-referenceHeight)*0.78,(p.z-referenceP.z)*shapeScale);
        cloudBaseShapeLighting(
            lightingUvw,
            macro.heightThreshold-cloudBillowMaximumOffset(macro.height),
            sampleSpacing,
            macro.height,
            macro.baseNoise);
    }
    return macro;
}
// 高度による雲底側の増密と降水域の増密を一つにまとめ、全ての密度経路で共有する。
float cloudHeightPrecipitationDensityScale(float height,float precipitation){
    return lerp(1.10,0.92,height)*lerp(1.0,1.28,precipitation);
}
// キャッシュを使えない遠距離の光標本を、地点ごとの天候で再構成する。
// 渦の形状移動は最大44 mなので視線標本から共有し、天候の最小約315 m模様は共有しない。
// 密度と、その地点の層に対応する光学的深さ尺度だけを返し、構造体の寿命を広げない。
float2 sampleCloudFarLightingDensityAndScale(
    float3 p,float weatherCoverage,float2 slowCurl,float sampleSpacing){
    float densityResult=0.0;
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(
        altitude,upperBand);
    float4 weather=cloudWeatherData(p,layerHeight,upperBand);
    float weatherMask=cloudWeatherMask(weather,weatherCoverage);
    if(weatherMask>0.001){
        float sampleHeight=cloudConvectiveHeight(
            layerHeight,cloudColumnHeightShift(weather,weatherMask),
            upperBand);
        float sampledProfile=cloudProfile(
            sampleHeight,weather.g,weather.b);
        if(sampledProfile>0.0){
            float profileShape=cloudVerticalProfileShape(
                sampledProfile);
            float heightThreshold=cloudHeightThreshold(
                weatherCoverage,profileShape)
                -cloudWeatherCoreShapeOffset(
                    weatherMask,sampleHeight,upperBand);
            float heightProfile=saturate(sampledProfile);
            float baseNoise=0.0;
            cloudBaseShapeLighting(cloudUVW(p,weather,slowCurl,sampleHeight),heightThreshold,sampleSpacing,sampleHeight,baseNoise);
            if(upperBand) weatherMask*=cloudUpperTerms.x;
            float weatheredBaseNoise=cloudWeatheredBaseNoise(
                baseNoise,weatherMask);
            float baseDensity=remapc(
                weatheredBaseNoise,heightThreshold,
                min(heightThreshold+0.22,0.98),0.0,1.0);
            float dimensionalDensity=cloudDimensionalDensity(baseDensity,heightProfile);
            if(dimensionalDensity>0.001){
                densityResult=saturate(dimensionalDensity*cloudHeightPrecipitationDensityScale(sampleHeight,weather.b));
            }
            if(upperBand) densityResult*=cloudUpperTerms.y;
        }
    }
    return float2(
        densityResult,cloudOpticalDepthScaleFromBand(upperBand));
}
float cloudShapeFromPositiveWeatherMacro(CloudMacroSample macro){
    float shapeResult=0.0;
    if(macro.heightProfile>0.0){
        // 詳細体積の最大膨張まで占有領域へ含め、房の外縁を採取前に捨てない。
        float envelopeNoise=cloudWeatheredBaseNoise(
            macro.baseNoise+cloudBillowMaximumOffset(macro.height),
            macro.weatherMask);
        // 詳細体積が到達できる最大形状にも完成密度と同じ非線形変換を使い、
        // 平方根で見えるようになった薄い支持領域を空間棄却で失わない。
        float envelopeBaseDensity=remapc(envelopeNoise,macro.heightThreshold,min(macro.heightThreshold+0.22,0.98),0.0,1.0);
        shapeResult=cloudDimensionalDensity(envelopeBaseDensity,macro.heightProfile);
    }
    return shapeResult;
}
float cloudShapeFromMacro(CloudMacroSample macro){
    float shapeResult=0.0;
    if(macro.weatherMask>0.001){
        shapeResult=cloudShapeFromPositiveWeatherMacro(macro);
    }
    return shapeResult;
}
// empty-space reject と light marching 用の粗い density。
float cloudShape(float3 p, float coverage){
    float shapeResult=0.0;
    CloudMacroSample macro=sampleCloudMacroLighting(p,coverage,0.0);
    shapeResult=cloudShapeFromMacro(macro);
    if(macro.upperBand>0.5) shapeResult*=cloudUpperTerms.x;
    return shapeResult;
}
// 二つの詳細領域で共通する回転成分を一度だけ求め、密度標本ごとの行列積を減らす。
// R の房形状は約 0.8～1.8 km、G の侵食形状は約 0.2～0.45 km を担当する。
// 旧尺度は最小形状が約 30 mしかなく、地平線の採取間隔で別の模様へ化けて粒状になっていた。
void cloudDetailDomains(
    float2 detailXz,float worldY,
    out float3 detailDomainA,out float3 detailDomainB){
    float3 horizontal=float3(
        detailXz.y*0.600,
        -detailXz.x*0.707+detailXz.y*0.566,
         detailXz.x*0.707+detailXz.y*0.566);
    float3 vertical=worldY*float3(0.800,-0.424,-0.424);
    // 高さ方向を横方向より緩やかに変え、上下へ連続する積雲の房を作る。
    detailDomainA=horizontal*0.00018+vertical*0.00014;
    detailDomainB=horizontal*0.00031+vertical*0.00024;
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
// 中間房と高周波侵食は一標本積分で揺れやすいため、最小周期の約4分の1までに消す。
float cloudErosionVisibilityFromSampleSpacing(float sampleSpacing){
    return cloudDetailFrequencyVisibility(sampleSpacing,16.0,0.05,0.24);
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
// 粗い採取点から一つ前の区間へ戻し、細密刻み内の採取位相だけを再適用する。
// 参照描画の 0.5 は区間中央となり、通常描画の乱数位相も粗密切り替えで失われない。
float cloudRefinedSampleT(float intervalStart,float coarseProbeT,float fineStep,float coarseStep,float jitter){
    float coarseCellStart=max(coarseProbeT-coarseStep,intervalStart);
    return coarseCellStart+jitter*fineStep;
}
// 通常描画では各細密区間の採取位置を一定周期を持たない列でずらし、周期形状との共振を防ぐ。
// 参照描画は時間平均を使わないため、従来どおり全区間の中央を採取する。
float cloudRayIntervalPhase(float basePhase,int intervalIndex){
    float samplePhase=0.5;
    if(cloudLightingAmbient.w<0.5){
        samplePhase=frac(basePhase+float(intervalIndex)*0.41421356237);
    }
    return samplePhase;
}
// 内部散乱確率用の低 LOD density。detail texture を読まず、最終 density と同じ
// weather/profile scale を保つ。
float cloudLowLodDensityFromPositiveWeatherMacro(CloudMacroSample macro,float heightThreshold,float weatherMask){
    float densityResult=0.0;
    if(macro.heightProfile>0.0){
        float weatheredBaseNoise=cloudWeatheredBaseNoise(
            macro.baseNoise,weatherMask);
        float baseDensity=remapc(weatheredBaseNoise,heightThreshold,min(heightThreshold+0.22,0.98),0.0,1.0);
        float dimensionalDensity=cloudDimensionalDensity(baseDensity,macro.heightProfile);
        if(dimensionalDensity>0.001){
            float h=saturate(macro.height);
            densityResult=saturate(dimensionalDensity*cloudHeightPrecipitationDensityScale(h,macro.weather.b));
        }
    }
    return densityResult;
}
// 詳細表示用密度。低周波の房と高周波の侵食を別々の採取限界で減衰させる。
float cloudDensityFromPositiveWeatherMacro(float3 p,CloudMacroSample macro,float heightThreshold,float weatherMask,float billowVisibility,float erosionVisibility){
    float densityResult=0.0;
    if(macro.heightProfile>0.0){
        float h=saturate(macro.height);
        float weatheredBaseNoise=cloudWeatheredBaseNoise(
            macro.baseNoise,weatherMask);
        float baseDensity=remapc(weatheredBaseNoise,heightThreshold,min(heightThreshold+0.22,0.98),0.0,1.0);
        // 基本形状の外側でも、詳細体積が到達できる範囲だけは密度評価へ進める。
        float envelopeBaseDensity=remapc(
            cloudWeatheredBaseNoise(
                macro.baseNoise+cloudBillowMaximumOffset(h),weatherMask),
            heightThreshold,
            min(heightThreshold+0.22,0.98),0.0,1.0);
        float envelopeDensity=cloudDimensionalDensity(envelopeBaseDensity,macro.heightProfile);
        if(envelopeDensity>0.001){
            // 高さ形状と被覆を含む実際の粗密度を先に確定し、その表面を侵食する。
            // 基本密度だけを先に侵食すると、値1の雲芯では細部が消えて滑らかな高さ面が残る。
            float densityScale=cloudHeightPrecipitationDensityScale(h,macro.weather.b);
            float coarseDensity=saturate(cloudDimensionalDensity(baseDensity,macro.heightProfile)*densityScale);
            float d=coarseDensity;
            billowVisibility=saturate(billowVisibility);
            erosionVisibility=saturate(erosionVisibility);
            float detailVisibility=max(
                billowVisibility,erosionVisibility);
            [branch] if(detailVisibility>0.001){
                // 基本形状とは別のメートル基準領域を使い、雲塊と細部に同じ模様を出さない。
                float2 detailXz=p.xz-cloudWindWorld()+macro.curl*35.0;
                float3 detailDomainA,detailDomainB;
                cloudDetailDomains(detailXz,p.y,detailDomainA,detailDomainB);
                float2 ndA=detailNoise.SampleLevel(detailNoise_sampler,detailDomainA+float3(0.19,0.67,0.41)+float3(cloudEvolution.z,cloudEvolution.w,-cloudEvolution.z),0);
                float2 ndB=detailNoise.SampleLevel(detailNoise_sampler,detailDomainB+float3(0.73,0.23,0.59)+float3(-cloudEvolution.w,cloudEvolution.z,cloudEvolution.w),0);
                // 同分布の差なので、領域全体を一方向へ膨張させず、動く房と谷を同時に作る。
                float billowOffset=cloudBillowOffset(ndA,ndB,h,erosionVisibility);
                float billowedBaseDensity=remapc(
                    cloudWeatheredBaseNoise(
                        macro.baseNoise+billowOffset,weatherMask),
                    heightThreshold,
                    min(heightThreshold+0.22,0.98),0.0,1.0);
                float billowedCoarseDensity=saturate(cloudDimensionalDensity(billowedBaseDensity,macro.heightProfile)*densityScale);
                float billowedDensity=lerp(
                    coarseDensity,billowedCoarseDensity,billowVisibility);
                float detailNear=ndA.g*0.62+ndB.g*0.38;
                float detailFar=ndA.r*0.62+ndB.r*0.38;
                float detail=lerp(
                    detailFar,detailNear,0.90*erosionVisibility);
                float erosion=lerp(0.10,0.24,smoothstep(0.18,0.92,h));
                float eroded=remapc(
                    billowedDensity,detail*erosion,1.0,0.0,1.0);
                d=lerp(billowedDensity,eroded,erosionVisibility);
            }
            densityResult=saturate(d);
        }
    }
    return densityResult;
}
float cloudDensityFromMacro(float3 p,CloudMacroSample macro,float heightThreshold,float weatherMask,float billowVisibility,float erosionVisibility){
    float densityResult=0.0;
    if(weatherMask>0.001){
        bool upperBand=macro.upperBand>0.5;
        // 被覆は密度の飽和前に掛け、疎らな上層の占有率を保つ。
        if(upperBand) weatherMask*=cloudUpperTerms.x;
        densityResult=cloudDensityFromPositiveWeatherMacro(p,macro,heightThreshold,weatherMask,billowVisibility,erosionVisibility);
        // 濃さは飽和後に掛け、降水補正で薄い上層が下層相当へ戻ることを防ぐ。
        if(upperBand) densityResult*=cloudUpperTerms.y;
    }
    return densityResult;
}
// 高次散乱の周囲媒質判定用の低 LOD 密度。空間スキップ用の広い占有しきい値ではなく、
// 詳細密度と同じ coverage・高さ threshold・上層倍率を使う。追加 texture fetch は無い。
float cloudLowLodDensityFromMacro(CloudMacroSample macro,float heightThreshold,float weatherMask){
    float densityResult=0.0;
    if(weatherMask>0.001){
        bool upperBand=macro.upperBand>0.5;
        // 詳細密度と同じ順序で被覆を飽和前へ適用する。
        if(upperBand) weatherMask*=cloudUpperTerms.x;
        densityResult=cloudLowLodDensityFromPositiveWeatherMacro(macro,heightThreshold,weatherMask);
        // 上層の濃さは飽和後へ適用し、光路と視線で同じ密度になるようにする。
        if(upperBand) densityResult*=cloudUpperTerms.y;
    }
    return densityResult;
}
float cloudDensity(float3 p, float coverage, float detailWeight){
    float densityResult=0.0;
    CloudMacroSample macro=sampleCloudMacroLighting(p,coverage,0.0);
    densityResult=cloudDensityFromMacro(p,macro,macro.heightThreshold,macro.weatherMask,detailWeight,detailWeight);
    return densityResult;
}

// 移流を除いた安定XZ座標と正規化高度から、現在の曲面雲層上の点を復元する。
// 有理化した沈み量により地球半径同士の減算を避け、誤差を1画素未満に抑える。
float3 cloudShadowWorldPosition(float3 uvw){
    float2 q=shadowGrid.xy
             +float2(uvw.x/max(shadowGrid.z,1e-8),
                     uvw.z/max(shadowGrid.w,1e-8));
    float2 worldXz=q+cloudWindWorld();
    float altitude=lerp(layer.x,layer.y,saturate(uvw.y));
    float2 d=worldXz-worldOrigin.xz;
    float radius=CLOUD_PLANET_RADIUS+altitude;
    float d2=dot(d,d);
    float root=sqrt(max(radius*radius-d2,0.0));
    float sag=d2/max(radius+root,1.0);
    return float3(worldXz.x,worldOrigin.y+altitude-sag,worldXz.y);
}

// キャッシュの各画素は、採取間隔に合わせて侵食帯域を制限した近距離3点の後へ対応する。
// l=3..7だけを積分し、48 m以下で採取できる細部は視線側の近距離採取へ残す。
float traceCloudShadowPattern(
    float3 lp,float patternJitter,float coverage,
    float3 sun,float3 lightTangent,float3 lightBitangent){
    float lightStep=0.0075/max(layer.w,1e-4);
    lightStep*=lerp(0.72,1.28,patternJitter);
    // 正確な経路と同じ丸め結果にするため、三回の乗算を別の定数へまとめない。
    lightStep*=1.8;
    lightStep*=1.8;
    lightStep*=1.8;
    float lightDepth=0.0;
    [loop] for(int l=3;l<8;l++){
        float2 coneGeometry=CLOUD_CONE_GEOMETRY[l];
        float conePhi=6.2831853*frac(
            patternJitter+float(l)*0.61803398875);
        float coneSin,coneCos;
        sincos(conePhi,coneSin,coneCos);
        float3 coneDir=cloudConeDirection(
            sun,lightTangent,lightBitangent,
            coneSin,coneCos,coneGeometry);
        // 区間終端ではなく中央で密度を評価し、同じ採取数で一次の密度変化を正しく積分する。
        float3 lightHalfStep=coneDir*(0.5*lightStep);
        lp+=lightHalfStep;
        CloudMacroSample lightMacro=
            sampleCloudMacroLighting(lp,coverage,lightStep);
        float lightDensity=cloudLowLodDensityFromMacro(
            lightMacro,lightMacro.heightThreshold,lightMacro.weatherMask);
        lightDepth+=lightDensity*lightStep
                   *cloudOpticalDepthScaleFromBand(
                       lightMacro.upperBand>0.5);
        lp+=lightHalfStep;
        lightStep*=1.8;
    }
    return lightDepth;
}

// 利用可否、光学的深さ、混合率を、初期化済みの一つの値として返す。
// FXCが早期棄却経路を未初期化と誤判定することを避ける。
float3 sampleCloudShadowTail(float3 lp,float density){
    // 全ての棄却経路で一つの初期値を保ち、D3D12上の孤立した明暗画素を防ぐ。
    float3 result=float3(0.0,0.0,0.0);
    if(shadowState.x>0.5){
        float2 q=lp.xz-cloudWindWorld();
        float altitude=cloudAltitude(lp);
        float h=(altitude-layer.x)/max(layer.y-layer.x,1e-4);
        float3 uvw=float3(
            (q.x-shadowGrid.x)*shadowGrid.z,
            h,
            (q.y-shadowGrid.y)*shadowGrid.w);
        // 範囲外の位置を無関係な端の画素へ固定しない。線形補間が境界をまたがないよう、
        // 一回採取の経路にも1.5画素分の余白を残す。
        float3 texel=float3(shadowState.z,shadowState.w,shadowState.z);
        float3 edgeCells=min(uvw,1.0-uvw)/texel;
        float minimumEdgeCells=min(
            min(edgeCells.x,edgeCells.y),edgeCells.z);
        if(minimumEdgeCells>1.5){
            float borderWeight=smoothstep(1.5,2.5,minimumEdgeCells);
            float2 cached=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,uvw,0);
            bool finiteValue=all(cached==cached)
                          && all(cached>=0.0)
                          && all(cached<65504.0);
            if(finiteValue){
                // yは二つの採取模様の差であり、大きい場所は正確な遠距離積分へ戻す。
                float tauDisagreement=
                    cached.y*density*cloudLightingExtinction.y;
                if(tauDisagreement<=shadowState.y){
                    float confidenceWeight=1.0-smoothstep(
                        shadowState.y*0.60,
                        shadowState.y,tauDisagreement);
                    float cacheWeight=borderWeight*confidenceWeight;
                    if(cacheWeight>0.0){
                        result=float3(1.0,cached.x,cacheWeight);
                    }
                }
            }
        }
    }
    return result;
}

[numthreads(4,4,4)]
void CSCloudShadow(uint3 tid : SV_DispatchThreadID){
    uint updateStride=max((uint)cloudShadowUpdate.z,1u);
    uint3 outputVoxel=uint3(tid.x*updateStride+(uint)cloudShadowUpdate.x,tid.y,tid.z*updateStride+(uint)cloudShadowUpdate.y);
    uint width,height,depth;
    cloudShadowOut.GetDimensions(width,height,depth);
    if(any(outputVoxel>=uint3(width,height,depth))) return;
    float3 uvw=(float3(outputVoxel)+0.5)/float3(width,height,depth);
    float3 p=cloudShadowWorldPosition(uvw);
    float3 sun=sunDir.xyz;
    float3 lightTangent=cloudLightTangent.xyz;
    float3 lightBitangent=cloudLightBitangent.xyz;
    float coverage=saturate(params.x);
    float depthA=traceCloudShadowPattern(
        p,0.211324865,coverage,sun,lightTangent,lightBitangent);
    float depthB=traceCloudShadowPattern(
        p,0.788675135,coverage,sun,lightTangent,lightBitangent);
    float meanDepth=max(0.5*(depthA+depthB),0.0);
    float disagreement=abs(depthA-depthB);
    // 現在のRHIはt0..t3の連続配置を要求する。実行時に成立しない負の密度分岐へ置くことで、
    // t2/s2を宣言へ残しつつ通常時のテクスチャ採取を発生させない。
    if(params.y<0.0){
        disagreement+=detailNoise.SampleLevel(
            detailNoise_sampler,float3(0.5,0.5,0.5),0).x;
    }
    cloudShadowOut[outputVoxel]=float2(meanDepth,disagreement);
}

[numthreads(8,8,1)]
void CSCloudWorldShadow(uint3 tid : SV_DispatchThreadID){
    uint updateStride=max((uint)cloudShadowUpdate.z,1u);
    uint2 outputPixel=tid.xy*updateStride+(uint2)cloudShadowUpdate.xy;
    uint width,height;
    cloudOut.GetDimensions(width,height);
    if(any(outputPixel>=uint2(width,height))) return;
    float2 uv=(float2(outputPixel)+0.5)/float2(width,height);
    float2 referenceXz=cloudWorldShadowMap.xy
        +uv/max(cloudWorldShadowMap.z,1e-8);
    float3 rayOrigin=float3(referenceXz.x,cloudWorldShadowMap.w,referenceXz.y);
    float transmittance=1.0;
    float opticalDepth=0.0;
    float3 sun=sunDir.xyz;
    float enter=0.0,exit=0.0;
    if(sun.y>0.03&&intersectCloudShellFromPosition(rayOrigin,sun,enter,exit)){
        const int SAMPLE_COUNT=32;
        float stepLength=(exit-enter)/float(SAMPLE_COUNT);
        float sampleDistance=enter+0.5*stepLength;
        [loop] for(int sampleIndex=0;sampleIndex<SAMPLE_COUNT;++sampleIndex){
            float3 p=rayOrigin+sun*sampleDistance;
            CloudMacroSample macro=sampleCloudMacroLighting(
                p,saturate(params.x),stepLength);
            float sampleDensity=cloudLowLodDensityFromMacro(
                macro,macro.heightThreshold,macro.weatherMask)
                *max(params.y,0.0);
            opticalDepth+=sampleDensity*stepLength
                         *cloudOpticalDepthScaleFromBand(
                             macro.upperBand>0.5);
            if(opticalDepth*cloudLightingExtinction.y>=12.0) break;
            sampleDistance+=stepLength;
        }
        // 登録番号をt0..t3で連続させるため、通常は到達しない分岐でも詳細雑音を宣言へ残す。
        if(params.y<0.0){
            opticalDepth+=detailNoise.SampleLevel(detailNoise_sampler,float3(0.5,0.5,0.5),0).x;
        }
        transmittance=exp(-max(opticalDepth,0.0)*max(cloudLightingExtinction.y,0.0));
    }
    cloudOut[outputPixel]=float4(saturate(transmittance),max(opticalDepth,0.0),0.0,1.0);
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

uint CloudJitterHash2D(uint2 pixel) {
    // One PCG RXS-M-XS avalanche turns the two integer pixel coordinates into
    // a decorrelated 32-bit value. Unlike interleaved-gradient noise this has
    // no screen-space diagonal dot lattice when held fixed by Ultra TSR.
    uint state=pixel.x*747796405u+pixel.y*2891336453u+277803737u;
    uint word=((state>>((state>>28u)+4u))^state)*277803737u;
    return (word>>22u)^word;
}
float CloudJitter01(uint2 pixel) {
    // Converting the high 24 bits is exact in float and stays strictly below 1.
    return float(CloudJitterHash2D(pixel)>>8u)*(1.0/16777216.0);
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
    float3 localUp=groundHorizon.xyz;
    float signedElevation=dot(dir,localUp);
    // The planet/ground occlusion boundary has no rasterizer coverage because
    // clouds are traced in compute. A hard angular compare therefore exposes
    // one quarter-resolution occupancy decision as a white, stair-stepped row
    // against the lower sky hemisphere. Integrate the projected full-resolution
    // pixel footprint around the same physical cutoff; this is analytic edge
    // coverage, not a post blur, and remains stable across TSR phases.
    float groundHorizonCoverage=1.0;
    float groundCutoff=groundHorizon.w;
    if(groundCutoff>=-1.0) {
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
    if(groundHorizonCoverage<=0.001){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    float coverage=saturate(params.x), density=max(params.y,0.05);
    float t0=0.0,t1=0.0;
    if(!intersectCloudShell(dir,t0,t1)){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // どこまで追うか。遠い雲は 1 画素に何 km も入るので積分が成立せず、描くほど
    // «ちらつく細かいゴミ» になる。打ち切りの手前で薄くして、境界の «壁» を出さない。
    float MAX_DISTANCE=cloudRange.x;
    t1=min(t1,MAX_DISTANCE);
    // 曲面雲層には有限な地平線区間がある。距離減衰はこの入口だけでレイ全体を
    // 棄却せず、後続の各密度標本へ適用する。
    if(t1<=t0){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // Keep samples in world-distance space.  The previous fixed-count
    // (t1-t0)/N step sampled identical height fractions in every pixel and
    // produced the view-centred starburst visible in the editor.
    // 刻み数。参照描画では大きくする (cloudLightingAmbient.z に入っている)。
    int MAX_STEPS=(int)cloudLightingAmbient.z;
    if(MAX_STEPS<32) MAX_STEPS=32;
    float span=t1-t0;
    float baseFineStep=cloudCoverageReciprocals.z;
    // 採取上限の 1/8 は空領域から細密領域へ戻る処理に残し、残りを実積分へ使う。
    // 上限を下げた場合も区間終端へ到達し、参照描画では増やした採取回数が刻み幅へ反映される。
    int fineSampleBudget=MAX_STEPS-(MAX_STEPS>>3);
    int coarseSampleBudget=max(fineSampleBudget>>1,1);
    float fineStep=max(baseFineStep,span/float(fineSampleBudget));
    float coarseStep=max(fineStep*2.0,span/float(coarseSampleBudget));
    // 遠くから始まるレイほど刻みを広げる。地平線へ向かうレイは 1 画素の担当する
    // 立体角が広く、細かく刻んでも結果に出ない。上向きのレイは t0 が小さいので
    // ここでは粗くならない。レイごとに一定の倍率にして、粗密の往復を乱さない。
    float distanceLod=1.0+cloudRange.z*saturate(t0/max(MAX_DISTANCE,1.0));
    fineStep*=distanceLod;
    coarseStep*=distanceLod;
    // 参照描画は時間履歴を使わないため、乱数位相を毎フレーム変えると未平均の粒状誤差だけが動く。
    // 区間中央を使う決定論的な積分にし、1 フレームだけで密度場と照明を比較できる基準画像にする。
    float jit=0.5;
    if(cloudLightingAmbient.w<0.5){
        // 超高品質の時間再構成では、同じ出力画素を 16 段階ごとに更新する。
        // その周期に乱数列を合わせ、通常の等倍・縮小描画では毎フレーム進めて履歴へ平均する。
        uint jitterFrame=(uint)temporal.z;
        uint jitterSequence=temporal.w>3.5?(jitterFrame>>4u):jitterFrame;
        uint2 jitterPixel=rayPixel+uint2((jitterSequence*47u)%131u,(jitterSequence*17u)%127u);
        float pixelJitter=CloudJitter01(jitterPixel);
        jit=frac(pixelJitter+float(jitterSequence)*0.754877666);
    }
    float3 sun=sunDir.xyz;
    float cosA=clamp(dot(dir,sun),-1.0,1.0);
    float phaseBlend=cloudLightingPhase.z;
    // sunCol はPBRと同じ放射照度で、白い拡散面は E/PI になる。HGは既に1/(4PI)を
    // 含むため4倍だけして等方散乱をE/PIへ合わせる。4PI倍すると雲だけがPI倍明るくなる。
    float phase=4.0*(hg(cosA,cloudLightingPhase.x)*phaseBlend
               +hg(cosA,cloudLightingPhase.y)*(1.0-phaseBlend));
    phase=clamp(phase,cloudLightingMulti.y,cloudLightingMulti.z);
    float3 lightTangent=cloudLightTangent.xyz;
    float3 lightBitangent=cloudLightBitangent.xyz;
    float4 coverageTerms=cloudCoverage;
    float transmit=1.0; float3 scatter=float3(0,0,0);
    float depthMoment=0.0;
    float finePhaseOffset=jit*fineStep;
    float t=t0+jit*coarseStep;
    bool nearDensity=false;
    float refineUntil=t0;
    [loop] for(int i=0;i<MAX_STEPS;i++){
        float sampleT=t;
        float stepLength=fineStep;
        if(nearDensity){
            // 乱数位相付きの位置から担当区間の始点を戻し、末尾の端数区間も全長を積分する。
            // 端数区間では同じ位相を区間内へ縮め、標本が雲層の外へ出ないようにする。
            float fineCellStart=max(t-finePhaseOffset,t0);
            if(fineCellStart>=t1) break;
            stepLength=min(fineStep,t1-fineCellStart);
            float intervalPhase=cloudRayIntervalPhase(jit,i);
            sampleT=fineCellStart+intervalPhase*stepLength;
        }else if(t>=t1){
            break;
        }
        // カメラ位置を含む完全な標本位置はワールド座標である。ここでカメラの高さを
        // 再び引くと、Editor のカメラ移動に合わせて雲層まで動いてしまう。
        float3 p=camPos.xyz+dir*sampleT;
        float3 viewMacroUvw;
        float densityHeightThreshold;
        CloudMacroSample macro=sampleCloudMacro(
            p,coverageTerms,fineStep,viewMacroUvw,densityHeightThreshold);
        float shape=cloudShapeFromMacro(macro);
        if(shape<=0.006){
            if(nearDensity && t<refineUntil) {
                t+=fineStep;
            } else {
                t+=coarseStep;
                nearDensity=false;
            }
            continue;
        }
        if(!nearDensity && coarseStep>fineStep*1.5){
            // 粗い採取点で密度を見つけたため、一つ前の区間へ戻して薄い雲縁も細かく積分する。
            // 粗い刻みの位相を流用せず、細密刻み内の同じ乱数位相へ置き直す。
            float coarseProbeT=t;
            refineUntil=coarseProbeT+coarseStep;
            t=cloudRefinedSampleT(t0,coarseProbeT,fineStep,coarseStep,jit);
            nearDensity=true;
            continue;
        }
        nearDensity=true;
        refineUntil=max(refineUntil,t+coarseStep);
        // レイの刻み幅から採取可能な房と侵食の帯域を別々に求める。最後の短い区間で細部が再出現しないよう、
        // 実際の区間長ではなくレイ全体で一定の fineStep を使う。
        float billowVisibility=cloudBillowVisibilityFromSampleSpacing(fineStep);
        float erosionVisibility=cloudErosionVisibilityFromSampleSpacing(fineStep);
        float viewWeatherMask=macro.densityWeatherMask;
        float distanceFade=cloudDistanceFade(sampleT,cloudRange.y,MAX_DISTANCE);
        float dens=cloudDensityFromMacro(p,macro,densityHeightThreshold,viewWeatherMask,billowVisibility,erosionVisibility)*density*distanceFade;
        if(dens>0.0015){
            bool sampleUpperBand=macro.upperBand>0.5;
            float sampleOpticalDepthScale=
                cloudOpticalDepthScaleFromBand(sampleUpperBand);
            // 指数的に間隔を広げる採取点で雲層全体を覆い、2 番目の Beer 項で
            // 高次の散乱を近似する。
            // 同じ視線内では黄金比の列で光採取の位相を巡回し、疎な積分誤差を層として揃えない。
            float lightJitter=frac(jit+float(i)*0.61803398875);
            float coneSin,coneCos;
            sincos(6.2831853*lightJitter,coneSin,coneCos);
            float lightDepth=0.0;
            // 近距離3点だけの光路密度。詳細体積が作る局所自己影を高次散乱へ残す。
            float detailedLightDepth=0.0;
            float lightStep=cloudLightStepFromBand(sampleUpperBand);
            lightStep*=lerp(0.72,1.28,lightJitter);
            float3 lp=p;
            float cachedTailForBlend=0.0;
            float cacheBlendWeight=0.0;
            float exactFarStart=0.0;
            bool blendCachedTail=false;
            // この短い光円すい内では低周波の天候と渦がほぼ変わらないため、視線採取の値を共有する。
            // 密度分布、基本形状、近距離の細部は各採取点で評価する。
            // 雲種、降水量、柱の高さ変形量を 1 つにまとめ、すべての光採取で
            // 視線密度と同じ縦形状を使う。
            float4 sharedLightProfileTerms=float4(cloudProfileTypeWeights(macro.weather.g),macro.weather.b,cloudColumnHeightShift(macro.weather,macro.densityWeatherMask));
            float2 sharedLightCurl=macro.curl;
            float sharedShapeScale=cloudShapeScale();
            // 8 個の光円すいは一定の黄金角で回し、上で求めた sin/cos を漸化式で再利用する。
            bool lightTerminated=false;
            // 近距離3点は実際の採取間隔で侵食帯域を減らす。既定層では4点目が最小でも
            // 48 mを越えるため、後半5点へ進む前に侵食用の一時値を破棄する。
            [loop] for(int l=0;l<3;l++){
                float2 coneGeometry=CLOUD_CONE_GEOMETRY[l];
                float3 coneDir=cloudConeDirection(
                    sun,lightTangent,lightBitangent,
                    coneSin,coneCos,coneGeometry);
                float3 lightHalfStep=coneDir*(0.5*lightStep);
                lp+=lightHalfStep;
                CloudMacroSample lightMacro=sampleCloudMacroLightingFromSlowFields(lp,viewWeatherMask,coverageTerms.w,sharedLightProfileTerms,sharedLightCurl,p,viewMacroUvw,macro.height,sharedShapeScale,lightStep);
                float lightBillowVisibility=cloudBillowVisibilityFromSampleSpacing(lightStep);
                float lightErosionVisibility=cloudErosionVisibilityFromSampleSpacing(lightStep);
                float lightDensity=cloudDensityFromMacro(lp,lightMacro,lightMacro.heightThreshold,lightMacro.weatherMask,lightBillowVisibility,lightErosionVisibility);
                lightDepth+=lightDensity*lightStep
                           *cloudOpticalDepthScaleFromBand(
                               lightMacro.upperBand>0.5);
                // 次区間と影キャッシュは従来と同じ区間終端から始める。
                lp+=lightHalfStep;
                // 直接光と多重散乱の近似値が知覚できない水準まで下がった後は、残りを省略する。
                if(lightDepth*density*cloudLightingExtinction.y>18.0){
                    lightTerminated=true;
                    break;
                }
                float previousConeCos=coneCos;
                coneCos=previousConeCos*(-0.737368878)
                       -coneSin*(-0.675490294);
                coneSin=coneSin*(-0.737368878)
                       +previousConeCos*(-0.675490294);
                lightStep*=1.8;
            }
            detailedLightDepth=lightDepth;
            bool cachedFarTail=false;
            if(!lightTerminated && CLOUD_MAIN_SHADOW_CACHE_ENABLED){
                float3 cachedTailSample=sampleCloudShadowTail(lp,density);
                if(cachedTailSample.x>0.5){
                    float cachedTail=cachedTailSample.y;
                    float cacheWeight=cachedTailSample.z;
                    if(cacheWeight>=0.999){
                        lightDepth+=cachedTail;
                        cachedFarTail=true;
                    }else{
                        // 境界と信頼度の遷移部分だけ正確な経路も計算し、内側ではキャッシュだけを使う。
                        cachedTailForBlend=cachedTail;
                        cacheBlendWeight=cacheWeight;
                        exactFarStart=lightDepth;
                        blendCachedTail=true;
                    }
                }
            }
            if(!lightTerminated && !cachedFarTail){
                // 既定の2500 m層で最大約2.05 kmに及ぶため、各地点の天候、高さ、
                // 基本形状を再評価する。視線標本の天候を流用すると、雲縁を跨いでも
                // 同じ被覆が続き、退避経路だけ自己影が板状になる。
                [loop] for(int l=3;l<8;l++){
                    float2 coneGeometry=CLOUD_CONE_GEOMETRY[l];
                    float3 coneDir=cloudConeDirection(
                        sun,lightTangent,lightBitangent,
                        coneSin,coneCos,coneGeometry);
                    float3 lightHalfStep=coneDir*(0.5*lightStep);
                    lp+=lightHalfStep;
                    float2 farLightSample=sampleCloudFarLightingDensityAndScale(
                        lp,coverage,sharedLightCurl,lightStep);
                    lightDepth+=farLightSample.x*lightStep*farLightSample.y;
                    lp+=lightHalfStep;
                    if(lightDepth*density*cloudLightingExtinction.y>18.0) break;
                    float previousConeCos=coneCos;
                    coneCos=previousConeCos*(-0.737368878)
                           -coneSin*(-0.675490294);
                    coneSin=coneSin*(-0.737368878)
                           +previousConeCos*(-0.675490294);
                    lightStep*=1.8;
                }
            }
            if(blendCachedTail && !lightTerminated){
                float exactTail=max(lightDepth-exactFarStart,0.0);
                lightDepth=exactFarStart+lerp(
                    exactTail,cachedTailForBlend,cacheBlendWeight);
            }
            float tauL=lightDepth*density*cloudLightingExtinction.y;
            float beer=exp(-tauL);
            float multiContribution=cloudLightingPhase.w;
            float multiOcclusion=cloudLightingMulti.x;
            // 遠距離では次数ごとに消散を弱めて光の回り込みを保つ。一方、詳細を採取した
            // 近距離まで同じ割合で弱めると、房の自己影が高次散乱だけで埋まる。
            // 近距離は一段前の縮小率を使い、追加採取なしで局所形状だけを保持する。
            float detailedTauL=min(
                detailedLightDepth*density*cloudLightingExtinction.y,tauL);
            float farTauL=max(tauL-detailedTauL,0.0);
            float secondDetailedOcclusion=sqrt(saturate(multiOcclusion));
            // 何度も散乱した光は向きを失うので、単散乱より等方に近い位相を使う。
            // 同じ位相を使うと、内部で回った光まで太陽方向へ偏って雲が薄く見える。
            float phaseMulti=4.0*hg(cosA,cloudMultiPhase.x);
            phaseMulti=clamp(
                phaseMulti,cloudLightingMulti.y,cloudLightingMulti.z);
            // 一次散乱と、係数を次数ごとに縮小した二次・三次散乱を独立に評価する。
            // CPU は散乱係数の縮小率を消散係数以下へ収め、各次数で散乱が消散を越えないようにする。
            // 一次散乱は現在の密度標本と区間不透明度で既に制限される。高次散乱は周囲の
            // 散乱源を必要とするため、低 LOD 密度と高さから求める確率をこちらだけへ掛ける。
            float lowLodDensity=cloudLowLodDensityFromMacro(
                macro,densityHeightThreshold,viewWeatherMask);
            float inScatterDepthExponent=lerp(
                0.5,2.0,saturate((macro.height-0.30)/0.55));
            float inScatterDepth=saturate(
                0.05+pow(saturate(lowLodDensity),inScatterDepthExponent));
            // 雲底の直上にも厚い雲体があるため、高さだけで高次散乱を抑えない。
            // 高さは密度指数に使い、疎な雲頂縁だけを強く抑える。
            float inScatterProbability=inScatterDepth;
            float inScatterFactor=lerp(
                1.0,inScatterProbability,cloudLightingExtinction.w);
            float singleScatter=beer*phase;
            float secondScatter=multiContribution
                *exp(-(detailedTauL*secondDetailedOcclusion
                      +farTauL*multiOcclusion))*phaseMulti;
            float thirdContribution=multiContribution*multiContribution;
            float thirdOcclusion=multiOcclusion*multiOcclusion;
            float thirdScatter=thirdContribution
                *exp(-(detailedTauL*multiOcclusion
                      +farTauL*thirdOcclusion))*phaseMulti;
            float multipleScatter=(secondScatter+thirdScatter)
                                 *inScatterFactor;
            float scatterTerm=singleScatter+multipleScatter;
            // 区間不透明度 a は既に消散係数を含むため、太陽光には消散に対する散乱の割合を掛ける。
            // ここを見た目調整用に暗くすると、水滴雲が光を吸収する灰色の媒質になってしまう。
            // 太陽付近の強い前方散乱は位相上限と後段の露出で制御する。
            // 太陽光は雲へ届く前に大気を通る。低い太陽ほど青が削られて赤くなる。
            float3 sunAtCloud=sunCol.rgb*cloudSunTransmittance.rgb;
            float3 sunL=sunAtCloud*cloudLightingExtinction.z*scatterTerm;
            float h=macro.height;
            // 環境光の遮蔽はカメラまでの透過率ではなく、局所密度と入射側の層境界までの距離で近似する。
            // 太陽方向の tauL は細い光円すいの経路であり、空半球や地面から届く拡散光の経路ではない。
            // ここへ流用すると、太陽が高いほど全天空光まで直接影と同じ形で失われ、厚い雲が一様な灰色へ沈む。
            float ambientLocalDensity=saturate(lowLodDensity*density);
            // 空半球の環境光は大気と雲で既に何度も散乱して方向を失っているため、
            // 二次散乱用の遮蔽率をそのまま掛けると、公開された雲底係数をさらに半分以下へ落としてしまう。
            // 三次散乱と同じ縮小率で拡散輸送を近似し、追加採取なしで雲頂と雲底の明暗差を保つ。
            float diffuseOcclusion=multiOcclusion*multiOcclusion;
            float reducedAmbientExtinction=0.60*diffuseOcclusion*cloudLightingExtinction.y;
            float skyAmbientOpticalDepth=ambientLocalDensity*(0.35+0.65*(1.0-h));
            float groundAmbientOpticalDepth=ambientLocalDensity*(0.35+0.65*h);
            float skyAmbientVisibility=exp(-reducedAmbientExtinction*skyAmbientOpticalDepth);
            float groundAmbientVisibility=exp(-reducedAmbientExtinction*groundAmbientOpticalDepth);
            // 地平色と天頂色を半球積分の二点近似として混ぜる。雲底でも天頂側の空を1/3含め、
            // 雲頂でも地平側を1/3残すことで、一点の灰色や青へ偏らせない。
            float skyAmbientZenithWeight=lerp(0.3333333,0.6666667,saturate(h));
            float3 skyAmbient=cloudSkyZenith.w>0.5
                             ?lerp(skyCol.rgb,cloudSkyZenith.rgb,skyAmbientZenithWeight)
                             :skyCol.rgb;
            float3 ambL=skyAmbient
                       *lerp(cloudLightingAmbient.x,cloudLightingAmbient.y,h)
                       *skyAmbientVisibility;
            // 地面からの照り返し。雲底ほど強く受ける。0 なら足さない。
            float bottomWeight=1.0-smoothstep(0.15,0.65,h);
            float3 groundL=cloudLightingGround.rgb*cloudLightingMulti.w
                          *bottomWeight*groundAmbientVisibility;
            float a=1.0-exp(
                -dens*stepLength*sampleOpticalDepthScale
                *cloudLightingExtinction.x);
            float sampleWeight=transmit*a;
            scatter += sampleWeight*(sunL+ambL+groundL);
            depthMoment += sampleWeight*sampleT;
            transmit *= (1.0-a);
            if(transmit<0.012) break;
        }
        t+=fineStep;
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
    float4 layer;      // x=world base Y, y=world top Y, z=XZ noise scale, w=world→canonical Y
    float4 worldOrigin;// xyz=rebased shell origin, w=history camera stationary
    float4 shadowGrid;
    float4 shadowState;
    float4 groundHorizon;// xyz=camera local up, w=ground tangent elevation; <-1 disables
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
// 現在レイと同じ出力画素の履歴差を、4x4領域で共有できる変化率へ写す。
// ごく小さい採取差は履歴へ任せ、雲縁の移動は15個の未採取画素にも伝える。
float CloudTemporalBlockResponse(float currentAlpha,float historyAlpha) {
    return smoothstep(0.015,0.12,abs(currentAlpha-historyAlpha));
}
// 静止画素では小さな採取誤差を長く平均し、被覆が明確に変わった時も孤立画素を作らない重みに抑える。
float CloudTemporalCurrentWeight(float currentAlpha,float historyAlpha,bool stationary) {
    float currentWeight=0.70;
    if(stationary) {
        float changeResponse=CloudTemporalBlockResponse(currentAlpha,historyAlpha);
        currentWeight=lerp(0.10,0.42,changeResponse);
    }
    return currentWeight;
}
// 未採取画素は変化中だけ現在の両側再構成へ寄せ、安定後は蓄積済みの等倍標本を保つ。
float CloudTemporalUnscheduledWeight(float blockResponse,bool stationary) {
    return stationary?0.20*saturate(blockResponse):0.0;
}
bool IsScheduledFullPixel(uint2 pixel,uint2 phaseOffset) {
    return ((pixel.x&3u)==phaseOffset.x) &&
           ((pixel.y&3u)==phaseOffset.y);
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

[numthreads(8,8,1)]
void CSResolve(uint3 tid : SV_DispatchThreadID) {
    uint fullW=(uint)dims.z;
    uint fullH=(uint)dims.w;
    if(tid.x>=fullW || tid.y>=fullH) return;
    float2 uv=(float2(tid.xy)+0.5)/dims.zw;
    bool temporalSuperRes=IsTemporalSuperResolution();
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
    // 低解像度の現在レイが実際に通った等倍画素を履歴側でも読む。
    // 対象画素自身と比較すると静止した雲縁でも4x4内の位置差を変化と誤認するため、同じレイ同士を比べる。
    float blockResponse=0.0;
    if(temporal.x>0.5 && temporalSuperRes) {
        int2 currentTracePixel=clamp(int2(CurrentSamplePixel(nearestQ,phaseIndex,true)+0.5),int2(0,0),int2(dims.zw)-1);
        float4 currentTraceHistory=historyColor.Load(int3(currentTracePixel,0));
        blockResponse=CloudTemporalBlockResponse(refC.a,currentTraceHistory.a);
    }
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
        !scheduled && worldOrigin.w>0.5 && blockResponse<=0.001;
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
                        resolved=float4(
                            stableHist.rgb*stableHist.a,stableHist.a);
                        resolvedDepth=float2(
                            sameScreenDepth.x,stableHist.a);
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
    // A scheduled Ultra pixel is the exact full-resolution ray stored at its
    // 4x4 block coordinate. Unscheduled pixels use this gather only as a
    // first-frame/disocclusion fallback; accepted history is never blurred by
    // the quarter-resolution footprint.
    bool exactCurrent=nativeMarch || scheduled;
    float curA=exactCurrent
        ? saturate(refC.a)
        : saturate(alphaSum/max(weightSum,1e-5));
    float3 curPremul=exactCurrent
        ? refC.rgb*refC.a
        : premulSum/max(weightSum,1e-5);
    float curDepth=exactCurrent
        ? ((refC.a>0.003 && refD.x<=250000.0)?refD.x:250001.0)
        : (depthWeight>1e-5 ? depthSum/depthWeight : 250001.0);
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
                bool occupancyMismatch=
                    (curA<0.02 && hist.a>0.08) ||
                    (curA>0.08 && hist.a<0.02);
                bool alphaOk=!occupancyMismatch &&
                    abs(hist.a-seedDepth.y)<0.42;
                if(depthOk && alphaOk) {
                    float4 histPacked=float4(hist.rgb*hist.a,hist.a);
                    if(!temporalSuperRes || scheduled) {
                        float4 currentRange=max(
                            neighborhoodMax-neighborhoodMin,
                            float4(0.015,0.015,0.015,0.025));
                        neighborhoodMin-=currentRange*0.35;
                        neighborhoodMax+=currentRange*0.35;
                        histPacked=clamp(
                            histPacked,neighborhoodMin,neighborhoodMax);
                    }
                    float edgeConfidence=saturate(abs(refC.a-0.5)*2.0);
                    if(temporalSuperRes) {
                        if(scheduled) {
                            // Each exact subpixel now advances a low-discrepancy
                            // sample once per sixteen-phase cycle. Accumulate a
                            // wider stationary window so stochastic lighting and
                            // shell-edge errors converge instead of becoming
                            // static stipple; keep motion responsive.
                            float currentWeight=CloudTemporalCurrentWeight(
                                curA,hist.a,worldOrigin.w>0.5);
                            resolved=lerp(
                                histPacked,current,currentWeight);
                            resolvedDepth=float2(curDepth,curA);
                        } else {
                            // 同じ現在レイで被覆が変わった場合だけ、4x4内の未採取画素も両側再構成へ少し寄せる。
                            // 変化が収まれば重みは0となり、他の15位相で蓄積した等倍細部を再びそのまま保つ。
                            float unscheduledWeight=CloudTemporalUnscheduledWeight(blockResponse,worldOrigin.w>0.5);
                            resolved=lerp(histPacked,current,unscheduledWeight);
                            resolvedDepth=float2(reprojectionDepth,resolved.a);
                        }
                    } else {
                        float feedback=lerp(
                            0.76,0.91,edgeConfidence);
                        resolved=lerp(current,histPacked,feedback);
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
// 履歴へ混ぜず、表示する全解像度画素で地平線被覆を一度だけ求める。
float CloudGroundCoverage(VSOut v) {
    float result=1.0;
    if(groundHorizon.w>=-1.0) {
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
    if (cloud.a < 0.001 || cloudHit.y < 0.001)
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
// 履歴へ混ぜず、表示する全解像度画素で地平線被覆を一度だけ求める。
float CloudGroundCoverage(VSOut v) {
    float result=1.0;
    if(groundHorizon.w>=-1.0) {
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
    if (cloud.a < 0.001 || cloudHit.y < 0.001)
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
    float slice = sqrt(saturate(cloudHit.x / maxDistance));
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
};
static_assert(sizeof(FCloudCb) == 688, "CloudCB must match the HLSL layout");
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
static_assert(
    CBSize<FCloudCb>() == 768u,
    "CloudCB allocation must preserve DX12's 256-byte alignment");

struct FCloudAtmosphereCb {
    FVec4 atmosphereParams;
};

bool IsFiniteCloudVector(FVec3 value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
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
           lhs.SunScatter == rhs.SunScatter && lhs.PowderStrength == rhs.PowderStrength &&
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
    /** 太陽から直接届く一次散乱。 */
    const f32 singleScattering = Exp(-opticalDepth) * singlePhase;
    /** 二次散乱へ使う散乱係数の縮小率。 */
    const f32 secondContribution = lighting.MultiScatterContribution;
    /** 二次散乱へ使う消散係数の縮小率。 */
    const f32 secondOcclusion = lighting.MultiScatterOcclusion;
    /** 消散を弱めた経路から届く近似二次散乱。 */
    const f32 secondScattering = secondContribution * Exp(-opticalDepth * secondOcclusion) * multiplePhase;
    /** 三次散乱へ使う散乱係数の縮小率。 */
    const f32 thirdContribution = secondContribution * secondContribution;
    /** 三次散乱へ使う消散係数の縮小率。 */
    const f32 thirdOcclusion = secondOcclusion * secondOcclusion;
    /** さらに内部へ回った経路から届く近似三次散乱。 */
    const f32 thirdScattering = thirdContribution * Exp(-opticalDepth * thirdOcclusion) * multiplePhase;
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

} // namespace

FVolumetricCloudFrameWorkload PlanVolumetricCloudFrameWorkload(
    const FVolumetricCloudFrameWorkloadPlan& plan) noexcept {
    FVolumetricCloudFrameWorkload out{};
    out.trace_width = plan.trace_width;
    out.trace_height = plan.trace_height;
    out.output_width = plan.output_width;
    out.output_height = plan.output_height;

    out.trace_logical_invocations =
        CloudLogicalInvocations2D(plan.trace_width, plan.trace_height);
    out.resolve_logical_invocations =
        CloudLogicalInvocations2D(plan.output_width, plan.output_height);
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

    add_bake_3d(plan.bake_shape_noise, 128u, 128u, 128u, 4u, 4u, 4u);
    add_bake_2d(plan.bake_weather, 512u, 512u, 8u, 8u);
    add_bake_3d(plan.bake_detail_noise, 64u, 64u, 64u, 4u, 4u, 4u);
    add_bake_2d(plan.bake_curl_noise, 128u, 128u, 8u, 8u);

    const u32 shadowUpdateDivisor = plan.shadow_update_divisor == kVolumetricCloudShadowTemporalDivisor
                                        ? kVolumetricCloudShadowTemporalDivisor
                                        : 1u;
    const u32 shadowCacheUpdateWidth = CloudCeilDivisor(kVolumetricCloudShadowCacheWidth, shadowUpdateDivisor);
    const u32 shadowCacheUpdateDepth = CloudCeilDivisor(kVolumetricCloudShadowCacheDepth, shadowUpdateDivisor);
    const u32 worldShadowUpdateResolution = CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution, shadowUpdateDivisor);

    if (plan.rebuild_shadow_cache) {
        out.shadow_cache_dispatches = 1u;
        const u64 logical = CloudLogicalInvocations3D(shadowCacheUpdateWidth, kVolumetricCloudShadowCacheHeight, shadowCacheUpdateDepth);
        const u64 launched = CloudLaunchedThreads3D(shadowCacheUpdateWidth, kVolumetricCloudShadowCacheHeight, shadowCacheUpdateDepth, 4u, 4u, 4u);
        out.shadow_cache_logical_invocations = logical;
        out.shadow_cache_launched_threads = launched;
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
    out.maximum_view_samples = SaturatingCloudWorkloadMultiply(out.trace_logical_invocations, maximumViewSteps);
    out.maximum_light_samples = SaturatingCloudWorkloadMultiply(out.maximum_view_samples, kVolumetricCloudMaxLightMarchSamples);
    out.maximum_world_shadow_samples = SaturatingCloudWorkloadMultiply(out.world_shadow_logical_invocations, kVolumetricCloudWorldShadowSamples);
    out.temporal_super_resolution =
        plan.output_width != 0u && plan.output_height != 0u &&
        plan.trace_width == CloudCeilDivisor(
            plan.output_width, kVolumetricCloudUltraTraceDivisor) &&
        plan.trace_height == CloudCeilDivisor(
            plan.output_height, kVolumetricCloudUltraTraceDivisor);
    return out;
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

    const f32 unboundedRadialY =
        kVolumetricCloudPlanetRadius + cameraLocal.y;
    const f32 radialY =
        unboundedRadialY > 1.0f ? unboundedRadialY : 1.0f;
    const f32 radialXz2 =
        cameraLocal.x * cameraLocal.x +
        cameraLocal.z * cameraLocal.z;
    const f32 q = radialXz2 / radialY;
    const f32 cameraAltitude =
        cameraLocal.y + q * (0.5f - q / (8.0f * radialY));
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
    f32 shapeScale = authoredScale * 0.006f;
    if (shapeScale < 0.00012f) shapeScale = 0.00012f;
    if (shapeScale > 0.00045f) shapeScale = 0.00045f;
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

    // 互いに割り切れない周期により、短い時間で同じ形へ戻る反復を避ける。
    out.shape_phase = FVec2{
        periodicSin(0.021) * 0.18f,
        periodicSin(0.013) * 0.16f};
    out.fine_phase = FVec2{
        periodicSin(0.037) * 0.11f,
        periodicSin(0.029) * 0.09f};
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
        out.exit = (centreDot < 0.0 && hitsInner && innerNear > 0.0f)
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

FVolumetricCloudMarchPlan PlanVolumetricCloudRayMarch(FVec3 ray_origin, FVec3 ray_direction, const FVolumetricCloudLayer& layer, f32 max_distance, FVec3 world_origin, u32 maximum_samples) noexcept {
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
    const FVec3 dir{ray_direction.x * invLen, ray_direction.y * invLen,
                    ray_direction.z * invLen};

    const FVolumetricCloudRayInterval interval =
        IntersectVolumetricCloudShell(
            ray_origin, dir, layer, kVolumetricCloudPlanetRadius,
            world_origin);
    if (!interval.hit) return out;

    const FVolumetricCloudRange defaults{};
    max_distance = SanitizeCloudScalar(max_distance, defaults.MaxDistance, kVolumetricCloudMinDistance,
                                       kVolumetricCloudMaxDistance);
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
    const u32 fineSampleBudget = maximum_samples - maximum_samples / 8u;
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

    const FVec3 fromCenter{
        ray_origin.x - world_origin.x,
        ray_origin.y - world_origin.y + kVolumetricCloudPlanetRadius,
        ray_origin.z - world_origin.z};
    const f32 upLen2 = fromCenter.x * fromCenter.x +
                       fromCenter.y * fromCenter.y +
                       fromCenter.z * fromCenter.z;
    const f32 invUpLen = upLen2 > 1e-12f ? 1.0f / Sqrt(upLen2) : 1.0f;
    f32 elevation = (dir.x * fromCenter.x + dir.y * fromCenter.y +
                     dir.z * fromCenter.z) * invUpLen;
    const f32 cameraAltitude = Sqrt(upLen2) -
                               kVolumetricCloudPlanetRadius;
    if (cameraAltitude < layer.base_height && elevation < -0.002f) {
        out.enter = 0.0f;
        out.exit = 0.0f;
        return out;
    }
    out.visibility = EvaluateVolumetricCloudDistanceFade(out.enter, max_distance, defaults.FadeFraction);
    // GPU は各標本を距離で薄めるため、入口係数だけを使って有効区間を捨てない。
    out.hit = true;
    return out;
}

void CVolumetricClouds::InvalidateCloudHistory_Internal(bool density_field_changed) noexcept
{
    m_HistoryValid = false;
    m_WorldShadowValid = false;
    if (density_field_changed) m_ShadowCacheValid = false;
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
    m_Lighting = lighting;
    InvalidateCloudHistory_Internal(false);
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
    InvalidateCloudHistory_Internal(false);
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
        weather.Get(),
        detail.Get(),
        curl.Get(),
        composite_vertex.Get(),
        composite_pixel.Get(),
        composite_atmosphere_pixel.Get(),
        resolve.Get(),
    };
    bool compiling = false;
    for (IRhiShader* shader : mandatory) {
        if (shader == nullptr) return EShaderStatus::Failed;
        const EShaderStatus status = shader->Status();
        if (status == EShaderStatus::Failed) return EShaderStatus::Failed;
        compiling = compiling || status == EShaderStatus::Compiling;
    }

    if (shadow) {
        const EShaderStatus shadow_status = shadow->Status();
        if (shadow_status == EShaderStatus::Failed) {
            return EShaderStatus::Failed;
        }
        compiling = compiling || shadow_status == EShaderStatus::Compiling;
    }
    if (world_shadow) {
        const EShaderStatus world_shadow_status = world_shadow->Status();
        if (world_shadow_status == EShaderStatus::Failed) {
            return EShaderStatus::Failed;
        }
        compiling = compiling ||
            world_shadow_status == EShaderStatus::Compiling;
    }
    return compiling ? EShaderStatus::Compiling : EShaderStatus::Ready;
}

namespace {

TResult<CVolumetricClouds::FCompiledShaders> CreateCloudShaderSet(
    IRhiDevice& device, bool compile_async) noexcept {
    if (compile_async && !device.SupportsAsyncShaderCompilation()) {
        return ACS_ERR(
            Render, 580,
            "Volumetric-cloud backend-managed asynchronous compilation "
            "is unsupported");
    }

    auto compile = [&device, compile_async](
                       EShaderStage stage, const char* source,
                       const char* entry, const char* name) noexcept {
        FShaderDesc desc{};
        desc.stage = stage;
        desc.hlsl_source = source;
        desc.entry_point = entry;
        desc.debug_name = name;
        desc.compile_async = compile_async;
        return CreateRhiShader(device, desc);
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
    auto shader_result = CreateCloudShaderSet(device, false);
    if (shader_result.IsErr()) return Err<void>(shader_result.Error());
    return InitWithCompiledShaders(
        device, Move(shader_result.Value()), hdr_format);
}

TResult<CVolumetricClouds::FCompiledShaders>
CVolumetricClouds::BeginCompileShadersAsync(IRhiDevice& device) noexcept {
    return CreateCloudShaderSet(device, true);
}

TResult<void> CVolumetricClouds::InitWithCompiledShaders(
    IRhiDevice& device, FCompiledShaders&& shaders,
    EFormat hdr_format) noexcept {
    if (!shaders.cloud || !shaders.noise || !shaders.weather ||
        !shaders.detail || !shaders.curl || !shaders.composite_vertex ||
        !shaders.composite_pixel ||
        !shaders.composite_atmosphere_pixel || !shaders.resolve) {
        return ACS_ERR(
            Render, 580,
            "Volumetric-cloud compiled shader set is incomplete");
    }

    CVolumetricClouds candidate;
    candidate.m_Layer = m_Layer;
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
        pd.srv_slots = 5;
        pd.srv_names[0] = "shapeNoise";
        pd.srv_names[1] = "weatherMap";
        pd.srv_names[2] = "detailNoise";
        pd.srv_names[3] = "curlNoise";
        pd.srv_names[4] = "cloudShadowCache";
        pd.static_sampler_count = 5;
        for (u32 i = 0; i < 4; ++i) {
            pd.static_samplers[i].filter = ESamplerFilter::Linear;
            pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
            pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
            pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
        }
        pd.static_samplers[4].filter = ESamplerFilter::Linear;
        pd.static_samplers[4].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[4].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[4].address_w = ESamplerAddress::Clamp;
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_CloudPipe = Move(r.Value()); }
    // 任意機能である浅い太陽方向深さを、一つの3次元テクスチャへ直接生成する。
    // 作成に失敗した場合も雲本体は初期化し、正確な遠距離積分へ戻す。
    {
        bool shadowOk = kVolumetricCloudShadowCacheEnabled &&
                        shaders.shadow;
        if (shadowOk) m_ShadowCs = Move(shaders.shadow);
        if (shadowOk) {
            FComputePipelineDesc pd{};
            pd.cs = m_ShadowCs.Get();
            pd.cbuffer_slots = 1;
            pd.cbuffer_names[0] = "CloudCB";
            pd.srv_slots = 4;
            pd.srv_names[0] = "shapeNoise";
            pd.srv_names[1] = "weatherMap";
            pd.srv_names[2] = "detailNoise";
            pd.srv_names[3] = "curlNoise";
            // 現在のRHIでは計算シェーダーの登録番号を連続させる。u0/u1には無害な
            // 代替テクスチャを割り当て、u2だけを3次元キャッシュとして書き込む。
            pd.uav_slots = 3;
            pd.uav_names[0] = "cloudOut";
            pd.uav_names[1] = "cloudDepthOut";
            pd.uav_names[2] = "cloudShadowOut";
            pd.static_sampler_count = 4;
            for (u32 i = 0; i < 4; ++i) {
                pd.static_samplers[i].filter = ESamplerFilter::Linear;
                pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
            }
            auto pipeResult = CreateRhiComputePipeline(device, pd);
            if (pipeResult.IsErr()) {
                shadowOk = false;
            } else {
                m_ShadowPipe = Move(pipeResult.Value());
            }
        }
        if (shadowOk) {
            FTextureDesc td{};
            td.width = kVolumetricCloudShadowCacheWidth;
            td.height = kVolumetricCloudShadowCacheHeight;
            td.depth = kVolumetricCloudShadowCacheDepth;
            td.format = EFormat::R16G16_Float;
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
                    "CVolumetricClouds: optional sun-depth cache unavailable; "
                    "exact lighting fallback remains active");
            }
        }
        m_ShadowCacheAvailable = shadowOk;
        m_ShadowCacheValid = false;
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
            pd.srv_slots = 4;
            pd.srv_names[0] = "shapeNoise";
            pd.srv_names[1] = "weatherMap";
            pd.srv_names[2] = "detailNoise";
            pd.srv_names[3] = "curlNoise";
            pd.uav_slots = 1;
            pd.uav_names[0] = "cloudOut";
            pd.static_sampler_count = 4;
            for (u32 i = 0; i < 4; ++i) {
                pd.static_samplers[i].filter = ESamplerFilter::Linear;
                pd.static_samplers[i].address_u = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_v = ESamplerAddress::Wrap;
                pd.static_samplers[i].address_w = ESamplerAddress::Wrap;
            }
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
        m_WorldShadowDispatchCount = 0u;
    }
    // Perlin-Worley noise 生成 compute。
    // shader 宣言は独立した物理行へ置き、行 comment と結合させない。
    {
        m_NoiseCs = Move(shaders.noise); }
    {   FComputePipelineDesc pd{}; pd.cs = m_NoiseCs.Get();
        pd.uav_slots = 1; pd.uav_names[0] = "noiseOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_NoisePipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 128; td.height = 128; td.depth = 128;
        td.format = EFormat::R16G16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ShapeTex = Move(r.Value()); }
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
    if (!m_Ready) return false;
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
    m_FrameIndex = 0; m_TemporalPhase = 0;
    m_ResolvedIndex = 0; m_HistoryValid = false;
    m_WorldShadowValid = false;
    m_WorldOrigin = FVec3{};
    m_PrevCameraRelativeViewProj = FMat4::Identity();
    m_PrevCameraRelativeInvViewProj = FMat4::Identity();
    m_PrevCamPos = FVec3{};
    m_PrevSunDir = FVec3{};
    m_PrevSunColor = FVec3{};
    m_PrevSkyColor = FVec3{};
    m_PrevWindOffset = 0.0f; m_PrevWindSpeed = 0.0f;
    m_PrevCoverage = -1.0f; m_PrevDensity = -1.0f; m_PrevTime = -1.0f;
    return true;
}

void CVolumetricClouds::RenderCompute(IRhiCommandList& cl, const FMat4& inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color, f32 coverage, f32 density, f32 wind, f32 time) noexcept {
    // 既存の呼び出し元との互換性を保つ。新しい呼び出し元は、精度を保てる
    // ビュー行列から作った camera_relative_inv_view_proj を直接渡す。
    const FMat4 cameraRelativeInverseViewProjection = inv_view_proj * FMat4::Translation(FVec3{-cam_pos.x, -cam_pos.y, -cam_pos.z});
    RenderComputeCameraRelative(cl, cameraRelativeInverseViewProjection, cam_pos, sun_dir, sun_color, sky_color, coverage, density, wind, time);
}

void CVolumetricClouds::RenderComputeCameraRelative(IRhiCommandList& cl, const FMat4& camera_relative_inv_view_proj, FVec3 cam_pos, FVec3 sun_dir, FVec3 sun_color, FVec3 sky_color, f32 coverage, f32 density, f32 wind, f32 time) noexcept {
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
    const FVec3 worldOrigin = RebaseVolumetricCloudWorldOrigin(cam_pos);
    const FVec2 worldShadowCenterReferenceXz = ProjectVolumetricCloudWorldShadowReferenceXZ(cam_pos, safeSun, worldOrigin.y);
    const FVec2 nextWorldShadowMapMinReferenceXz = VolumetricCloudWorldShadowMapMinimum(worldShadowCenterReferenceXz);
    const bool worldShadowMappingChanged =
        nextWorldShadowMapMinReferenceXz.x !=
            m_WorldShadowMapMinReferenceXz.x ||
        nextWorldShadowMapMinReferenceXz.y !=
            m_WorldShadowMapMinReferenceXz.y;
    m_WorldShadowMapMinReferenceXz = nextWorldShadowMapMinReferenceXz;
    m_WorldShadowReferenceHeight = worldOrigin.y;
    m_WorldShadowSunDirection = safeSun;
    m_WorldShadowWorldOrigin = worldOrigin;
    m_WorldShadowCloudBaseAltitude = m_Layer.base_height;
    // 一つのワールド移流距離を天候、形状、侵食、渦、時間再投影で共有する。
    // 雑音の周波数とは分離し、各領域が風によって互いに滑ることを防ぐ。
    const f32 windOffset = safeTime * safeWind * 2.5f;

    const FVolumetricCloudDensityFrameTerms densityFrameTerms =
        ResolveVolumetricCloudDensityFrameTerms(m_Layer, windOffset);
    // 独立領域の相対移動だけを変え、基準領域のワールド固定と風移流は維持する。
    const FVolumetricCloudEvolutionFrameTerms evolutionFrameTerms =
        ResolveVolumetricCloudEvolutionFrameTerms(safeTime, safeWind);
    const FVec2 cameraQ = VolumetricCloudMaterialXZ(cam_pos, windOffset);
    bool shadowGridChanged = false;
    if (!m_ShadowGridInitialized) {
        const auto mapping = CenterVolumetricCloudShadowCache(cameraQ);
        m_ShadowGridMinQ = mapping.min_material_xz;
        m_ShadowGridCenterQ = mapping.center_material_xz;
        m_ShadowGridInitialized = true;
        shadowGridChanged = true;
    } else {
        const f32 dx = cameraQ.x - m_ShadowGridCenterQ.x;
        const f32 dz = cameraQ.y - m_ShadowGridCenterQ.y;
        if (std::fabs(dx) > kVolumetricCloudShadowCacheSafeRadius ||
            std::fabs(dz) > kVolumetricCloudShadowCacheSafeRadius) {
            const auto mapping = CenterVolumetricCloudShadowCache(cameraQ);
            m_ShadowGridMinQ = mapping.min_material_xz;
            m_ShadowGridCenterQ = mapping.center_material_xz;
            shadowGridChanged = true;
        }
    }
    const bool shadowResourcesReady =
        m_ShadowCacheAvailable && m_ShadowCs && m_ShadowPipe && m_ShadowTex;
    if (!shadowResourcesReady) m_ShadowCacheValid = false;
    const bool worldShadowResourcesReady =
        m_WorldShadowAvailable && m_WorldShadowCs &&
        m_WorldShadowPipe && m_WorldShadowTex;

    // history invalidation: resize は EnsureSize で扱う。ここでは camera cut、time jump、
    // 見える品質設定の不連続を拒否する。Reprojection に使える履歴と、
    // expensive march を 2x2 interleave してよい静止状態は別に判定する。
    // These strict deltas only gate the same-screen stationary fast path below.
    // Camera-cut invalidation uses projection-ray angles and a true teleport
    // threshold, so ordinary translated view matrices keep valid history.
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
    bool historyValid = m_HistoryValid;
    if (historyValid) {
        if (VolumetricCloudViewCutDetected(m_PrevCameraRelativeInvViewProj, m_PrevCamPos, camera_relative_inv_view_proj, cam_pos)) {
            historyValid = false;
        }
        // The soft-snapped tangent origin changes continuously only inside a
        // transition band.  Treat it like ordinary camera motion and let the
        // reprojection depth/alpha tests reject individual stale pixels.
        // Invalidating the whole frame for every sub-unit origin change turns
        // those bands into visibly noisy strips during editor pans.
        if (VolumetricCloudLightingChanged(
                m_PrevSunDir, m_PrevSunColor, m_PrevSkyColor,
                safeSun, safeSunColor, safeSkyColor)) {
            historyValid = false;
        }
        f32 coverageDelta = safeCoverage - m_PrevCoverage; if (coverageDelta < 0.0f) coverageDelta = -coverageDelta;
        f32 densityDelta = safeDensity - m_PrevDensity; if (densityDelta < 0.0f) densityDelta = -densityDelta;
        f32 windSpeedDelta = safeWind - m_PrevWindSpeed; if (windSpeedDelta < 0.0f) windSpeedDelta = -windSpeedDelta;
        f32 timeDelta = safeTime - m_PrevTime; if (timeDelta < 0.0f) timeDelta = -timeDelta;
        f32 windDelta = windOffset - m_PrevWindOffset; if (windDelta < 0.0f) windDelta = -windDelta;
        if (coverageDelta > 0.001f || densityDelta > 0.001f ||
            windSpeedDelta > 0.001f || timeDelta > 0.25f || windDelta > 2.0f) historyValid = false;
    }
    // A cut/settings invalidation starts a deterministic spatially distributed
    // phase sequence. Ping-pong ownership stays on m_FrameIndex, so resetting
    // reconstruction never changes resource selection or dispatch work.
    if (!historyValid) m_TemporalPhase = 0u;

    const bool bakeShapeNoiseThisFrame =
        !m_NoiseBaked && m_NoisePipe && m_ShapeTex;
    const bool bakeWeatherThisFrame =
        !m_WeatherBaked && m_WeatherPipe && m_WeatherTex;
    const bool bakeDetailNoiseThisFrame =
        !m_DetailBaked && m_DetailPipe && m_DetailTex;
    const bool bakeCurlNoiseThisFrame =
        !m_CurlBaked && m_CurlPipe && m_CurlTex;
    // 雲は風移流とは別に対流変形するため、自己影は毎フレーム更新する。
    // 安定時は4位相へ分け、履歴や座標が不連続なフレームだけ全体を更新する。
    const bool rebuildShadowCacheThisFrame =
        shadowResourcesReady &&
        (m_NoiseBaked || bakeShapeNoiseThisFrame) &&
        (m_WeatherBaked || bakeWeatherThisFrame) &&
        (m_DetailBaked || bakeDetailNoiseThisFrame) &&
        (m_CurlBaked || bakeCurlNoiseThisFrame);
    if (!rebuildShadowCacheThisFrame) m_ShadowCacheValid = false;
    const bool rebuildWorldShadowThisFrame =
        worldShadowResourcesReady &&
        safeSun.y > kVolumetricCloudWorldShadowMinimumSunY &&
        safeCoverage > 0.001f &&
        (m_NoiseBaked || bakeShapeNoiseThisFrame) &&
        (m_WeatherBaked || bakeWeatherThisFrame) &&
        (m_DetailBaked || bakeDetailNoiseThisFrame) &&
        (m_CurlBaked || bakeCurlNoiseThisFrame);
    if (!rebuildWorldShadowThisFrame) m_WorldShadowValid = false;
    const bool shadowCacheNeedsFullRefresh =
        rebuildShadowCacheThisFrame &&
        (!m_ShadowCacheValid || shadowGridChanged);
    const bool worldShadowNeedsFullRefresh =
        rebuildWorldShadowThisFrame &&
        (!m_WorldShadowValid || worldShadowMappingChanged);
    const bool refreshAllShadows =
        m_ReferenceMode || !historyValid ||
        shadowCacheNeedsFullRefresh || worldShadowNeedsFullRefresh;
    const u32 shadowUpdateDivisor = refreshAllShadows
        ? 1u : kVolumetricCloudShadowTemporalDivisor;
    const u32 shadowUpdatePhase = m_FrameIndex & 3u;
    const u32 shadowUpdateOffsetX = shadowUpdateDivisor == 1u
        ? 0u : (shadowUpdatePhase & 1u);
    const u32 shadowUpdateOffsetY = shadowUpdateDivisor == 1u
        ? 0u : ((shadowUpdatePhase >> 1u) & 1u);
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
    m_LastFrameWorkload =
        PlanVolumetricCloudFrameWorkload(workloadPlan);
    m_LastFrameWorkload.attempted = true;
    m_LastFrameWorkload.history_was_available =
        historyWasAvailable;
    m_LastFrameWorkload.history_reused = historyValid;
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
        (m_FrameIndex & 4080u) | (m_TemporalPhase & 15u);
    // 参照描画では履歴も時間方向の再構成も使わない。そのフレームだけで完結させる
    // (再構成の影響を混ぜたままだと、ライティングの良し悪しを判断できない)。
    cb.temporal = FVec4{
        (historyValid && !m_ReferenceMode) ? 1.0f : 0.0f,
        m_PrevWindOffset,
        static_cast<f32>(temporalFrame),
        (temporalSuperResolution && !m_ReferenceMode)
            ? static_cast<f32>(kVolumetricCloudUltraTraceDivisor)
            : 0.0f };
    const f32 layerThickness = m_Layer.top_height - m_Layer.base_height;
    const f32 layerCanonicalScale = 1.6f / layerThickness;
    cb.layer = FVec4{m_Layer.base_height, m_Layer.top_height,
                     m_Layer.horizontal_noise_scale, layerCanonicalScale};
    const bool temporalHistoryStationary = historyValid &&
        cameraDeltaSquared <= 0.0025f && matrixDelta <= 0.002f;
    cb.worldOrigin = FVec4{
        worldOrigin.x, worldOrigin.y, worldOrigin.z,
        temporalHistoryStationary ? 1.0f : 0.0f};
    const f32 invShadowExtent =
        1.0f / kVolumetricCloudShadowCacheExtent;
    cb.shadowGrid = FVec4{m_ShadowGridMinQ.x, m_ShadowGridMinQ.y,
                          invShadowExtent, invShadowExtent};
    cb.shadowState = FVec4{
        rebuildShadowCacheThisFrame ? 1.0f : 0.0f,
        0.10f,
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheWidth),
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheHeight)};
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
    cb.cloudWeatherControl = FVec4{
        m_Weather.CloudType,
        m_Weather.CloudTypeInfluence,
        m_Weather.Precipitation,
        m_Weather.PrecipitationInfluence};
    cb.cloudShadowUpdate = FVec4{
        static_cast<f32>(shadowUpdateOffsetX),
        static_cast<f32>(shadowUpdateOffsetY),
        static_cast<f32>(shadowUpdateDivisor),
        refreshAllShadows ? 1.0f : 0.0f};
    cb.cloudWorldShadowMap = FVec4{m_WorldShadowMapMinReferenceXz.x, m_WorldShadowMapMinReferenceXz.y, 1.0f / kVolumetricCloudWorldShadowMapExtent, m_WorldShadowReferenceHeight};
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
    const f32 occupancyCoverage =
        safeCoverage + 0.08f < 1.0f ? safeCoverage + 0.08f : 1.0f;
    const f32 occupancyHeightCoverage =
        occupancyCoverage < 0.72f ? occupancyCoverage : 0.72f;
    const f32 densityHeightCoverage =
        safeCoverage < 0.72f ? safeCoverage : 0.72f;
    cb.cloudCoverage = FVec4{
        0.72f - 0.36f * occupancyCoverage,
        0.72f - 0.36f * safeCoverage,
        0.50f - 0.16f * occupancyHeightCoverage,
        0.50f - 0.16f * densityHeightCoverage};
    const f32 occupancyWeatherUpper =
        cb.cloudCoverage.x + 0.14f < 0.98f
            ? cb.cloudCoverage.x + 0.14f : 0.98f;
    const f32 densityWeatherUpper =
        cb.cloudCoverage.y + 0.14f < 0.98f
            ? cb.cloudCoverage.y + 0.14f : 0.98f;
    const f32 unclampedFineStep =
        0.035f /
        (m_Layer.horizontal_noise_scale > 0.001f
             ? m_Layer.horizontal_noise_scale : 0.001f);
    const f32 fineStep =
        unclampedFineStep < 0.5f ? 0.5f
        : (unclampedFineStep > 2.0f ? 2.0f : unclampedFineStep);
    const f32 lightStep =
        0.0075f /
        (layerCanonicalScale > 0.0001f
             ? layerCanonicalScale : 0.0001f);
    cb.cloudCoverageReciprocals = FVec4{
        1.0f / (occupancyWeatherUpper - cb.cloudCoverage.x),
        1.0f / (densityWeatherUpper - cb.cloudCoverage.y),
        fineStep,
        lightStep};
    // Camera, rebase origin and layer are invariant for the complete trace
    // dispatch. Build the two factorized shell quadratics once on the CPU
    // instead of reconstructing both c terms and the same ray-origin b term
    // in every quarter-resolution invocation.
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
    // 上層があるなら殻の外側をそこまで伸ばす。伸ばさないとレイが下層の天井で止まり、
    // 上層はいつまでも見えない。あいだの隙間は密度 0 なので粗い刻みで素通りする。
    const bool hasUpperLayer =
        m_UpperLayer.top_height > m_UpperLayer.base_height &&
        m_UpperLayer.base_height >= m_Layer.top_height;
    const f32 shellTopHeight =
        hasUpperLayer ? m_UpperLayer.top_height : m_Layer.top_height;
    cb.cloudShellTerms = FVec4{
        shellC(shellTopHeight), 0.0f, 0.0f, 0.0f};
    cb.cloudUpperLayer = hasUpperLayer
        ? FVec4{m_UpperLayer.base_height, m_UpperLayer.top_height,
                1.0f / (m_UpperLayer.top_height - m_UpperLayer.base_height),
                1.0f}
        : FVec4{0.0f, 0.0f, 0.0f, 0.0f};
    // どちらの層も厚さ全体を 1.6 の基準幅へ写す。上層の尺度と、その逆数から求める
    // 光採取間隔をここで一度だけ計算し、GPU の密度標本ごとの除算を避ける。
    const f32 upperLayerCanonicalScale = hasUpperLayer
        ? 1.6f / (m_UpperLayer.top_height - m_UpperLayer.base_height)
        : layerCanonicalScale;
    const f32 upperLayerLightStep =
        0.0075f /
        (upperLayerCanonicalScale > 0.0001f
             ? upperLayerCanonicalScale : 0.0001f);
    cb.cloudUpperTerms = FVec4{
        m_UpperLayer.coverage_scale, m_UpperLayer.density_scale,
        upperLayerCanonicalScale, upperLayerLightStep};
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
        m_Lighting.GroundColor.z, 0.0f};
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
    cb.cloudRange = FVec4{
        maxDistance,
        maxDistance * (1.0f - fadeFraction),
        m_Range.StepGrowth,
        0.0f};
    m_Cb->Update(&cb, sizeof(cb));
    // 初回に Perlin-Worley shape noise (128^3) を焼く (1 回のみ、以降 SRV で sample)。
    if (bakeShapeNoiseThisFrame) {
        cl.SetComputePipeline(*m_NoisePipe);
        cl.BindUav(0, *m_ShapeTex);
        cl.Dispatch(32, 32, 32);   // 128/4
        m_NoiseBaked = true;
    }
    if (bakeWeatherThisFrame) {
        cl.SetComputePipeline(*m_WeatherPipe);
        cl.BindUav(0, *m_WeatherTex);
        cl.Dispatch(64, 64, 1);    // 512/8
        m_WeatherBaked = true;
    }
    if (bakeDetailNoiseThisFrame) {
        cl.SetComputePipeline(*m_DetailPipe);
        cl.BindUav(0, *m_DetailTex);
        cl.Dispatch(16, 16, 16);   // 64/4
        m_DetailBaked = true;
    }
    if (bakeCurlNoiseThisFrame) {
        cl.SetComputePipeline(*m_CurlPipe);
        cl.BindUav(0, *m_CurlTex);
        cl.Dispatch(16, 16, 1);    // 128/8
        m_CurlBaked = true;
    }
    if (rebuildShadowCacheThisFrame) {
        cl.SetComputePipeline(*m_ShadowPipe);
        cl.SetConstantBuffer(0, *m_Cb);
        cl.SetTexture(0, *m_ShapeTex);
        cl.SetTexture(1, *m_WeatherTex);
        cl.SetTexture(2, *m_DetailTex);
        cl.SetTexture(3, *m_CurlTex);
        // 現在の計算RHIはUAV登録番号を連続させるため、u0/u1へ有効な代替テクスチャを
        // 割り当てる。CSCloudShadowが書き込むのはu2だけである。
        cl.BindUav(0, *m_CloudTex);
        cl.BindUav(1, *m_CloudDepth);
        cl.BindUav(2, *m_ShadowTex);
        const u32 updateWidth = CloudCeilDivisor(kVolumetricCloudShadowCacheWidth - shadowUpdateOffsetX, shadowUpdateDivisor);
        const u32 updateDepth = CloudCeilDivisor(kVolumetricCloudShadowCacheDepth - shadowUpdateOffsetY, shadowUpdateDivisor);
        cl.Dispatch((updateWidth + 3u) / 4u, (kVolumetricCloudShadowCacheHeight + 3u) / 4u, (updateDepth + 3u) / 4u);
        m_ShadowCacheValid = true;
        ++m_ShadowCacheDispatchCount;
    }
    if (rebuildWorldShadowThisFrame) {
        cl.SetComputePipeline(*m_WorldShadowPipe);
        cl.SetConstantBuffer(0, *m_Cb);
        cl.SetTexture(0, *m_ShapeTex);
        cl.SetTexture(1, *m_WeatherTex);
        cl.SetTexture(2, *m_DetailTex);
        cl.SetTexture(3, *m_CurlTex);
        cl.BindUav(0, *m_WorldShadowTex);
        const u32 updateWidth = CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution - shadowUpdateOffsetX, shadowUpdateDivisor);
        const u32 updateHeight = CloudCeilDivisor(kVolumetricCloudWorldShadowMapResolution - shadowUpdateOffsetY, shadowUpdateDivisor);
        cl.Dispatch((updateWidth + 7u) / 8u, (updateHeight + 7u) / 8u, 1u);
        m_WorldShadowValid = true;
        ++m_WorldShadowDispatchCount;
    }
    cl.SetComputePipeline(*m_CloudPipe);
    cl.SetConstantBuffer(0, *m_Cb);
    if (m_ShapeTex) cl.SetTexture(0, *m_ShapeTex);   // shape noise SRV (UAV→SRV は Dispatch の TRANSITION commit)
    if (m_WeatherTex) cl.SetTexture(1, *m_WeatherTex);
    if (m_DetailTex) cl.SetTexture(2, *m_DetailTex);
    if (m_CurlTex) cl.SetTexture(3, *m_CurlTex);
    if (m_ShadowTex && m_ShadowCacheValid) {
        cl.SetTexture(4, *m_ShadowTex);
    } else if (m_ShapeTex) {
        // 型が一致する代替3次元テクスチャ。shadowState.xが採取を止める。
        cl.SetTexture(4, *m_ShapeTex);
    }
    cl.BindUav(0, *m_CloudTex);
    cl.BindUav(1, *m_CloudDepth);
    cl.Dispatch((m_W + 7u) / 8u, (m_H + 7u) / 8u, 1);

    // Scaled UAV → full-resolution temporal color/depth.  One compute pass
    // shares reconstruction/history reads and writes both formats as UAVs,
    // avoiding the mixed-MRT fullscreen draw overhead. Ping-pong keeps input
    // SRVs and output UAVs disjoint.
    const u32 cur = m_FrameIndex & 1u;
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

    m_LastFrameWorkload.submitted = true;
    if (m_WorkloadSubmissionIndex != kCloudWorkloadMaximum) {
        ++m_WorkloadSubmissionIndex;
    }
    m_LastFrameWorkload.submission_index =
        m_WorkloadSubmissionIndex;
    m_ResolvedIndex = cur;
    ++m_FrameIndex;
    m_TemporalPhase = (m_TemporalPhase + 1u) & 15u;
    m_HistoryValid = true;
    m_PrevCameraRelativeViewProj = cameraRelativeViewProj;
    m_PrevCameraRelativeInvViewProj = camera_relative_inv_view_proj;
    m_PrevCamPos = cam_pos;
    m_WorldOrigin = worldOrigin;
    m_PrevSunDir = safeSun;
    m_PrevSunColor = safeSunColor;
    m_PrevSkyColor = safeSkyColor;
    m_PrevWindOffset = windOffset;
    m_PrevWindSpeed = safeWind;
    m_PrevCoverage = safeCoverage;
    m_PrevDensity = safeDensity;
    m_PrevTime = safeTime;
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

void CVolumetricClouds::Composite(IRhiCommandList& cl, IRhiTexture& scene_depth,
                                  u32 scW, u32 scH,
                                  IRhiTexture* atmosphere_volume,
                                  IRhiTexture* atmosphere_transmittance,
                                  f32 atmosphere_max_distance) noexcept {
    if (!m_Ready || !m_HistoryValid || !m_HistoryColor[m_ResolvedIndex] ||
        !m_HistoryDepth[m_ResolvedIndex] || !m_CompPipe || !m_Cb) return;
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
    cl.SetTexture(0, *m_HistoryColor[m_ResolvedIndex]); // UAV→SRV transition は RHI が処理
    cl.SetTexture(1, scene_depth);
    cl.SetTexture(2, *m_HistoryDepth[m_ResolvedIndex]);
    if (useAtmosphere) cl.SetTexture(3, *atmosphere_volume);
    if (useAtmosphere) cl.SetTexture(4, *atmosphere_transmittance);
    cl.Draw(3, 0);
    if (m_LastFrameWorkload.submitted &&
        m_LastFrameWorkload.composite_draws != ~u32{0}) {
        ++m_LastFrameWorkload.composite_draws;
    }
}

void CVolumetricClouds::Shutdown() noexcept {
    m_ShapeTex.Reset(); m_NoisePipe.Reset(); m_NoiseCs.Reset(); m_NoiseBaked = false;
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
