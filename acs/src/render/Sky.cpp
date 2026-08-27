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
    // x=radius (1-cos), y=glow, z=出力ディザ強度, w=予約
    float4   sun_params;
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
void CSky::Render(IRhiCommandList& cl, const CCamera& camera) noexcept {
    if (!m_Pipeline || !m_Cb) return;
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
// 128^3 RG16F: R=低周波 Perlin-Worley/Worley 合成、G=追加領域用 Perlin-Worley。繰り返し可能。
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
// 主形状だけを水平4方向へ短く平均する。高周波の尖った枝を焼き込み時に消し、
// 高さ方向の3D変化は残したまま、実行時の追加採取なしで雲体を連続させる。
float fullShapeAt(float3 uvw){
    // 低周波の3Dゆがみで雲塊の側面を上下へずらし、同じX・Zの房が
    // 高さ方向へ一直線に積み重なるのを防ぐ。追加計算は初回焼き込みだけで行う。
    float warpX=gnoise(uvw+float3(0.173,0.417,0.619),1.0);
    float warpZ=gnoise(uvw+float3(0.731,0.251,0.847),1.0);
    float3 domainWarp=float3(
        warpX-0.5,(warpX+warpZ)*0.5-0.5,warpZ-0.5)
        *float3(0.22,0.28,0.22);
    float3 warpedUvw=uvw+domainWarp;
    float perlin2 = gnoise(warpedUvw*2.0,2.0);
    float perlin4 = gnoise(warpedUvw*4.0,4.0);
    float perlin8 = gnoise(warpedUvw*8.0,8.0);
    float perlin16 = gnoise(warpedUvw*16.0,16.0);
    // 主形状は2～8セルを基準にし、16セル級の細部は後段の侵食へ任せる。
    // 中～高周波を少し増やし、同じX・Zへ細い房が縦に積み重なるのを抑える。
    float perlinFull = perlin2*0.46+perlin4*0.36
                      +perlin8*0.14+perlin16*0.04;
    float wa = worley(warpedUvw,4.0);
    float wb = worley(warpedUvw,8.0);
    float wc = worley(warpedUvw,16.0);
    float worleyFull = wa*0.82+wb*0.15+wc*0.03;
    // 低周波Perlinで連続した雲塊の外形を作り、4セルWorleyで大きな房だけを
    // 主形状の内側へ残す。8・16セルは弱い補助に下げ、粒状の柱を作らない。
    // 細胞補正の振幅は抑え、低周波の連続した雲塊へ局所的な房だけを重ねる。
    // 遷移が広過ぎると雲塊の大半が同じ密度へ寄り、照明の陰影が一枚の板になる。
    // 遷移を少し締め、芯と縁の差を戻しながら空洞が粒へ分断されない範囲に保つ。
    float broadMass=smoothstep(0.42,0.64,perlinFull);
    float lobeMass=smoothstep(0.30,0.82,wa);
    float cellularMass=smoothstep(0.36,0.88,worleyFull);
    // 焼き込み済みの低周波形状を外形の内側の大きな房にも使う。外形へ足し算せず、
    // 既存の連続した雲体を上下へ濃淡化するため、粒状の浮遊物を増やさない。
    float interiorLobe=smoothstep(0.24,0.78,perlinFull);
    float bodyVariation=lerp(0.86,1.18,interiorLobe);
    // 大きな房の振幅だけを広げ、主形状の外側へ新しい粒を足さずに
    // 連続した雲塊の中の膨らみと谷を読み取れるようにする。
    return broadMass*bodyVariation*lerp(0.58,1.12,lobeMass)
                   *lerp(0.86,1.02,cellularMass);
}
float fullShapeColumnAt(float3 uvw){
    // 水平だけでなく上下も平均し、同じ柱の細い縦筋を焼き込み時に抑える。
    // 中心の形状を少し多く残し、近傍平均で房の明暗まで消さない。
    // 近傍標本の数は変えず、焼き込み後の実行時負荷と時間安定性を保つ。
    return fullShapeAt(uvw)*0.60
         +fullShapeAt(uvw+float3(0.035,0.0,0.0))*0.08
         +fullShapeAt(uvw-float3(0.035,0.0,0.0))*0.08
         +fullShapeAt(uvw+float3(0.0,0.0,0.035))*0.08
         +fullShapeAt(uvw-float3(0.0,0.0,0.035))*0.08
         +fullShapeAt(uvw+float3(0.0,0.060,0.0))*0.04
         +fullShapeAt(uvw-float3(0.0,0.060,0.0))*0.04;
}
[numthreads(4,4,4)]
void CSNoise(uint3 id : SV_DispatchThreadID){
    if(id.x>=128u||id.y>=128u||id.z>=128u) return;
    float3 uvw=(float3(id)+0.5)/128.0;
    // 連続した広域形状と、輪郭だけを崩す Perlin-Worley を別々に焼き込む。
    float perlin2 = gnoise(uvw*2.0,2.0);
    float perlin4 = gnoise(uvw*4.0,4.0);
    float fullShape=fullShapeColumnAt(uvw);
    // Perlin-Worleyの0は雲の外側を表すため、負の下限から再マップして空洞を
    // 持ち上げない。低周波Perlinは空間の連結性を補う範囲だけ混ぜ、雲体の
    // 境界を埋めて柱へ戻さない。
    // 低密度の外縁を持ち上げ過ぎると、離れた小塊まで同時に残って粒状になる。
    // 主形状の内側だけを残し、低密度の枝を指数で持ち上げない。
    // 指数で0.5未満を膨らませると、輪郭の外側へ粒状の雲が伸びやすい。
    float baseCloud=saturate(fullShape);
    float billowCloud=perlin2*0.70+perlin4*0.30;
    // 低周波房は主形状の大きさだけを変え、主形状の外へ密度を足さない。
    // 補助値の加算は同じX・Zの全高度へ薄い密度を残し、柱と浮遊粒を作る。
    float broadMass=lerp(0.78,1.14,saturate(billowCloud));
    float macroCloud=saturate(baseCloud*broadMass);
    noiseOut[id]=float2(macroCloud,fullShape);
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
    float4 layer;      // x=world base Y, y=world top Y, z=XZ noise scale, w=1 m当たりの基準消散
    float4 worldOrigin;// xyz=discrete curved-shell tangent origin
    float4 shadowGrid; // xy=material-space min XZ, zw=inverse horizontal extents
    float4 shadowState;// x=valid, y=max tau disagreement, z=1/width/depth, w=1/height
    float4 groundHorizon;// xyz=camera local up, w=ground tangent elevation; <-1 disables
    float4 cloudFrameTerms;// xy=world wind, z=shape scale, w=1/(top-base)
    float4 cloudLightTangent;// xyz=CPU-hoisted Duff/Frisvad tangent
    float4 cloudLightBitangent;// xyz=CPU-hoisted Duff/Frisvad bitangent
    float4 cloudCoverage;// xy=天候しきい値、zw=基本形状雑音の固定正規化範囲
    // xy=inverse weather transition widths, z=view fine step, w=light step
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
    // x=上層の被覆, y=上層の濃さ, z=1 m当たりの基準消散, w=上層の光採取基準間隔
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
};
RWTexture2D<float4> cloudOut : register(u0);
RWTexture2D<float2> cloudDepthOut : register(u1); // x=不透明度加重ヒット距離, y=アルファ信頼度
// xy=太陽方向の平均深さと二標本差、zw=空・地面方向の積算密度。
RWTexture3D<float4> cloudShadowOut : register(u2);
Texture3D<float2> shapeNoise     : register(t0);   // (低周波形状, 全帯域形状)
Texture2D    weatherMap          : register(t1);   // coverage/type/precipitation/warp
Texture3D<float2> detailNoise    : register(t2);   // (低周波房, 三帯域侵食)
Texture2D    curlNoise           : register(t3);   // independent world-space curl field
// CSCloud は、現在フレームに生成した平均深さと二標本差を連続した t4 から読む。
Texture3D<float4> cloudShadowCache : register(t4);
SamplerState shapeNoise_sampler  : register(s0);   // wrap (tileable)
SamplerState weatherMap_sampler  : register(s1);   // world-scale wrap
SamplerState detailNoise_sampler : register(s2);   // wrap (tileable)
SamplerState curlNoise_sampler   : register(s3);   // world-scale wrap
SamplerState cloudShadowCache_sampler : register(s4); // clamp (finite cache footprint)

float remapc(float v,float a,float b,float c,float d){ return c + saturate((v-a)/max(b-a,1e-5))*(d-c); }
float hash13(float3 p){ p=frac(p*0.1031); p+=dot(p,p.zyx+31.32); return frac((p.x+p.y)*p.z); }
// CPU側でabs(g)<=0.99を保証する。前方または後方の鋭い頂点でも近い数の差を取らず、桁落ちを防ぐ。
float hg(float c,float g){ float a=abs(g); float oneMinusA=1.0-a; float alignedC=g>=0.0?c:-c; float d=oneMinusA*oneMinusA+2.0*a*max(1.0-alignedC,0.0); return (oneMinusA*(1.0+a))/(12.566370*pow(max(d,1e-6),1.5)); }
// 既に求めた区間透過率から、次数ごとに縮小した均質区間の散乱重みを解析積分する。
// 消散縮小率が0へ近づく場合も極限 contribution*opticalDepth を使い、0除算を起こさない。
float cloudReducedIntervalScatteringWeight(float opticalDepth,float intervalTransmittance,float contribution,float occlusion,float scatteringToExtinction){
    if(occlusion<=1e-4)
        return contribution*max(opticalDepth,0.0);
    return scatteringToExtinction*(1.0-saturate(intervalTransmittance));
}

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
// CPU側の kVolumetricCloudShadowCacheHeight と一致させる。一つのスレッドが縦列を完結させる。
static const uint CLOUD_SHADOW_CACHE_HEIGHT=32u;
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
// 光円すいの採取間隔も同じ基準幅へ合わせる。CPU で除算済みの値を選び、密度標本ごとの
// 除算を避ける。
float cloudLightStepFromBand(bool upperBand){
    float lightStep=cloudCoverageReciprocals.w;
    if(upperBand) lightStep=cloudUpperTerms.w;
    return lightStep;
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

// 一つの曲面雲層について、正の距離にある最初の連続区間を返す。
// 地面による遮蔽と最大距離は呼び出し側で扱い、層の下・中・上では同じ交差規則を使う。
bool intersectCloudShellTerms(float b,float innerC,float outerC,out float t0,out float t1){
    t0=0.0;
    t1=0.0;
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

// 下層と上層の交差区間を視点から近い順へ詰める。返り値は有効な区間数。
int packCloudBandIntervals(bool lowerHit,float2 lowerInterval,bool upperHit,float2 upperInterval,out float4 intervals){
    intervals=float4(0,0,0,0);
    if(lowerHit&&upperHit){
        bool lowerFirst=lowerInterval.x<=upperInterval.x;
        intervals=lowerFirst
            ?float4(lowerInterval,upperInterval)
            :float4(upperInterval,lowerInterval);
        return 2;
    }
    if(lowerHit){
        intervals.xy=lowerInterval;
        return 1;
    }
    if(upperHit){
        intervals.xy=upperInterval;
        return 1;
    }
    return 0;
}

// カメラ固定の主描画では、CPUで先行計算した下層・上層の係数を使う。
int intersectCloudBands(float3 rayDir,out float4 intervals){
    float b=dot(cloudShellRayOrigin.xyz,rayDir);
    float2 lowerInterval=float2(0,0);
    bool lowerHit=intersectCloudShellTerms(b,cloudShellRayOrigin.w,cloudShellTerms.x,lowerInterval.x,lowerInterval.y);
    float2 upperInterval=float2(0,0);
    bool upperHit=false;
    if(cloudUpperLayer.w>0.5){
        upperHit=intersectCloudShellTerms(b,cloudShellTerms.y,cloudShellTerms.z,upperInterval.x,upperInterval.y);
    }
    return packCloudBandIntervals(lowerHit,lowerInterval,upperHit,upperInterval,intervals);
}

// 任意の始点に対する曲面高度の二次方程式c項を、巨大な半径の二乗差を避けて求める。
float cloudShellCFromLocalPosition(float3 local,float altitude){
    return dot(local.xz,local.xz)
        +(local.y-altitude)
         *(2.0*CLOUD_PLANET_RADIUS+local.y+altitude);
}

// 立体物用の透過率地図は画素ごとに始点が異なるため、下層・上層の係数をその場で求める。
int intersectCloudBandsFromPosition(float3 rayOrigin,float3 rayDir,out float4 intervals){
    float3 local=rayOrigin-worldOrigin.xyz;
    float3 centreOffset=float3(local.x,CLOUD_PLANET_RADIUS+local.y,local.z);
    float b=dot(centreOffset,rayDir);
    float2 lowerInterval=float2(0,0);
    bool lowerHit=intersectCloudShellTerms(b,cloudShellCFromLocalPosition(local,layer.x),cloudShellCFromLocalPosition(local,layer.y),lowerInterval.x,lowerInterval.y);
    float2 upperInterval=float2(0,0);
    bool upperHit=false;
    if(cloudUpperLayer.w>0.5){
        upperHit=intersectCloudShellTerms(b,cloudShellCFromLocalPosition(local,cloudUpperLayer.x),cloudShellCFromLocalPosition(local,cloudUpperLayer.y),upperInterval.x,upperInterval.y);
    }
    return packCloudBandIntervals(lowerHit,lowerInterval,upperHit,upperInterval,intervals);
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
    float broadPotential=smoothstep(0.66,0.92,saturate(weather.a));
    float interiorPotential=smoothstep(0.50,0.96,saturate(cloudInterior));
    float localPotential=broadPotential*interiorPotential;
    // 対流域の外側では通常雲の高さへ戻し、全域指定時にも縁へ塔を残さない。
    // 二つの平滑補間をさらに掛け合わせ、成熟した雲中心だけが積乱雲の高さへ遷移する。
    return authoredTower*localPotential*localPotential;
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
    // 積乱雲本体はかなとこが十分に立ち上がるまで密度支持を保つ。
    // 本体、肩、かなとこを別々の山として最大化すると中層に密度0の谷ができるため、
    // 本体を緩やかに細めた上へかなとこを重ね、塔の支持領域を連続させる。
    float stormRiseEnd=riseEnds.w;
    float stormRiseBegin=stormRiseEnd*0.20;
    float stormBody=smoothstep(stormRiseBegin,stormRiseEnd,h)
                   *(1.0-0.34*smoothstep(0.34,0.74,h))
                   *(1.0-smoothstep(0.78,0.995,h));
    float anvil=smoothstep(0.56,0.70,h)
               *(1.0-smoothstep(0.80,0.995,h))*0.22;
    // 本体が雲頂側で連続しているため、かなとこは別の棚にならず不足部分だけを補う。
    float storm=saturate(stormBody+anvil*(1.0-stormBody));
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
float cloudColumnTopShift(float4 weather,float cloudInterior,float toweringStrength){
    float core=smoothstep(0.08,0.92,saturate(cloudInterior));
    // 通常の積雲にも雲頂の起伏を与える。積乱雲の強さだけで振幅を決めると、
    // 通常雲の上面が平面になり、密度場の横変化だけが柱として見える。
    float typePuff=smoothstep(0.26,0.72,saturate(weather.g));
    float precipitationPuff=smoothstep(0.20,0.70,saturate(weather.b));
    float puffStrength=max(typePuff,precipitationPuff*0.80);
    float reliefStrength=max(puffStrength,saturate(toweringStrength));
    // 雲頂の高さ差を読み取れる範囲まで増やす。密度の下限は変えず、
    // 雲体全体を一様に厚くして平板化することは避ける。
    float amplitude=lerp(0.018,0.100,reliefStrength);
    float evolvingWarp=clamp(weather.a-0.5+cloudLocalConvectionPhase(weather)*0.45,-0.5,0.5);
    float signal=clamp((core-0.45)*1.45+evolvingWarp*0.65,-1.0,1.0);
    return signal*amplitude;
}
// 既存の低周波天候模様から、柱ごとに物理層内の雲底を少し持ち上げる。
// 対流雲は共通の凝結高度へ底面が揃いやすいため、積乱雲ほど変化率を抑え、
// 雲頂の高さ変形とは独立して長い下向きの尾を作らない。
float cloudColumnBaseLift(float4 weather,float cloudInterior,float toweringStrength){
    float verticalType=saturate(max(weather.g,weather.b));
    float broadPattern=smoothstep(0.18,0.82,weather.a);
    float edgePattern=1.0-smoothstep(0.08,0.86,saturate(cloudInterior));
    float amplitude=lerp(0.006,0.014,verticalType);
    amplitude*=lerp(1.0,0.72,saturate(toweringStrength));
    float signal=saturate(0.10+broadPattern*0.75+edgePattern*0.15);
    return amplitude*signal;
}
// 通常部は層厚にかかわらず約2.4 kmを目安にし、局所対流核だけを全球上端近くへ伸ばす。
// 9.4 kmの積乱雲層で通常部まで84%へ固定していた旧式は、全雲塊を約7.9 kmの柱にしていた。
// 薄い上層雲では従来どおり高さ差を30%へ抑え、層全体を過度に分断しない。
float cloudColumnTop(float topShift,float toweringStrength,bool upperBand){
    float ordinaryTop=clamp(3000.0*cloudFrameTerms.w,0.38,0.88);
    float lowerCenter=lerp(ordinaryTop,0.92,saturate(toweringStrength));
    float topCenter=upperBand?0.96:lowerCenter;
    float shiftScale=upperBand?0.30:1.0;
    float minimumTop=upperBand?0.90:max(lowerCenter-0.12,0.20);
    return clamp(topCenter+topShift*shiftScale,minimumTop,0.995);
}
// 局所雲底から局所雲頂だけを0～1へ単調に再配置する。全球上端を固定した座標の曲げでは
// 雲頂位置を直接表せず、同じ層厚の柱が残るため、雲底と雲頂を明示的な支持境界にする。
float2 cloudColumnHeightAndSpan(float h,float topShift,float baseLift,float toweringStrength,bool upperBand){
    h=saturate(h);
    float bandScale=upperBand?0.35:1.0;
    float localBase=saturate(baseLift*bandScale);
    float localTop=max(cloudColumnTop(topShift,toweringStrength,upperBand),localBase+0.08);
    float columnSpan=max(localTop-localBase,0.001);
    return float2(saturate((h-localBase)/columnSpan),columnSpan);
}
// bake 済み volume は tile あたり 4..32 cells を既に含む。world frequency を下げ、
// 小さな blob の反復ではなく連続した cloud bank を作る。
float cloudShapeScale(){
    // CPU側で layer.z * 0.0022 を 0.00004～0.00020 に収め、1フレームに一度だけ求めた倍率を使う。
    return cloudFrameTerms.z;
}
// 基本形状の高さ方向へ、雲種ごとの低周波変化率を与える。
// 下層は局所的な膨らみを出し、上層は薄い見え方を保つため、横方向と同じ尺度を
// そのまま使わず、密度の高さプロファイルと干渉しない範囲へ分ける。
float cloudShapeVerticalSpan(bool upperBand){
    float inverseThickness=upperBand
        ?cloudUpperLayer.z:cloudFrameTerms.w;
    return cloudShapeScale()/max(inverseThickness,1e-6);
}
float cloudShapeVerticalVariation(bool upperBand,float toweringStrength){
    // 下層の物理厚で縦方向の進行距離を補正し、通常雲は約0.72周期、
    // 局所対流核は約1.30周期に収める。層厚を無視して一律に増やすと、
    // 同じ雲塊が縦に再出現して帯状になるため、発達強度から周期を決める。
    float verticalSpan=cloudShapeVerticalSpan(upperBand);
    float targetCycles=upperBand
        ?0.72
        :lerp(0.72,1.30,saturate(toweringStrength));
    float variation=targetCycles/max(verticalSpan,0.08);
    return upperBand
        ?clamp(variation,1.20,4.00)
        :clamp(variation,1.20,8.50);
}
float2 cloudWindWorld(){
    // CPU mirrors params.z*float2(0.9284767,0.3713907) once per frame.
    return cloudFrameTerms.xy;
}
// 2Dの天候包絡は柱ごとに固定したまま、3D密度形状だけを高度に応じて風下へ傾ける。
// Nubisの500 m基準で上下の断面を少しだけ風下へずらす。
float2 cloudHeightShapeShear(float layerHeight,bool upperBand){
    float bandScale=upperBand?0.25:1.0;
    return float2(0.9284767,0.3713907)
          *(850.0*saturate(layerHeight)*bandScale);
}
// 被覆、雲種、降水量は雲底と雲頂を決める2Dの柱条件なので、同じXZでは高度に依存させない。
// 高さ方向の立体変化は局所縦分布と3D形状へ任せ、柱条件が自分の雲頂を高度ごとに変えないようにする。
float4 cloudWeatherData(float3 p){
    float2 xz=p.xz-cloudWindWorld();
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
        dot(q,float3(0.0000000,0.8000000,0.6000000)),
        dot(q,float3(-0.7071068,-0.4242641,0.5656854)),
        dot(q,float3(0.7071068,-0.4242641,0.5656854)));
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
    float3 p,float4 weather,float2 cachedCurl,float physicalLayerHeight,
    float localLayerHeight,float toweringStrength,bool upperBand){
    float shapeScale=cloudShapeScale();
    float2 xz=p.xz-cloudWindWorld()
             +cloudHeightShapeShear(physicalLayerHeight,upperBand);
    float warpAngle=weather.g*6.2831853+weather.a*2.17;
    float warpSin,warpCos;
    sincos(warpAngle,warpSin,warpCos);
    float2 weatherWarp=float2(warpCos,warpSin)
                      *(weather.a-0.5)*190.0;
    float2 curlWarp=cachedCurl*22.0;
    // XZを基準に、下層だけ高さ方向の変化を少し早めて縦柱を崩す。天候による
    // 縦位相は430 mと240 mの物理距離で定義し、形状尺度を変えても同じ地点の
    // 持ち上がり量が変わらないようにする。0.07は固定の位相ずらしだけを担う。
    // 高さ座標を層内の正規化値へ直接掛けると、雲頂が低い通常雲でもテクスチャを
    // 一周して房が積み重なるため、横方向と同じ実寸周期へ戻す。
    float verticalSpan=cloudShapeVerticalSpan(upperBand);
    float verticalVariation=cloudShapeVerticalVariation(upperBand,toweringStrength);
    float globalCanonicalY=physicalLayerHeight*verticalSpan*verticalVariation;
    // 局所雲柱の高さを32%混ぜ、局所雲頂の膨らみを形状へ伝える。同じXZの雲が
    // 正規化によって複数周期へ引き伸ばされないよう、全球高さも同じ倍率で進める。
    float localCanonicalY=saturate(localLayerHeight)*verticalSpan*verticalVariation;
    float canonicalY=lerp(globalCanonicalY,localCanonicalY,0.32)
                    +(weather.g*430.0+weather.a*240.0)*shapeScale+0.07;
    float3 canonicalPosition=float3(
        (xz.x+weatherWarp.x+curlWarp.x)*shapeScale,
        canonicalY,
        (xz.y+weatherWarp.y+curlWarp.y)*shapeScale);
    // 直交回転で物理尺度を保ったまま世界軸との整列を外す。同じXZの上下で
    // tile周期が一致して断面が積み重なる、煙柱状の反復を防ぐ。
    return rotateNoise(canonicalPosition);
}
// Perlin-Worley形状は0付近を雲の外側として持つ。外形は焼き込み後の実測範囲
// 0.18～0.50で一度だけ決め、高さと被覆が薄い場所でも3D形状の連結性を保つ。
// 密度の減衰は後段で行う。
float cloudPositiveDensityNoiseThreshold(){
    return cloudCoverage.z;
}
// 基本雑音を焼き込み式から実測した固定範囲で正規化する。
float cloudNormalizedBaseDensity(float baseNoise){
    // 外形しきい値を通過した値を正規化した後、低密度側だけを少し抑える。
    // 線形値をそのまま使うと、広い雲縁が長い視線で積算されて灰色の霧になり、
    // 高密度の芯と空洞の差が失われる。強いS字変換は細い柱を作るため避ける。
    float normalized=remapc(
        baseNoise,cloudCoverage.z,cloudCoverage.w,0.0,1.0);
    return pow(normalized,1.28);
}
// 積雲の凝結高度では同じ天候塊の底面が概ね揃う。3D雑音の高い点だけを先に
// 可視化すると逆円すい状の尾になるため、雲底近傍だけに低い密度下限を置く。
// 高さ0.16より上では完全に0となり、2D天候場を雲頂まで柱状に押し出さない。
float cloudCondensationBaseSupport(
    float height,float weatherMask,float toweringStrength){
    float localBaseEntry=smoothstep(0.0,0.035,saturate(height));
    float baseBand=1.0-smoothstep(0.035,0.16,saturate(height));
    float weatherCore=smoothstep(0.12,0.72,saturate(weatherMask));
    float support=lerp(0.42,0.36,saturate(toweringStrength));
    return localBaseEntry*baseBand*weatherCore*support;
}
// 既に正規化した3D形状を、凝結面の有界な支持密度までだけ持ち上げる。
float cloudAnchoredBaseDensity(
    float baseDensity,float height,float weatherMask,
    float toweringStrength){
    float condensationSupport=cloudCondensationBaseSupport(
        height,weatherMask,toweringStrength)*0.28;
    return max(saturate(baseDensity),condensationSupport);
}
// 高さ分布と被覆は3D形状を削り取るしきい値ではなく、密度の重みとして適用する。
// 減算式で不足分を引くと中間層の低い値が全て0になり、尖った柱と粒へ分断される。
float cloudDimensionalProfile(float verticalProfile,float weatherMask){
    return saturate(verticalProfile)*saturate(weatherMask);
}
float cloudDensityFromDimensionalProfile(
    float baseDensity,float dimensionalProfile){
    // 高さと被覆は、薄い縁を平方根で持ち上げず、体積密度へ線形に掛ける。
    // 平方根は低密度の端部を過剰に残し、雲底の柱と灰色の棚を生みやすい。
    return saturate(baseDensity)*saturate(dimensionalProfile);
}
// 雲体の中層だけ、既に存在する密度の明暗幅を広げる。0の外形へ密度を足さず、
// 1の芯も飽和させたままなので、雲縁の位置と連続性を変えずに雲中の厚みを読ませる。
// 低詳細度の自己遮蔽と表示密度で同じ補正を使い、光だけが別の雲形状になるのを防ぐ。
float cloudInteriorDensityContrast(
    float density,float height,float weatherMask){
    float middleBand=smoothstep(0.10,0.30,saturate(height))
                    *(1.0-smoothstep(0.70,0.94,saturate(height)));
    float coreWeight=smoothstep(0.22,0.76,saturate(weatherMask));
    float contrastWeight=0.42*middleBand*coreWeight;
    float contrasted=saturate(
        0.5+(saturate(density)-0.5)*1.32);
    return lerp(saturate(density),contrasted,contrastWeight);
}
// 雲頂だけ低周波形状の明暗を少し強め、平らな板ではなく膨らみと谷を見せる。
// 雲底と中層には適用せず、密度の連続性と雲中の視程を変えない。
float cloudTopReliefDensity(
    float baseDensity,float height,float toweringStrength){
    float topWeight=smoothstep(0.46,0.88,saturate(height))
                   *lerp(0.30,0.54,saturate(toweringStrength));
    // 高密度側を1へ押し付ける smoothstep は雲頂を一枚の面へ均すため、
    // 基準密度の中心から傾きを少しだけ広げ、明暗の順序と平均を保つ。
    float relieved=saturate(0.5+(saturate(baseDensity)-0.5)*1.42);
    return lerp(saturate(baseDensity),relieved,topWeight);
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
// 雲底では大きな房を保ち、雲頂へ近づくほど中間帯域を混ぜて積雲の段階的な膨らみを作る。
// 二領域の差と合計1の重みを使うため、既存の最大変形量は厳密な上限のまま保たれる。
float cloudBillowOffset(float2 detailA,float2 detailB,float height,float middleVisibility){
    float topMiddleWeight=0.48*smoothstep(0.38,0.90,saturate(height))*saturate(middleVisibility);
    float coarseDifference=detailA.r-detailB.r;
    float middleDifference=cloudDetailMiddleBand(detailA)-cloudDetailMiddleBand(detailB);
    return lerp(coarseDifference,middleDifference,topMiddleWeight)*cloudBillowMaximumOffset(height);
}
// 採取間隔が各帯域の半周期へ近づく前に細部を消し、別の低周波模様への折り返しを防ぐ。
float cloudShapeFrequencyVisibility(
    float sampleSpacing,float domainScale,float frequency){
    float footprint=max(sampleSpacing,0.0)*cloudShapeScale()
                    *domainScale*frequency;
    return 1.0-smoothstep(0.22,0.52,footprint);
}
// Rの連続した低周波雲体を支持範囲として残し、同じ標本のGに焼き込んだ
// Perlin-Worleyだけで輪郭を内側へ削る。雲頂ほど房を強めるが、削減率を
// 14～26%へ制限して天候中心の雲体を粒へ分断しない。
float cloudBaseShapeBand(
    float2 shapeBands,float sampleSpacing,float domainScale,float height){
    float fineVisibility=cloudShapeFrequencyVisibility(
        sampleSpacing,domainScale,32.0);
    float billowSignal=smoothstep(0.18,0.70,saturate(shapeBands.g));
    float topDetail=smoothstep(0.35,0.90,saturate(height));
    float maximumErosion=fineVisibility*lerp(0.18,0.34,topDetail);
    return saturate(shapeBands.r)
          *lerp(1.0-maximumErosion,1.0,billowSignal);
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
float cloudWeatherMaskForLayer(float4 weather,float coverage,float layerHeight,float toweringStrength){
    float threshold=cloudWeatherThreshold(coverage);
    float upper=min(threshold+0.14,0.98);
    float inverseTransitionWidth=1.0/max(upper-threshold,0.001);
    return cloudWeatherMaskFromTermsForLayer(weather,threshold,inverseTransitionWidth,layerHeight,toweringStrength);
}
// 2D天候場は雲の発生域を囲う包絡として使い、実際の境界は3D基本形状へ渡す。
// 元のしきい値をそのまま密度へ掛けると、天候場の縦断面が柱として残るため、
// 低い側へ少し広げた柔らかい包絡を使う。広がった部分は3D形状が無ければ0になる。
float cloudWeatherEnvelopeMaskFromTerms(
    float4 weather,float threshold,float inverseTransitionWidth,
    float layerHeight,float toweringStrength){
    float sourceWidth=1.0/max(inverseTransitionWidth,0.001);
    float envelopeThreshold=max(threshold-0.045,0.0);
    float envelopeWidth=min(sourceWidth+0.045,0.20);
    float envelopeUpper=min(envelopeThreshold+envelopeWidth,0.98);
    float envelopeInverseWidth=1.0/max(envelopeUpper-envelopeThreshold,0.001);
    return cloudWeatherMaskFromTermsForLayer(
        weather,envelopeThreshold,envelopeInverseWidth,
        layerHeight,toweringStrength);
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
    // 第1領域を雲体の低周波形状として使い、雲の連続した塊を先に確定する。
    // 詳細な房と侵食は後段の密度処理へ分離し、別位相の領域で雲体を分断しない。
    float shape=cloudBaseShapeBand(a,sampleSpacing,1.0,height);
    [branch] if(shape<rejectionThreshold-1e-5) return;
    shapeResult=saturate(shape);
}
// 光円すい側も視線側と同じ低周波形状を使い、自己影だけが別の雲模様にならないようにする。
void cloudBaseShapeLighting(
    float3 uvw,float rejectionThreshold,float sampleSpacing,float height,
    out float shapeResult){
    shapeResult=0.0;
    float2 a=shapeNoise.SampleLevel(shapeNoise_sampler,uvw,0);
    float shape=cloudBaseShapeBand(a,sampleSpacing,1.0,height);
    [branch] if(shape<rejectionThreshold-1e-5) return;
    shapeResult=saturate(shape);
}

// 視線採取では天候、渦、基本形状を占有判定用に一度だけ評価し、詳細密度へ再利用する。
// 高周波の独立した詳細採取だけを後段へ残し、各光標本で同じ柱条件を再構成しない。
struct CloudMacroSample {
    float4 weather;
    float2 curl;
    float baseNoise;
    float weatherMask;
    // 高さ別のかなとこ・くびれを加える前の柱内部位置。局所雲頂を高さから独立させる。
    float columnInterior;
    // 作者指定の積乱雲強度を低周波天候場で局所化した値。高さ、くびれ、かなとこで共有する。
    float toweringStrength;
    // 最終密度の被覆境界から雲柱内部までを表す補間値。
    float densityWeatherMask;
    // 被覆を掛ける前の縦分布。後段で dimensional profile を作るとき一度だけ使う。
    float heightProfile;
    // 全球の雲殻内での物理高さ。3D基本形状の等方な座標にだけ使う。
    float layerHeight;
    // 球殻基準の高度。世界Yや原点再配置に依存しない詳細3D座標に使う。
    float altitude;
    // 局所雲底と局所雲頂の間で正規化した高さ。縦分布と形状制御に使う。
    float height;
    // 全球層の正規化座標に対する局所雲柱の幅。物理メートルの雲底幅を局所座標へ移す。
    float columnSpan;
    // 高度計算で確定した層。後段の密度と積分尺度で再利用し、同じ高度を再計算しない。
    float upperBand;
};
// キャッシュ外でも、局所密度を雲柱境界まで積分した無次元の光学的厚さへ変換する。
// xyは空方向と地面方向であり、両方の和は同じ局所雲柱の全厚に一致する。
float2 cloudAmbientFallbackOpticalDepth(CloudMacroSample macro,float localDensity){
    bool upperBand=macro.upperBand>0.5;
    float bandThickness=upperBand
        ?cloudUpperLayer.y-cloudUpperLayer.x:layer.y-layer.x;
    float columnThickness=max(bandThickness,0.0)
                         *max(macro.columnSpan,0.0);
    float fullColumnDepth=max(localDensity,0.0)*columnThickness
        *cloudOpticalDepthScaleFromBand(upperBand);
    float h=saturate(macro.height);
    return fullColumnDepth*float2(1.0-h,h);
}
CloudMacroSample sampleCloudMacro(
    float3 p,float4 coverageTerms,float sampleSpacing){
    CloudMacroSample macro;
    float3 sampleUvw=float3(0,0,0);
    macro.weather=float4(0,0,0,0);
    macro.curl=float2(0,0);
    macro.baseNoise=0.0;
    macro.weatherMask=0.0;
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
    macro.weather=cloudWeatherData(p);
    macro.upperBand=upperBand?1.0:0.0;
    // 局所雲頂はかなとこ・くびれの高さ別補正より先に、未変形の被覆から一度だけ決める。
    // 逆順では同じXZ柱の高さごとに雲頂条件が変わり、柱座標が自己矛盾する。
    macro.columnInterior=cloudWeatherMaskFromTerms(macro.weather,coverageTerms.y,cloudCoverageReciprocals.y);
    macro.toweringStrength=cloudLocalToweringStrength(macro.weather,macro.columnInterior);
    float2 columnMetrics=cloudColumnHeightAndSpan(layerHeight,cloudColumnTopShift(macro.weather,macro.columnInterior,macro.toweringStrength),cloudColumnBaseLift(macro.weather,macro.columnInterior,macro.toweringStrength),macro.toweringStrength,upperBand);
    macro.height=columnMetrics.x;
    macro.columnSpan=columnMetrics.y;
    macro.weatherMask=cloudWeatherEnvelopeMaskFromTerms(
        macro.weather,coverageTerms.x,cloudCoverageReciprocals.x,
        macro.height,macro.toweringStrength);
    // 形状による空間棄却も上層の被覆倍率に合わせ、空を一様な二層目で閉じない。
    if(upperBand) macro.weatherMask*=cloudUpperTerms.x;
    if(macro.weatherMask>0.001){
        // 最終密度のかなとこ・くびれは局所高さへ合わせ、縦分布との帯域ずれを作らない。
        // 包絡は早期棄却の余白だけに使い、実密度は発生境界へ戻す。
        // 包絡まで密度へ掛けると、雲縁の薄い支持が広域へ残り、積乱雲が板状に見える。
        macro.densityWeatherMask=cloudWeatherMaskFromTermsForLayer(
            macro.weather,coverageTerms.y,cloudCoverageReciprocals.y,
            macro.height,macro.toweringStrength);
        if(upperBand) macro.densityWeatherMask*=cloudUpperTerms.x;
        float sampledProfile=cloudProfile(
            macro.height,macro.weather.g,macro.toweringStrength,
            macro.columnSpan,upperBand);
        if(sampledProfile>0.0){
            macro.heightProfile=saturate(sampledProfile);
            float rejectionThreshold=cloudPositiveDensityNoiseThreshold();
            macro.curl=cloudCurlOffset(p);
            sampleUvw=cloudUVW(
                p,macro.weather,macro.curl,macro.layerHeight,
                macro.height,macro.toweringStrength,upperBand);
            // 後段の詳細体積が外側へ膨らませられる最大量を早期棄却にも含める。
            cloudBaseShape(
                sampleUvw,
                rejectionThreshold-cloudBillowMaximumOffset(macro.height),
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
    macro.weather=cloudWeatherData(p);
    macro.upperBand=upperBand?1.0:0.0;
    macro.columnInterior=cloudWeatherMask(macro.weather,weatherCoverage);
    macro.toweringStrength=cloudLocalToweringStrength(macro.weather,macro.columnInterior);
    float2 columnMetrics=cloudColumnHeightAndSpan(layerHeight,cloudColumnTopShift(macro.weather,macro.columnInterior,macro.toweringStrength),cloudColumnBaseLift(macro.weather,macro.columnInterior,macro.toweringStrength),macro.toweringStrength,upperBand);
    macro.height=columnMetrics.x;
    macro.columnSpan=columnMetrics.y;
    macro.weatherMask=cloudWeatherEnvelopeMaskFromTerms(
        macro.weather,cloudWeatherThreshold(weatherCoverage),
        1.0/0.14,macro.height,macro.toweringStrength);
    if(upperBand) macro.weatherMask*=cloudUpperTerms.x;
    // 光採取も視線の実密度と同じ境界を使い、早期棄却用の包絡を光路へ広げない。
    macro.densityWeatherMask=cloudWeatherMaskForLayer(
        macro.weather,weatherCoverage,macro.height,macro.toweringStrength);
    if(upperBand) macro.densityWeatherMask*=cloudUpperTerms.x;
    if(macro.weatherMask>0.001){
        float sampledProfile=cloudProfile(
            macro.height,macro.weather.g,macro.toweringStrength,
            macro.columnSpan,upperBand);
        if(sampledProfile>0.0){
            macro.heightProfile=saturate(sampledProfile);
            float rejectionThreshold=cloudPositiveDensityNoiseThreshold();
            macro.curl=cloudCurlOffset(p);
            cloudBaseShapeLighting(
                cloudUVW(
                    p,macro.weather,macro.curl,macro.layerHeight,
                    macro.height,macro.toweringStrength,upperBand),
                rejectionThreshold-cloudBillowMaximumOffset(macro.height),
                sampleSpacing,
                macro.height,
                macro.baseNoise);
        }
    }
    return macro;
}
// 高度による雲底側の増密と降水域の増密を一つにまとめ、全ての密度経路で共有する。
float cloudHeightPrecipitationDensityScale(float height,float precipitation){
    return lerp(1.10,0.92,height)*lerp(1.0,1.28,precipitation);
}
// キャッシュを使えない遠距離の光標本を、地点ごとの天候で再構成する。
// 天候、局所列、渦を各地点で再構成し、別地点の雲を光線上へ延長しない。
// 密度と、その地点の層に対応する光学的深さ尺度だけを返し、構造体の寿命を広げない。
float2 sampleCloudFarLightingDensityAndScale(
    float3 p,float weatherCoverage,float sampleSpacing){
    float densityResult=0.0;
    float altitude=cloudAltitude(p);
    bool upperBand=inUpperCloudBandFromAltitude(altitude);
    float layerHeight=heightFractionFromAltitude(
        altitude,upperBand);
    float4 weather=cloudWeatherData(p);
    float columnInterior=cloudWeatherMask(weather,weatherCoverage);
    float toweringStrength=cloudLocalToweringStrength(weather,columnInterior);
    float2 columnMetrics=cloudColumnHeightAndSpan(layerHeight,cloudColumnTopShift(weather,columnInterior,toweringStrength),cloudColumnBaseLift(weather,columnInterior,toweringStrength),toweringStrength,upperBand);
    float sampleHeight=columnMetrics.x;
    float columnSpan=columnMetrics.y;
    float weatherMask=cloudWeatherMaskForLayer(
        weather,weatherCoverage,sampleHeight,toweringStrength);
    if(upperBand) weatherMask*=cloudUpperTerms.x;
    if(weatherMask>0.001){
        float sampledProfile=cloudProfile(sampleHeight,weather.g,toweringStrength,columnSpan,upperBand);
        if(sampledProfile>0.0){
            float rejectionThreshold=cloudPositiveDensityNoiseThreshold();
            float baseNoise=0.0;
            float2 curl=cloudCurlOffset(p);
            cloudBaseShapeLighting(
                cloudUVW(p,weather,curl,layerHeight,sampleHeight,toweringStrength,upperBand),
                rejectionThreshold,sampleSpacing,sampleHeight,baseNoise);
            float baseDensity=cloudNormalizedBaseDensity(baseNoise);
            baseDensity=cloudAnchoredBaseDensity(
                baseDensity,sampleHeight,weatherMask,toweringStrength);
            float dimensionalDensity=cloudDensityFromDimensionalProfile(
                baseDensity,cloudDimensionalProfile(
                    sampledProfile,weatherMask));
            if(dimensionalDensity>0.001){
                densityResult=saturate(
                    dimensionalDensity*cloudHeightPrecipitationDensityScale(
                        sampleHeight,weather.b));
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
        float envelopeNoise=
            macro.baseNoise+cloudBillowMaximumOffset(macro.height);
        // 詳細体積が到達できる最大形状にも完成密度と同じしきい値変換を使う。
        float envelopeBaseDensity=cloudNormalizedBaseDensity(envelopeNoise);
        envelopeBaseDensity=cloudAnchoredBaseDensity(
            envelopeBaseDensity,macro.height,macro.weatherMask,
            macro.toweringStrength);
        shapeResult=cloudDensityFromDimensionalProfile(
            envelopeBaseDensity,
            cloudDimensionalProfile(
                macro.heightProfile,macro.weatherMask));
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
    return shapeResult;
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
// 積分間隔と投影画素幅のうち広い方を使い、画素より細かい形状を遠方へ残さない。
// sampleDistance が 0 の雲内部でも積分間隔を下回らず、距離とともに単調に広がる。
float cloudProjectedSampleSpacing(float integrationSpacing,float sampleDistance,float angularPixelFootprint){
    float projectedPixelWidth=max(sampleDistance,0.0)
                            *max(angularPixelFootprint,0.0);
    return max(max(integrationSpacing,0.0),projectedPixelWidth);
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
// 固定した開始位相0.5は区間中央となり、粗密切り替えでも失われない。
float cloudRefinedSampleT(float intervalStart,float coarseProbeT,float fineStep,float coarseStep,float jitter){
    float coarseCellStart=max(coarseProbeT-coarseStep,intervalStart);
    return coarseCellStart+jitter*fineStep;
}
// 通常描画では各細密区間の採取位置を決定論的な無理数列でずらし、周期形状との共振を防ぐ。
// 参照描画は誤差比較の基準として全区間の中央を採取する。
float cloudRayIntervalPhase(float basePhase,int intervalIndex){
    float samplePhase=0.5;
    if(cloudLightingAmbient.w<0.5){
        samplePhase=frac(basePhase+float(intervalIndex)*0.41421356237);
    }
    return samplePhase;
}
// 高次散乱が周囲の媒質量を判定するための低 LOD 密度を返す。
float cloudLowLodDensityFromPositiveWeatherMacro(CloudMacroSample macro,float weatherMask){
    float densityResult=0.0;
    if(macro.heightProfile>0.0){
        float baseDensity=cloudNormalizedBaseDensity(macro.baseNoise);
        baseDensity=cloudAnchoredBaseDensity(
            baseDensity,macro.height,weatherMask,
            macro.toweringStrength);
        baseDensity=cloudTopReliefDensity(
            baseDensity,macro.height,macro.toweringStrength);
        float dimensionalDensity=cloudDensityFromDimensionalProfile(
            baseDensity,
            cloudDimensionalProfile(macro.heightProfile,weatherMask));
        if(dimensionalDensity>0.001){
            float h=saturate(macro.height);
            dimensionalDensity=cloudInteriorDensityContrast(
                dimensionalDensity,h,weatherMask);
            densityResult=saturate(
                dimensionalDensity
                *cloudHeightPrecipitationDensityScale(h,macro.weather.b));
        }
    }
    return densityResult;
}
// 詳細表示用密度。低周波房、中間房、高周波侵食を別々の採取限界で減衰させる。
float cloudDensityFromPositiveWeatherMacro(float3 p,CloudMacroSample macro,float weatherMask,float billowVisibility,float middleBillowVisibility,float erosionVisibility){
    float densityResult=0.0;
    if(macro.heightProfile>0.0){
        float h=saturate(macro.height);
        float baseDensity=cloudNormalizedBaseDensity(macro.baseNoise);
        baseDensity=cloudAnchoredBaseDensity(
            baseDensity,h,weatherMask,macro.toweringStrength);
        baseDensity=cloudTopReliefDensity(
            baseDensity,h,macro.toweringStrength);
        // 基本形状の外側でも、詳細体積が到達できる範囲だけは密度評価へ進める。
        float envelopeBaseDensity=cloudNormalizedBaseDensity(
            macro.baseNoise+cloudBillowMaximumOffset(h));
        envelopeBaseDensity=cloudAnchoredBaseDensity(
            envelopeBaseDensity,h,weatherMask,macro.toweringStrength);
        float envelopeDensity=cloudDensityFromDimensionalProfile(
            envelopeBaseDensity,
            cloudDimensionalProfile(macro.heightProfile,weatherMask));
        float densityScale=cloudHeightPrecipitationDensityScale(h,macro.weather.b);
        if(envelopeDensity*densityScale>0.001){
            // 詳細房は既存の基本形状の縁だけを変形し、基本形状が空の場所へ
            // 新しい粒を発生させない。包絡判定だけで進めると、雲から離れた房が浮く。
            float edgeBillowSupport=smoothstep(0.08,0.28,baseDensity);
            billowVisibility*=edgeBillowSupport;
            middleBillowVisibility*=edgeBillowSupport;
            // 縦分布と被覆を含む幾何密度を先に確定し、その表面を房変形と侵食で整える。
            // 高さ・降水の光学密度倍率を先に掛けて1へ飽和させると、積乱雲表面の侵食が消える。
            float coarseDensity=cloudDensityFromDimensionalProfile(
                baseDensity,
                cloudDimensionalProfile(macro.heightProfile,weatherMask));
            float d=coarseDensity;
            billowVisibility=saturate(billowVisibility);
            middleBillowVisibility=saturate(middleBillowVisibility);
            erosionVisibility=saturate(erosionVisibility);
            float detailVisibility=max(
                billowVisibility,erosionVisibility);
            [branch] if(detailVisibility>0.001){
                // 基本形状とは別のメートル基準領域を使い、雲塊と細部に同じ模様を出さない。
                float2 detailXz=p.xz-cloudWindWorld()
                               +cloudHeightShapeShear(macro.layerHeight,macro.upperBand>0.5)
                               +macro.curl*35.0;
                float3 detailDomainA,detailDomainB;
                cloudDetailDomains(
                    detailXz,macro.altitude,detailDomainA,detailDomainB);
                float2 ndA=detailNoise.SampleLevel(detailNoise_sampler,detailDomainA+float3(0.19,0.67,0.41)+float3(cloudEvolution.z,cloudEvolution.w,-cloudEvolution.z),0);
                float2 ndB=detailNoise.SampleLevel(detailNoise_sampler,detailDomainB+float3(0.73,0.23,0.59)+float3(-cloudEvolution.w,cloudEvolution.z,cloudEvolution.w),0);
                // 同分布の差なので、領域全体を一方向へ膨張させず、動く房と谷を同時に作る。
                float billowOffset=cloudBillowOffset(
                    ndA,ndB,h,middleBillowVisibility);
                float billowedBaseDensity=cloudNormalizedBaseDensity(
                    macro.baseNoise+billowOffset);
                billowedBaseDensity=cloudAnchoredBaseDensity(
                    billowedBaseDensity,h,weatherMask,
                    macro.toweringStrength);
                float billowedCoarseDensity=cloudDensityFromDimensionalProfile(
                    billowedBaseDensity,
                    cloudDimensionalProfile(macro.heightProfile,weatherMask));
                float billowedDensity=lerp(
                    coarseDensity,billowedCoarseDensity,billowVisibility);
                float detailNear=ndA.g*0.62+ndB.g*0.38;
                float detailFar=ndA.r*0.62+ndB.r*0.38;
                float detail=lerp(
                    detailFar,detailNear,0.90*erosionVisibility);
                float erosion=lerp(0.10,0.24,smoothstep(0.18,0.92,h));
                float eroded=remapc(
                    billowedDensity,detail*erosion,1.0,0.0,1.0);
                // 高周波侵食を0まで許すと、雲体の本体まで細い輪郭線へ分解される。
                // 雲頂では房を残し、雲底では過度な穴を抑える下限を設ける。
                float erosionFloor=lerp(
                    0.42,0.58,smoothstep(0.18,0.92,h));
                eroded=max(eroded,billowedDensity*erosionFloor);
                d=lerp(billowedDensity,eroded,erosionVisibility);
                // 低周波の房を密度へ明確に反映し、均一な灰色の板ではなく
                // 雲内部の明暗と厚みを作る。高周波の穴ではなく広い房だけを使い、
                // 輪郭と採取間隔の安定性は変えない。
                float interiorLobe=lerp(
                    0.82,1.18,smoothstep(0.26,0.74,detailFar));
                float interiorLobeWeight=0.55
                    *smoothstep(0.10,0.86,h)*erosionVisibility;
                d=saturate(d*lerp(1.0,interiorLobe,interiorLobeWeight));
            }
            d=cloudInteriorDensityContrast(d,h,weatherMask);
            // 光学密度の補正は形状処理の後へ掛ける。詳細を無効にした経路は従来値と一致する。
            densityResult=saturate(d*densityScale);
        }
    }
    return densityResult;
}
float cloudDensityFromMacro(float3 p,CloudMacroSample macro,float weatherMask,float billowVisibility,float middleBillowVisibility,float erosionVisibility){
    float densityResult=0.0;
    if(weatherMask>0.001){
        bool upperBand=macro.upperBand>0.5;
        densityResult=cloudDensityFromPositiveWeatherMacro(
            p,macro,weatherMask,
            billowVisibility,middleBillowVisibility,erosionVisibility);
        // 濃さは飽和後に掛け、降水補正で薄い上層が下層相当へ戻ることを防ぐ。
        if(upperBand) densityResult*=cloudUpperTerms.y;
    }
    return densityResult;
}
// 高次散乱の周囲媒質判定用密度を求める。
// 空間スキップ用の広い占有しきい値ではなく、詳細密度と同じ被覆・高さしきい値を使う。
float cloudLowLodDensityFromMacro(CloudMacroSample macro,float weatherMask){
    float densityResult=0.0;
    if(weatherMask>0.001){
        bool upperBand=macro.upperBand>0.5;
        densityResult=cloudLowLodDensityFromPositiveWeatherMacro(
            macro,weatherMask);
        // 上層の作者指定密度を、周囲媒質の判定にも一度だけ反映する。
        if(upperBand) densityResult*=cloudUpperTerms.y;
    }
    return densityResult;
}
float cloudDensity(float3 p, float coverage, float detailWeight){
    float densityResult=0.0;
    CloudMacroSample macro=sampleCloudMacroLighting(p,coverage,0.0);
    densityResult=cloudDensityFromMacro(
        p,macro,macro.densityWeatherMask,
        detailWeight,detailWeight,detailWeight);
    return densityResult;
}

// 影キャッシュの第4標本は高周波侵食を解像できない一方、約0.8～1.8 kmの房は
// まだ完全に解像できる。低詳細度のキャッシュ値へ同じ標本位置の房差分だけを足し、
// 8標本の距離と低周波平均を変えずに雲頂の局所自己影を保つ。
float cloudBillowLightDepthResidual(
    float3 tailOrigin,float lightStep,float coverage,
    float3 sun,float3 lightTangent,float3 lightBitangent,
    float coneSin,float coneCos){
    float residual=0.0;
    float billowVisibility=
        cloudBillowVisibilityFromSampleSpacing(lightStep);
    float middleBillowVisibility=
        cloudMiddleBillowVisibilityFromSampleSpacing(lightStep);
    [branch] if(billowVisibility>0.001){
        float3 coneDir=cloudConeDirection(
            sun,lightTangent,lightBitangent,
            coneSin,coneCos,CLOUD_CONE_GEOMETRY[3]);
        float3 samplePosition=tailOrigin+coneDir*(0.5*lightStep);
        CloudMacroSample macro=
            sampleCloudMacroLighting(samplePosition,coverage,lightStep);
        float lowLodDensity=cloudLowLodDensityFromMacro(
            macro,macro.densityWeatherMask);
        float billowedDensity=cloudDensityFromMacro(
            samplePosition,macro,macro.densityWeatherMask,billowVisibility,
            middleBillowVisibility,0.0);
        residual=(billowedDensity-lowLodDensity)*lightStep
                *cloudOpticalDepthScaleFromBand(
                    macro.upperBand>0.5);
    }
    return residual;
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
float3 cloudShadowWorldPosition(float3 uvw){
    float2 q=shadowGrid.xy
             +float2(uvw.x/max(shadowGrid.z,1e-8),
                     uvw.z/max(shadowGrid.w,1e-8));
    float2 worldXz=q+cloudWindWorld();
    float altitude=lerp(layer.x,layer.y,saturate(uvw.y));
    return cloudShadowWorldPositionAtAltitude(worldXz,altitude);
}

// キャッシュの各画素は、採取間隔に合わせて侵食帯域を制限した近距離3点の後へ対応する。
// l=3..7の低詳細度基準を積分し、第4標本でまだ解像できる房だけは主描画の符号付き差分へ残す。
float traceCloudShadowPattern(
    float3 lp,float patternJitter,float coverage,
    float3 sun,float3 lightTangent,float3 lightBitangent){
    // 光の採取間隔は層厚に応じたCPU計算値を使い、物理消散係数とは分離する。
    float lightStep=cloudCoverageReciprocals.w;
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
            lightMacro,lightMacro.densityWeatherMask);
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
float3 sampleCloudShadowTail(float3 lp){
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
            float4 cached=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,uvw,0);
            bool finiteValue=all(cached==cached)
                          && all(cached>=0.0)
                          && all(cached<65504.0);
            if(finiteValue){
                // yは二つの採取模様の差であり、大きい場所は正確な遠距離積分へ戻す。
                // cached.yは形状密度を光線上へ積分した光学的深さの差である。
                // 現在地点の局所密度は掛けず、信頼度判定では無次元の差として比較する。
                float tauDisagreement=cached.y;
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

// 現在地点から空と地面までの積算密度と、キャッシュ境界の混合率を返す。
// 上下方向はキャッシュの全高度を用いるため、局所密度だけの代替式と異なり頭上の空隙を反映できる。
float3 sampleCloudAmbientDepth(float3 p){
    float3 result=float3(0.0,0.0,0.0);
    if(shadowState.x>0.5){
        float2 q=p.xz-cloudWindWorld();
        float altitude=cloudAltitude(p);
        float h=(altitude-layer.x)/max(layer.y-layer.x,1e-4);
        float2 uvwXz=float2(
            (q.x-shadowGrid.x)*shadowGrid.z,
            (q.y-shadowGrid.y)*shadowGrid.w);
        float2 edgeCells=min(uvwXz,1.0-uvwXz)/shadowState.z;
        float minimumEdgeCells=min(edgeCells.x,edgeCells.y);
        if(h>=0.0&&h<=1.0&&minimumEdgeCells>1.5){
            float borderWeight=smoothstep(1.5,2.5,minimumEdgeCells);
            float4 cached=cloudShadowCache.SampleLevel(
                cloudShadowCache_sampler,float3(uvwXz.x,h,uvwXz.y),0);
            bool finiteValue=all(cached==cached)
                          && all(cached>=0.0)
                          && all(cached<65504.0);
            if(finiteValue){
                result=float3(borderWeight,cached.z,cached.w);
            }
        }
    }
    return result;
}

[numthreads(4,1,4)]
void CSCloudShadow(uint3 tid : SV_DispatchThreadID){
    uint updateStride=max((uint)cloudShadowUpdate.z,1u);
    uint2 outputColumn=uint2(
        tid.x*updateStride+(uint)cloudShadowUpdate.x,
        tid.z*updateStride+(uint)cloudShadowUpdate.y);
    uint width,height,depth;
    cloudShadowOut.GetDimensions(width,height,depth);
    if(any(outputColumn>=uint2(width,depth))
       ||height!=CLOUD_SHADOW_CACHE_HEIGHT) return;
    float3 sun=sunDir.xyz;
    float3 lightTangent=cloudLightTangent.xyz;
    float3 lightBitangent=cloudLightBitangent.xyz;
    float coverage=saturate(params.x);
    float columnSegmentDepth[CLOUD_SHADOW_CACHE_HEIGHT];
    float totalColumnDepth=0.0;
    float cellWorldStep=(layer.y-layer.x)/float(CLOUD_SHADOW_CACHE_HEIGHT);
    float upperColumnDepth=0.0;
    if(cloudUpperLayer.w>0.5){
        // 下層の空側へ届く光は、層間の晴天域を通った後に上層も通過する。
        // 下層キャッシュの32区間とは別に上層の列全体だけを一度積算し、
        // 上層を有効にしたときも環境光の遮蔽を欠落させない。
        float2 columnWorldXz=shadowGrid.xy
            +float2(
                (float(outputColumn.x)+0.5)/max(shadowGrid.z,1e-8),
                (float(outputColumn.y)+0.5)/max(shadowGrid.w,1e-8))
            +cloudWindWorld();
        float upperCellWorldStep=(cloudUpperLayer.y-cloudUpperLayer.x)
            /float(CLOUD_SHADOW_CACHE_HEIGHT);
        [loop] for(uint upperDensityHeightIndex=0u;
                   upperDensityHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;
                   ++upperDensityHeightIndex){
            float upperHeight=(float(upperDensityHeightIndex)+0.5)
                /float(CLOUD_SHADOW_CACHE_HEIGHT);
            float3 upperP=cloudShadowWorldPositionAtAltitude(
                columnWorldXz,
                lerp(cloudUpperLayer.x,cloudUpperLayer.y,upperHeight));
            CloudMacroSample upperMacro=sampleCloudMacroLighting(
                upperP,coverage,upperCellWorldStep);
            float upperDensity=cloudLowLodDensityFromMacro(
                upperMacro,upperMacro.densityWeatherMask);
            upperColumnDepth+=max(upperDensity,0.0)
                *upperCellWorldStep*cloudOpticalDepthScaleFromBand(true);
        }
    }
    // Nubis 2023と同じく、空方向の密度を事前積算する。同じXZ列は高度ごとに一度だけ評価し、
    // 各高度から別々レイマーチする重複を避ける。
    [loop] for(uint densityHeightIndex=0u;densityHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;++densityHeightIndex){
        float3 uvw=(float3(outputColumn.x,densityHeightIndex,outputColumn.y)+0.5)
                  /float3(width,height,depth);
        float3 p=cloudShadowWorldPosition(uvw);
        CloudMacroSample macro=sampleCloudMacroLighting(
            p,coverage,cellWorldStep);
        float columnDensity=cloudLowLodDensityFromMacro(
            macro,macro.densityWeatherMask);
        float segmentDepth=max(columnDensity,0.0)
            *cellWorldStep*cloudOpticalDepthScaleFromBand(false);
        columnSegmentDepth[densityHeightIndex]=segmentDepth;
        totalColumnDepth+=segmentDepth;
    }
    float groundDepth=0.0;
    [loop] for(uint outputHeightIndex=0u;outputHeightIndex<CLOUD_SHADOW_CACHE_HEIGHT;++outputHeightIndex){
        uint3 outputVoxel=uint3(outputColumn.x,outputHeightIndex,outputColumn.y);
        float3 uvw=(float3(outputVoxel)+0.5)/float3(width,height,depth);
        float3 p=cloudShadowWorldPosition(uvw);
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
        float halfSegmentDepth=0.5*columnSegmentDepth[outputHeightIndex];
        float skyDepth=max(
            totalColumnDepth-groundDepth-halfSegmentDepth
                +upperColumnDepth,0.0);
        float sampleGroundDepth=groundDepth+halfSegmentDepth;
        cloudShadowOut[outputVoxel]=float4(
            meanDepth,disagreement,skyDepth,sampleGroundDepth);
        groundDepth+=columnSegmentDepth[outputHeightIndex];
    }
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
    float4 bandIntervals=float4(0,0,0,0);
    int bandCount=intersectCloudBandsFromPosition(rayOrigin,sun,bandIntervals);
    if(sun.y>0.03&&bandCount>0){
        const int SAMPLE_COUNT=32;
        float firstLength=bandIntervals.y-bandIntervals.x;
        float secondLength=bandCount>1
            ?bandIntervals.w-bandIntervals.z:0.0;
        float occupiedLength=max(firstLength+secondLength,1e-5);
        int firstSampleCount=SAMPLE_COUNT;
        if(bandCount>1){
            firstSampleCount=clamp((int)round(float(SAMPLE_COUNT)*firstLength/occupiedLength),1,SAMPLE_COUNT-1);
        }
        [loop] for(int sampleIndex=0;sampleIndex<SAMPLE_COUNT;++sampleIndex){
            bool useSecondBand=sampleIndex>=firstSampleCount;
            int bandSampleIndex=useSecondBand
                ?sampleIndex-firstSampleCount:sampleIndex;
            int bandSampleCount=useSecondBand
                ?SAMPLE_COUNT-firstSampleCount:firstSampleCount;
            float bandStart=useSecondBand
                ?bandIntervals.z:bandIntervals.x;
            float bandLength=useSecondBand?secondLength:firstLength;
            float stepLength=bandLength/float(bandSampleCount);
            float sampleDistance=bandStart
                +(float(bandSampleIndex)+0.5)*stepLength;
            float3 p=rayOrigin+sun*sampleDistance;
            CloudMacroSample macro=sampleCloudMacroLighting(
                p,saturate(params.x),stepLength);
            float sampleDensity=cloudLowLodDensityFromMacro(
                macro,macro.densityWeatherMask)
                *max(params.y,0.0);
            opticalDepth+=sampleDensity*stepLength
                         *cloudOpticalDepthScaleFromBand(
                             macro.upperBand>0.5);
            if(opticalDepth*cloudLightingExtinction.y>=12.0) break;
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
    float4 bandIntervals=float4(0,0,0,0);
    int cloudBandCount=intersectCloudBands(dir,bandIntervals);
    if(cloudBandCount<=0){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // どこまで追うか。遠い雲は 1 画素に何 km も入るので積分が成立せず、描くほど
    // «ちらつく細かいゴミ» になる。打ち切りの手前で薄くして、境界の «壁» を出さない。
    // カメラが雲層内または境界近傍にあるときは、CPUで連続補間した局所視程を使う。
    // 境界から十分離れた地上と上空では w==x のため従来の遠景距離を保つ。
    float MAX_DISTANCE=min(cloudRange.x,max(cloudRange.w,1.0));
    float fadeStartRatio=saturate(cloudRange.y/max(cloudRange.x,1.0));
    float fadeStart=MAX_DISTANCE*fadeStartRatio;
    float intervalStart=bandIntervals.x;
    float intervalEnd=min(bandIntervals.y,MAX_DISTANCE);
    float nextIntervalStart=bandIntervals.z;
    float nextIntervalEnd=min(bandIntervals.w,MAX_DISTANCE);
    bool hasNextInterval=cloudBandCount>1
        &&nextIntervalEnd>nextIntervalStart;
    // 曲面雲層には有限な地平線区間がある。距離減衰はこの入口だけでレイ全体を
    // 棄却せず、後続の各密度標本へ適用する。
    if(intervalEnd<=intervalStart){
        cloudOut[pixelQ]=float4(0,0,0,0);
        cloudDepthOut[pixelQ]=float2(250001.0,0.0);
        return;
    }
    // 隣接する視線積分画素の視線差を一度だけ求め、各採取距離で画素が覆う実幅へ変換する。
    // 縮小描画では縮小画素、時間再構成では実出力画素の幅となるため、再構成方式とも一致する。
    float2 pixelCenter=float2(rayPixel)+0.5;
    float xOffset=rayPixel.x+1u<(uint)rayDimensions.x?1.0:-1.0;
    float yOffset=rayPixel.y+1u<(uint)rayDimensions.y?1.0:-1.0;
    float2 xUv=(pixelCenter+float2(xOffset,0.0))/rayDimensions;
    float2 yUv=(pixelCenter+float2(0.0,yOffset))/rayDimensions;
    float3 xPixelDirection=CloudViewDirection(float2(xUv.x*2.0-1.0,-(xUv.y*2.0-1.0)));
    float3 yPixelDirection=CloudViewDirection(float2(yUv.x*2.0-1.0,-(yUv.y*2.0-1.0)));
    float angularPixelFootprint=max(length(xPixelDirection-dir),length(yPixelDirection-dir));
    // Keep samples in world-distance space.  The previous fixed-count
    // (t1-t0)/N step sampled identical height fractions in every pixel and
    // produced the view-centred starburst visible in the editor.
    // 刻み数。参照描画では大きくする (cloudLightingAmbient.z に入っている)。
    int MAX_STEPS=(int)cloudLightingAmbient.z;
    if(MAX_STEPS<32) MAX_STEPS=32;
    // 晴天の層間距離を標本予算へ含めず、実際に密度を持ち得る二つの区間だけで刻み幅を決める。
    float occupiedSpan=intervalEnd-intervalStart;
    if(hasNextInterval)
        occupiedSpan+=nextIntervalEnd-nextIntervalStart;
    float baseFineStep=cloudCoverageReciprocals.z;
    // 採取上限の 1/8 は空領域から細密領域へ戻る処理に残し、残りを実積分へ使う。
    // 上限を下げた場合も区間終端へ到達し、参照描画では増やした採取回数が刻み幅へ反映される。
    int fineSampleBudget=MAX_STEPS-(MAX_STEPS>>3);
    int coarseSampleBudget=max(fineSampleBudget>>1,1);
    float fineStep=max(baseFineStep,occupiedSpan/float(fineSampleBudget));
    float coarseStep=max(fineStep*2.0,occupiedSpan/float(coarseSampleBudget));
    // 遠くから始まるレイほど刻みを広げる。地平線へ向かうレイは 1 画素の担当する
    // 立体角が広く、細かく刻んでも結果に出ない。上向きのレイは区間入口が近いので
    // ここでは粗くならない。レイごとに一定の倍率にして、粗密の往復を乱さない。
    float distanceLod=1.0+cloudRange.z
        *saturate(intervalStart/max(MAX_DISTANCE,1.0));
    fineStep*=distanceLod;
    coarseStep*=distanceLod;
    // 実レイを持つ画素は形状と不透明度を現在値へ置換し、過去の積分位置を時間平均していない。
    // 毎フレーム乱数位相を変えると未平均の誤差だけが動くため、開始位相を区間中央へ固定する。
    // 通常描画の後続区間は決定論的な無理数列で分散し、周期形状との共振だけを避ける。
    float jit=0.5;
    float3 sun=sunDir.xyz;
    // dir はカメラから雲へ向かう入射レイなので、散乱の出射方向である
    // 雲からカメラへの -dir と、雲から太陽への sun を比較する。
    float cosA=clamp(dot(-dir,sun),-1.0,1.0);
    float phaseBlend=cloudLightingPhase.z;
    // HG は全立体角で積分すると1になる位相関数であり、指向性光の放射照度へそのまま掛ける。
    // Lambert面の1/PIは半球反射用で、体積散乱へ流用すると位相積分が4へ増えてしまう。
    // 前方・後方の混合率も物性値として固定し、視線刻みを変えても位相を変えない。
    float forwardPhase=hg(cosA,cloudLightingPhase.x);
    float backwardPhase=hg(cosA,cloudLightingPhase.y);
    float phase=lerp(backwardPhase,forwardPhase,saturate(phaseBlend));
    phase=clamp(
        phase,cloudLightingMulti.y,cloudLightingMulti.z);
    // 高次ほど方向を失うため、一次散乱とは別の等方寄り位相を使う。
    float phaseMulti=hg(cosA,cloudMultiPhase.x);
    phaseMulti=clamp(
        phaseMulti,cloudLightingMulti.y,cloudLightingMulti.z);
    float multiOcclusion=saturate(cloudLightingMulti.x);
    float multiContribution=min(
        saturate(cloudLightingPhase.w),multiOcclusion);
    float thirdContribution=multiContribution*multiContribution;
    float thirdOcclusion=multiOcclusion*multiOcclusion;
    float secondScatteringToExtinction=multiOcclusion>1e-4
        ?multiContribution/multiOcclusion:0.0;
    float thirdScatteringToExtinction=thirdOcclusion>1e-4
        ?thirdContribution/thirdOcclusion:0.0;
    // 有効な最高次数が消えるまで光路を積分する。一次散乱だけの18という打ち切りを
    // そのまま使うと、弱い消散で進む二次・三次散乱を途中で切ってしまう。
    float lightTerminationOcclusion=1.0;
    if(multiContribution>1e-4)
        lightTerminationOcclusion=multiOcclusion;
    if(thirdContribution>1e-4)
        lightTerminationOcclusion=thirdOcclusion;
    float3 lightTangent=cloudLightTangent.xyz;
    float3 lightBitangent=cloudLightBitangent.xyz;
    float4 coverageTerms=cloudCoverage;
    float transmit=1.0;
    float secondOrderTransmit=1.0;
    float thirdOrderTransmit=1.0;
    float3 scatter=float3(0,0,0);
    float depthMoment=0.0;
    float finePhaseOffset=jit*fineStep;
    // 雲殻内から始まるレイは、最初の粗い区間を飛ばすとカメラ直前の密度を積分できない。
    // 最初の粗い区間だけ細密刻みで確認し、空なら従来の粗い探索へ戻す。
    bool startsInsideShell=intervalStart<=1e-4;
    float initialProbeStep=min(startsInsideShell?fineStep:coarseStep,intervalEnd-intervalStart);
    float t=intervalStart+jit*initialProbeStep;
    bool nearDensity=startsInsideShell;
    float refineUntil=startsInsideShell
        ?min(intervalStart+coarseStep,intervalEnd):intervalStart;
    [loop] for(int i=0;i<MAX_STEPS;i++){
        bool intervalFinished=nearDensity
            ?max(t-finePhaseOffset,intervalStart)>=intervalEnd
            :t>=intervalEnd;
        if(intervalFinished){
            if(!hasNextInterval) break;
            intervalStart=nextIntervalStart;
            intervalEnd=nextIntervalEnd;
            hasNextInterval=false;
            // 薄い上層を粗い採取位相だけで飛ばさないよう、各後続層の先頭を細密区間で確認する。
            float nextProbeStep=min(fineStep,intervalEnd-intervalStart);
            t=intervalStart+jit*nextProbeStep;
            nearDensity=true;
            refineUntil=min(intervalStart+coarseStep,intervalEnd);
        }
        float sampleT=t;
        float stepLength=fineStep;
        if(nearDensity){
            // 採取位相付きの位置から担当区間の始点を戻し、末尾の端数区間も全長を積分する。
            // 端数区間では同じ位相を区間内へ縮め、標本が雲層の外へ出ないようにする。
            float fineCellStart=max(t-finePhaseOffset,intervalStart);
            stepLength=min(fineStep,intervalEnd-fineCellStart);
            float intervalPhase=cloudRayIntervalPhase(jit,i);
            sampleT=fineCellStart+intervalPhase*stepLength;
        }
        // カメラ位置を含む完全な標本位置はワールド座標である。ここでカメラの高さを
        // 再び引くと、Editor のカメラ移動に合わせて雲層まで動いてしまう。
        float3 p=camPos.xyz+dir*sampleT;
        float projectedSampleSpacing=cloudProjectedSampleSpacing(fineStep,sampleT,angularPixelFootprint);
        CloudMacroSample macro=sampleCloudMacro(
            p,coverageTerms,projectedSampleSpacing);
        float shape=cloudShapeFromMacro(macro);
        // 包絡密度へ後段の最大増幅を全て掛けた完成密度上限でだけ空間棄却する。
        // 固定しきい値では、降水・作者密度・上層密度で可視になる薄い雲縁を先に捨ててしまう。
        float shapeDensityUpperBound=shape
            *cloudHeightPrecipitationDensityScale(
                macro.height,macro.weather.b)
            *density;
        if(macro.upperBand>0.5)
            shapeDensityUpperBound*=cloudUpperTerms.y;
        if(shapeDensityUpperBound<=0.0015){
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
            // 粗い刻みの位相を流用せず、細密刻み内の同じ開始位相へ置き直す。
            float coarseProbeT=t;
            refineUntil=min(coarseProbeT+coarseStep,intervalEnd);
            t=cloudRefinedSampleT(intervalStart,coarseProbeT,fineStep,coarseStep,jit);
            nearDensity=true;
            continue;
        }
        nearDensity=true;
        refineUntil=max(refineUntil,min(t+coarseStep,intervalEnd));
        // 積分間隔と現在距離の投影画素幅から、採取可能な房と侵食の帯域を別々に求める。
        // 最後の短い区間でも projectedSampleSpacing は fineStep を下回らず、細部が再出現しない。
        float billowVisibility=cloudBillowVisibilityFromSampleSpacing(projectedSampleSpacing);
        float middleBillowVisibility=cloudMiddleBillowVisibilityFromSampleSpacing(projectedSampleSpacing);
        float erosionVisibility=cloudErosionVisibilityFromSampleSpacing(projectedSampleSpacing);
        float viewWeatherMask=macro.densityWeatherMask;
        float distanceFade=cloudDistanceFade(sampleT,fadeStart,MAX_DISTANCE);
        float dens=cloudDensityFromMacro(
            p,macro,viewWeatherMask,
            billowVisibility,middleBillowVisibility,erosionVisibility)
            *density*distanceFade;
        if(dens>0.0015){
            bool sampleUpperBand=macro.upperBand>0.5;
            float sampleOpticalDepthScale=
                cloudOpticalDepthScaleFromBand(sampleUpperBand);
            float viewSampleOpticalDepth=dens*stepLength
                *sampleOpticalDepthScale*cloudLightingExtinction.x;
            float intervalTransmittance=exp(-viewSampleOpticalDepth);
            float secondIntervalTransmittance=exp(
                -viewSampleOpticalDepth*multiOcclusion);
            float thirdIntervalTransmittance=exp(
                -viewSampleOpticalDepth*thirdOcclusion);
            // 指数的に間隔を広げる採取点で雲層全体を覆い、2 番目の Beer 項で
            // 高次の散乱を近似する。
            // 同じ視線内では黄金比の列で光採取の位相を巡回し、疎な積分誤差を層として揃えない。
            float lightJitter=frac(jit+float(i)*0.61803398875);
            float coneSin,coneCos;
            sincos(6.2831853*lightJitter,coneSin,coneCos);
            float lightDepth=0.0;
            float lightStep=cloudLightStepFromBand(sampleUpperBand);
            lightStep*=lerp(0.72,1.28,lightJitter);
            float3 lp=p;
            float cachedTailForBlend=0.0;
            float cacheBlendWeight=0.0;
            float exactFarStart=0.0;
            bool blendCachedTail=false;
            // 8 個の光円すいは一定の黄金角で回し、上で求めた sin/cos を漸化式で再利用する。
            bool lightTerminated=false;
            // 近距離3点は実際の採取間隔で侵食帯域を減らす。既定層では4点目が最小でも
            // 48 mを越えるため高周波侵食を破棄し、残る低周波の房は後段の差分で保持する。
            [loop] for(int l=0;l<3;l++){
                float2 coneGeometry=CLOUD_CONE_GEOMETRY[l];
                float3 coneDir=cloudConeDirection(
                    sun,lightTangent,lightBitangent,
                    coneSin,coneCos,coneGeometry);
                float3 lightHalfStep=coneDir*(0.5*lightStep);
                lp+=lightHalfStep;
                CloudMacroSample lightMacro=sampleCloudMacroLighting(
                    lp,coverage,lightStep);
                float lightBillowVisibility=cloudBillowVisibilityFromSampleSpacing(lightStep);
                float lightMiddleBillowVisibility=cloudMiddleBillowVisibilityFromSampleSpacing(lightStep);
                float lightErosionVisibility=cloudErosionVisibilityFromSampleSpacing(lightStep);
                float lightDensity=cloudDensityFromMacro(
                    lp,lightMacro,lightMacro.densityWeatherMask,lightBillowVisibility,
                    lightMiddleBillowVisibility,lightErosionVisibility);
                lightDepth+=lightDensity*lightStep
                           *cloudOpticalDepthScaleFromBand(
                               lightMacro.upperBand>0.5);
                // 次区間と影キャッシュは従来と同じ区間終端から始める。
                lp+=lightHalfStep;
                // 直接光と多重散乱の近似値が知覚できない水準まで下がった後は、残りを省略する。
                if(lightDepth*density*cloudLightingExtinction.y
                   *lightTerminationOcclusion>18.0){
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
            // 影キャッシュの最初の低詳細度標本と同じ第4区間だけ、主描画側で房帯域の
            // 差分を求める。標本を追加せず、キャッシュ内の低周波基準へ重ねる。
            if(!lightTerminated){
                lightDepth+=cloudBillowLightDepthResidual(
                    lp,lightStep,coverage,sun,
                    lightTangent,lightBitangent,
                    coneSin,coneCos);
            }
            bool cachedFarTail=false;
            if(!lightTerminated && CLOUD_MAIN_SHADOW_CACHE_ENABLED){
                float3 cachedTailSample=sampleCloudShadowTail(lp);
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
                        lp,coverage,lightStep);
                    lightDepth+=farLightSample.x*lightStep*farLightSample.y;
                    lp+=lightHalfStep;
                    if(lightDepth*density*cloudLightingExtinction.y
                       *lightTerminationOcclusion>18.0) break;
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
            lightDepth=max(lightDepth,0.0);
            // lightDepthは形状密度と距離を既に積分した値なので、ここでは
            // 設定された全体密度と光方向の消散係数だけを一度掛ける。
            // 光線標本の局所密度を再度掛けないため、密度の二重計上にならない。
            float tauL=lightDepth*density*cloudLightingExtinction.y;
            float beer=exp(-tauL);
            // 二次・三次とも、光源側と視線側へ同じ次数の消散縮小率を使う。
            // 光源側だけを弱めて一次の視線重みを再利用すると、厚い雲ほど高次光を失う。
            float secondLightTransmittance=exp(-tauL*multiOcclusion);
            float thirdLightTransmittance=exp(-tauL*thirdOcclusion);
            // 一次散乱は現在の密度標本と区間不透明度で既に制限される。高次散乱は周囲の
            // 散乱源を必要とするため、低 LOD 密度と高さから求める確率をこちらだけへ掛ける。
            float lowLodDensity=cloudLowLodDensityFromMacro(
                macro,viewWeatherMask);
            float inScatterDepthExponent=lerp(
                0.5,2.0,saturate((macro.height-0.30)/0.55));
            float inScatterDepth=saturate(
                0.05+pow(saturate(lowLodDensity),inScatterDepthExponent));
            // 雲底の直上にも厚い雲体があるため、高さだけで高次散乱を抑えない。
            // 高さは密度指数に使い、疎な雲頂縁だけを強く抑える。
            float inScatterProbability=inScatterDepth;
            float inScatterFactor=lerp(
                1.0,inScatterProbability,cloudLightingExtinction.w);
            float h=macro.height;
            // 太陽光は雲へ届く前に大気を通る。低い太陽ほど青が削られて赤くなる。
            float3 sunAtCloud=sunCol.rgb*cloudSunTransmittance.rgb;
            // 有限次数の近似で失う有向光だけを明示的に補う。
            // 位相とアルベドに混ぜないことで、それぞれの正規化と物性を保つ。
            float directionalScatteringScale=cloudLightingExtinction.z
                                               *cloudLightingGround.w;
            float3 singleSunL=sunAtCloud*directionalScatteringScale
                              *beer*phase;
            float3 secondSunL=sunAtCloud*directionalScatteringScale
                               *secondLightTransmittance*phaseMulti
                               *inScatterFactor;
            float3 thirdSunL=sunAtCloud*directionalScatteringScale
                              *thirdLightTransmittance*phaseMulti
                              *inScatterFactor;
            // 環境光は現在点の密度だけではなく、影キャッシュで積算した空・地面方向の密度を使う。
            // キャッシュ外や上層雲では局所密度と境界距離の代替式へ滑らかに戻す。
            float ambientDensityScale=max(density*distanceFade,0.0);
            float ambientLocalDensity=max(
                lowLodDensity*ambientDensityScale,0.0);
            // 空半球の環境光は方向を失っているが、入射光路の消散まで三次散乱の
            // 縮小率で二重に弱めるものではない。二乗は三次散乱の経路専用とし、
            // 環境光の遮蔽は一段分だけ緩和することで、雲中の明暗差を残す。
            float diffuseOcclusion=multiOcclusion;
            float reducedAmbientExtinction=0.60*diffuseOcclusion*cloudLightingExtinction.y;
            float2 fallbackAmbientDepth=
                cloudAmbientFallbackOpticalDepth(macro,ambientLocalDensity);
            float fallbackSkyAmbientDepth=fallbackAmbientDepth.x;
            float fallbackGroundAmbientDepth=fallbackAmbientDepth.y;
            float3 cachedAmbientDepth=sampleCloudAmbientDepth(p);
            float skyAmbientOpticalDepth=lerp(
                fallbackSkyAmbientDepth,
                cachedAmbientDepth.y*ambientDensityScale,
                cachedAmbientDepth.x);
            float groundAmbientOpticalDepth=lerp(
                fallbackGroundAmbientDepth,
                cachedAmbientDepth.z*ambientDensityScale,
                cachedAmbientDepth.x);
            // 環境光はキャッシュで積分した入射光路の消散で減衰させる。
            // 局所密度で再度0へ乗算すると、同じ媒質を二重に遮蔽し、厚い雲の高次散乱が消える。
            float skyAmbientVisibility=
                exp(-reducedAmbientExtinction*skyAmbientOpticalDepth);
            float groundAmbientVisibility=
                exp(-reducedAmbientExtinction*groundAmbientOpticalDepth);
            // 地平色と天頂色を半球積分の二点近似として混ぜる。雲底でも天頂側の空を1/3含め、
            // 雲頂でも地平側を1/3残すことで、一点の灰色や青へ偏らせない。
            float skyAmbientZenithWeight=lerp(0.3333333,0.6666667,saturate(h));
            float3 skyAmbient=cloudSkyZenith.w>0.5
                             ?lerp(skyCol.rgb,cloudSkyZenith.rgb,skyAmbientZenithWeight)
                             :skyCol.rgb;
            float3 ambL=skyAmbient
                       *lerp(cloudLightingAmbient.x,cloudLightingAmbient.y,h)
                       *skyAmbientVisibility*cloudLightingExtinction.z;
            // 地面からの照り返し。雲底ほど強く受ける。0 なら足さない。
            float bottomWeight=1.0-smoothstep(0.15,0.65,h);
            float3 groundL=cloudLightingGround.rgb*cloudLightingMulti.w
                          *bottomWeight*groundAmbientVisibility
                          *cloudLightingExtinction.z;
            // 各次数は縮小後の散乱係数と消散係数で同じ区間を解析積分する。
            // 細分数を変えても均質区間の総寄与が変わらず、厚い雲の奥から届く高次光を失わない。
            float intervalOpacity=1.0-intervalTransmittance;
            float sampleWeight=transmit*intervalOpacity;
            float secondSampleWeight=secondOrderTransmit
                *cloudReducedIntervalScatteringWeight(
                    viewSampleOpticalDepth,secondIntervalTransmittance,
                    multiContribution,multiOcclusion,
                    secondScatteringToExtinction);
            float thirdSampleWeight=thirdOrderTransmit
                *cloudReducedIntervalScatteringWeight(
                    viewSampleOpticalDepth,thirdIntervalTransmittance,
                    thirdContribution,thirdOcclusion,
                    thirdScatteringToExtinction);
            scatter += sampleWeight*(singleSunL+ambL+groundL)
                    +secondSampleWeight*secondSunL
                    +thirdSampleWeight*thirdSunL;
            depthMoment += sampleWeight*sampleT;
            transmit*=intervalTransmittance;
            secondOrderTransmit*=secondIntervalTransmittance;
            thirdOrderTransmit*=thirdIntervalTransmittance;
            float remainingDirectionalWeight=directionalScatteringScale
                *phaseMulti*(secondOrderTransmit*secondScatteringToExtinction+thirdOrderTransmit*thirdScatteringToExtinction);
            if(transmit<0.012 && remainingDirectionalWeight<0.012) break;
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
    // 未採取画素の現在値は別の等倍レイから作った空間再構成なので、固定割合で混ぜると
    // 16フレームの正確な画素履歴を毎フレームぼかす。非剛体な対流変化が実際に進んだ分だけ
    // 現在値へ寄せ、変化が無い場合は次の等倍採取まで画素別履歴をそのまま保つ。
    float temporalCurrentWeight=evolutionMismatch;
    float scheduledCurrentWeight=
        CloudTemporalScheduledCurrentWeight(evolutionMismatch);
    float scaledCurrentWeight=
        CloudTemporalScaledCurrentWeight(evolutionMismatch);
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
        !scheduled && worldOrigin.w>0.5 && evolutionMismatch<0.08;
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
// 再マップ前のPerlin-Worley形状で、雲体の外側を棄却する下端。
constexpr f32 kVolumetricCloudBaseNoiseLower = 0.18f;
// 再マップ前のPerlin-Worley形状で、芯の密度へ到達する上端。
constexpr f32 kVolumetricCloudBaseNoiseUpper = 0.50f;
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
};
static_assert(sizeof(FCloudCb) == 704, "CloudCB must match the HLSL layout");
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
static_assert(
    CBSize<FCloudCb>() == 768u,
    "CloudCB allocation must preserve DX12's 256-byte alignment");

/** 画面描画と環境キューブマップで共有する密度・光採取項。 */
struct FCloudSamplingTerms {
    /** xy=天候しきい値、zw=基本形状雑音の固定正規化範囲。 */
    FVec4 coverage{};

    /** xy=天候遷移幅の逆数、z=視線の細密刻み、w=太陽光の基準刻み。 */
    FVec4 coverageReciprocals{};

    /** xy=上層の被覆と濃さ、z=1 m当たりの基準消散、w=上層の太陽光刻み。 */
    FVec4 upperTerms{};
};

/**
 * 同じ密度シェーダーを使う全経路へ、同一の被覆・採取尺度を渡す。
 *
 * @param safeCoverage 0～1へ正規化済みの被覆。
 * @param horizontalNoiseScale 下層の水平方向ノイズ尺度。
 * @param layerSamplingScale 下層厚を基準幅1.6へ写す採取間隔用の倍率。
 * @param upperLayer 正規化済みの上層設定。
 * @param hasUpperLayer 上層が下層より上で成立しているならtrue。
 * @return 定数バッファーへそのまま設定できる共有項。
 */
FCloudSamplingTerms ResolveVolumetricCloudSamplingTerms_Internal(f32 safeCoverage, f32 horizontalNoiseScale, f32 layerSamplingScale, const FVolumetricCloudUpperLayer& upperLayer, bool hasUpperLayer) noexcept {
    // 呼び出し側が定数バッファーへ複製せず設定できる採取項。
    FCloudSamplingTerms out{};
    // 空領域の早期棄却では、実密度より少し広い範囲を残す。
    const f32 occupancyCoverage = safeCoverage + 0.08f < 1.0f ? safeCoverage + 0.08f : 1.0f;
    // 雲量は天候被覆だけへ適用する。基本形状を雲量でも動かすとdimensional profileとの
    // 二重適用になり、低雲量で雲体が実分布の上端だけへ分断される。
    out.coverage = FVec4{
        0.72f - 0.36f * occupancyCoverage,
        0.72f - 0.36f * safeCoverage,
        kVolumetricCloudBaseNoiseLower,
        kVolumetricCloudBaseNoiseUpper};
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
    // 下層の正規化幅0.0075に相当するワールド空間の太陽光刻み。
    const f32 lightStep = 0.0075f / (layerSamplingScale > 0.0001f ? layerSamplingScale : 0.0001f);
    out.coverageReciprocals = FVec4{1.0f / (occupancyWeatherUpper - out.coverage.x), 1.0f / (densityWeatherUpper - out.coverage.y), fineStep, lightStep};
    // 上層が無効な場合も有限値を維持し、未使用成分へNaNを持ち込まない。
    const f32 upperLayerSamplingScale = hasUpperLayer ? 1.6f / (upperLayer.top_height - upperLayer.base_height) : layerSamplingScale;
    const f32 upperLayerLightStep = 0.0075f / (upperLayerSamplingScale > 0.0001f ? upperLayerSamplingScale : 0.0001f);
    out.upperTerms = FVec4{upperLayer.coverage_scale, upperLayer.density_scale, kVolumetricCloudReferenceExtinctionPerMeter, upperLayerLightStep};
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
        // 一つのスレッドが32高度を書き、同じ縦列の環境光密度を一度だけ積算する。
        const u64 launched = CloudLaunchedThreads2D(shadowCacheUpdateWidth, shadowCacheUpdateDepth, 4u, 4u);
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
    // 大きな板に見える。雲塊の連続性を保てる範囲で尺度を詰め、同じ標本数の
    // まま中規模の房を見せる。
    f32 shapeScale = authoredScale * 0.0022f;
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
    const f32 windOffset = ResolveVolumetricCloudAdvectionDistance(
        safeTime, safeWind);

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
    // 対流位相の前フレーム値を別に保持し、風の平行移動だけでは表せない形状差を
    // 時間再投影側で検出できるようにする。履歴が無いフレームは現在値で初期化する。
    const FVolumetricCloudEvolutionFrameTerms previousEvolutionFrameTerms =
        historyValid && std::isfinite(m_PrevTime)
            ? ResolveVolumetricCloudEvolutionFrameTerms(
                m_PrevTime, m_PrevWindSpeed)
            : evolutionFrameTerms;

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
    const f32 layerSamplingScale = 1.6f / layerThickness;
    cb.layer = FVec4{m_Layer.base_height, m_Layer.top_height,
                     m_Layer.horizontal_noise_scale,
                     kVolumetricCloudReferenceExtinctionPerMeter};
    // 下層と重ならず正の厚さを持つ上層だけを採取対象にする。
    const bool hasUpperLayer =
        m_UpperLayer.top_height > m_UpperLayer.base_height &&
        m_UpperLayer.base_height >= m_Layer.top_height;
    // 画面と環境光で同じ密度形状、物理消散、採取間隔を使う。
    const FCloudSamplingTerms samplingTerms = ResolveVolumetricCloudSamplingTerms_Internal(safeCoverage, m_Layer.horizontal_noise_scale, layerSamplingScale, m_UpperLayer, hasUpperLayer);
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
        cl.Dispatch((updateWidth + 3u) / 4u, 1u, (updateDepth + 3u) / 4u);
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

TResult<TUniquePtr<IRhiTexture>>
CVolumetricClouds::BuildEnvironmentCubemap(
        IRhiDevice& device, IRhiCommandList& cl,
        IRhiTexture& base_environment) noexcept {
    if (!m_Ready || !m_HistoryValid || !m_CloudPipe || !m_Cb
        || !m_ShapeTex || !m_WeatherTex || !m_DetailTex || !m_CurlTex
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
    const f32 layer_thickness =
        m_Layer.top_height - m_Layer.base_height;
    const f32 layer_sampling_scale = 1.6f / layer_thickness;
    cb.layer = FVec4{
        m_Layer.base_height, m_Layer.top_height,
        m_Layer.horizontal_noise_scale,
        kVolumetricCloudReferenceExtinctionPerMeter};
    // 下層と重ならず正の厚さを持つ上層だけを採取対象にする。
    const bool has_upper_layer =
        m_UpperLayer.top_height > m_UpperLayer.base_height
        && m_UpperLayer.base_height >= m_Layer.top_height;
    // 画面と環境光で同じ密度形状、物理消散、採取間隔を使う。
    const FCloudSamplingTerms samplingTerms = ResolveVolumetricCloudSamplingTerms_Internal(safe_coverage, m_Layer.horizontal_noise_scale, layer_sampling_scale, m_UpperLayer, has_upper_layer);
    cb.worldOrigin = FVec4{
        world_origin.x, world_origin.y, world_origin.z, 0.0f};
    const f32 inverse_shadow_extent =
        1.0f / kVolumetricCloudShadowCacheExtent;
    cb.shadowGrid = FVec4{
        m_ShadowGridMinQ.x, m_ShadowGridMinQ.y,
        inverse_shadow_extent, inverse_shadow_extent};
    cb.shadowState = FVec4{
        m_ShadowCacheValid ? 1.0f : 0.0f,
        0.10f,
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheWidth),
        1.0f / static_cast<f32>(kVolumetricCloudShadowCacheHeight)};
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
        cl.SetTexture(1, *m_WeatherTex);
        cl.SetTexture(2, *m_DetailTex);
        cl.SetTexture(3, *m_CurlTex);
        if (m_ShadowTex && m_ShadowCacheValid) {
            cl.SetTexture(4, *m_ShadowTex);
        } else {
            cl.SetTexture(4, *m_ShapeTex);
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
