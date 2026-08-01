// SPDX-License-Identifier: Apache-2.0
// Physical atmospheric scattering 実装
#include "render/Atmosphere.h"
#include "math/Math.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

#include <cmath>
#include <cstring>

namespace acs {

namespace {

/** 地表半径 (m、Earth)。 */
constexpr f32 kGroundRadius     = 6360000.0f;

/** 大気上端半径 (m、地表から 60 km)。 */
constexpr f32 kAtmosphereRadius = 6420000.0f;

/** Rayleigh の scale height (m、8 km)。 */
constexpr f32 kRayleighH        = 8000.0f;

/** Mie の scale height (m、1.2 km)。 */
constexpr f32 kMieH             = 1200.0f;

/**
 * Rayleigh 散乱係数 β (RGB、m⁻¹) を返す。
 *
 * @return 波長別の Rayleigh 散乱係数 (5.802, 13.558, 33.1) ×10⁻⁶ m⁻¹。
 */
inline FVec3 RayleighBeta() noexcept {
    return FVec3{5.802e-6f, 13.558e-6f, 33.1e-6f};
}

/**
 * Mie 散乱係数 β (m⁻¹) を返す。
 *
 * @return Mie 散乱係数 3.996 ×10⁻⁶ m⁻¹。
 */
inline f32  MieBeta() noexcept       { return 3.996e-6f; }

/**
 * Mie 吸収係数 (m⁻¹) を返す。
 *
 * @return Mie 吸収係数 4.4 ×10⁻⁶ m⁻¹。
 */
inline f32  MieAbsorption() noexcept { return 4.4e-6f; }

/**
 * Mie 位相関数の非対称パラメータ g を返す。
 *
 * @return Henyey-Greenstein の g (0.8、前方散乱寄り)。
 */
inline f32  MieG() noexcept          { return 0.8f; }

/**
 * ray-sphere 交差で外側に抜ける距離 t を求める。
 *
 * @details
 * ro は地球中心からのオフセット、rd は単位ベクトル前提。常に大きい方の解
 * (ray が大気圏内にあるとき外側に出る距離) を返す。交差しなければ -1 を返す。
 * @param ro 地球中心を原点とする ray の始点。
 * @param rd 単位長の ray 方向。
 * @param radius 交差を取る球の半径。
 * @return ray 始点から外側交点までの距離 t (交差しなければ -1)。
 */
f32 RaySphereOuter(FVec3 ro, FVec3 rd, f32 radius) noexcept {
    const f32 b = Dot(ro, rd);
    const f32 c = Dot(ro, ro) - radius * radius;
    const f32 disc = b*b - c;
    if (disc < 0) return -1.0f;
    return -b + Sqrt(disc);
}

/** ray が球の手前側へ入る距離。地表へ向く環境光線の終端に使う。 */
f32 RaySphereNear(FVec3 ro, FVec3 rd, f32 radius) noexcept {
    const f32 b = Dot(ro, rd);
    const f32 c = Dot(ro, ro) - radius * radius;
    const f32 disc = b*b - c;
    if (disc < 0.0f) return -1.0f;
    const f32 t = -b - Sqrt(disc);
    return t > 0.0f ? t : -1.0f;
}

/**
 * Henyey-Greenstein 位相関数 (Mie 用)。
 *
 * @param cos_theta 入射と散乱方向のなす角のコサイン。
 * @param g 非対称パラメータ (前方/後方散乱の偏り)。
 * @return 与えた角度での散乱位相値。
 */
ACS_FORCEINLINE f32 PhaseHG(f32 cos_theta, f32 g) noexcept {
    const f32 g2 = g * g;
    const f32 d = 1.0f + g2 - 2.0f * g * cos_theta;
    return (1.0f - g2) / (4.0f * kPi * Pow(d, 1.5f));
}

/**
 * Rayleigh 位相関数。
 *
 * @param cos_theta 入射と散乱方向のなす角のコサイン。
 * @return 与えた角度での Rayleigh 位相値。
 */
ACS_FORCEINLINE f32 PhaseRayleigh(f32 cos_theta) noexcept {
    return (3.0f / (16.0f * kPi)) * (1.0f + cos_theta * cos_theta);
}

/**
 * 高度 h での Rayleigh 密度比 (地表で 1)。
 *
 * @param h 地表を 0 とした高度 (m)。
 * @return 高度 h での Rayleigh 密度比。
 */
ACS_FORCEINLINE f32 DensityRayleigh(f32 h) noexcept { return Exp(-h / kRayleighH); }

/**
 * 高度 h での Mie 密度比 (地表で 1)。
 *
 * @param h 地表を 0 とした高度 (m)。
 * @return 高度 h での Mie 密度比。
 */
ACS_FORCEINLINE f32 DensityMie(f32 h)      noexcept { return Exp(-h / kMieH); }

/**
 * 高度 h でのオゾン密度比 (Hillaire/Bruneton)。25km をピークとするテント (10〜40km)。
 * オゾン層は «吸収のみ»。これが «正しい青» と薄明の色 (sunset の青紫帯) を生む。
 * @param h 地表を 0 とした高度 (m)。
 */
ACS_FORCEINLINE f32 DensityOzone(f32 h) noexcept {
    const f32 km = h * 0.001f;
    const f32 dist = (km >= 25.0f) ? (km - 25.0f) : (25.0f - km);
    const f32 d = 1.0f - dist / 15.0f;
    return d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
}

/** オゾン吸収係数 β (RGB、m⁻¹)。Hillaire 標準 (0.650, 1.881, 0.085)×10⁻⁶。散乱はしない (吸収のみ)。 */
inline FVec3 OzoneAbsorption() noexcept {
    return FVec3{0.650e-6f, 1.881e-6f, 0.085e-6f};
}

/**
 * 点 P から方向 dir に距離 t_max 進んだ点までの透過率 T(P, Q) を計算する。
 *
 * @details 累積光学厚さを steps 段で中点積分し、Rayleigh + Mie 消衰から透過率を求める。
 * @param P_earth_centered 地球中心を原点とする始点 P。
 * @param dir 透過率を測る単位方向。
 * @param t_max P から測る距離 (経路長)。
 * @param steps 光学厚さ積分のサンプル段数。
 * @return RGB 透過率 (t_max<=0 または steps==0 なら (1,1,1))。
 */
FVec3 Transmittance(FVec3 P_earth_centered, FVec3 dir, f32 t_max, u32 steps) noexcept {
    if (t_max <= 0.0f || steps == 0) return FVec3{1, 1, 1};
    const f32 step_len = t_max / static_cast<f32>(steps);
    f32 optical_depth_r = 0;
    f32 optical_depth_m = 0;
    f32 optical_depth_o = 0;   // オゾン
    for (u32 i = 0; i < steps; ++i) {
        const FVec3 sample_pos = P_earth_centered + dir * (step_len * (static_cast<f32>(i) + 0.5f));
        f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) alt = 0;
        optical_depth_r += DensityRayleigh(alt) * step_len;
        optical_depth_m += DensityMie(alt)      * step_len;
        optical_depth_o += DensityOzone(alt)    * step_len;
    }
    const FVec3 beta_r = RayleighBeta();
    const f32  beta_m_ext = MieBeta() + MieAbsorption();
    const FVec3 beta_o = OzoneAbsorption();
    const FVec3 tau{
        beta_r.x * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.x * optical_depth_o,
        beta_r.y * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.y * optical_depth_o,
        beta_r.z * optical_depth_r + beta_m_ext * optical_depth_m + beta_o.z * optical_depth_o,
    };
    return FVec3{Exp(-tau.x), Exp(-tau.y), Exp(-tau.z)};
}

/**
 * view ray に沿って太陽からの単散乱を積算し inscatter 輝度を返す。
 *
 * @details
 * view ray r(t) = ro + rd * t に沿って大気上端まで march し、各 sample で太陽方向
 * への透過率と view 側の累積透過率を掛けて Rayleigh / Mie の in-scatter を積算する。
 * @param ro 地球中心を原点とする view ray の始点。
 * @param rd view ray の単位方向。
 * @param sun_dir 太陽方向の単位ベクトル。
 * @param sun_intensity 太陽のピーク輝度 (RGB)。
 * @param ray_steps view ray 沿いのサンプル段数。
 * @param sun_steps 各 sample から太陽への透過率積分の段数。
 * @return view 方向の単散乱輝度 (RGB)。大気と交差しなければ (0,0,0)。
 */
FVec3 SingleScatter(FVec3 ro, FVec3 rd, FVec3 sun_dir, FVec3 sun_intensity,
                  u32 ray_steps, u32 sun_steps) noexcept {
    // 大気圏外との交点まで march
    f32 t_atm = RaySphereOuter(ro, rd, kAtmosphereRadius);
    if (t_atm <= 0) return FVec3{0, 0, 0};

    const f32 cos_view_sun = Dot(rd, sun_dir);
    const f32 phase_r = PhaseRayleigh(cos_view_sun);
    const f32 phase_m = PhaseHG(cos_view_sun, MieG());

    const f32 step_len = t_atm / static_cast<f32>(ray_steps);
    const FVec3 beta_r = RayleighBeta();
    const f32  beta_m = MieBeta();

    FVec3 inscatter_r{0, 0, 0};
    FVec3 inscatter_m{0, 0, 0};
    // view ray 沿いに伝播した累積光学厚さ (T_view)
    f32 view_od_r = 0, view_od_m = 0, view_od_o = 0;
    const FVec3 beta_o = OzoneAbsorption();

    // 注: ray_steps=32 は十分でリング状バンドはほぼ出ず、出力側 TPDF ディザが 8bit バンドを処理する。
    // ここに per-direction ジッタを入れるとベイク (時間平滑化されない) にグレインが残るため midpoint を維持。
    for (u32 i = 0; i < ray_steps; ++i) {
        const FVec3 sample_pos = ro + rd * (step_len * (static_cast<f32>(i) + 0.5f));
        const f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) {
            // 地面に潜った点、寄与なし
            continue;
        }
        const f32 d_r = DensityRayleigh(alt) * step_len;
        const f32 d_m = DensityMie(alt)      * step_len;
        const f32 d_o = DensityOzone(alt)    * step_len;
        view_od_r += d_r;
        view_od_m += d_m;
        view_od_o += d_o;

        // 太陽光線が sample_pos へ届く透過率
        const f32 t_sun = RaySphereOuter(sample_pos, sun_dir, kAtmosphereRadius);
        if (t_sun <= 0) continue;
        const FVec3 T_sun = Transmittance(sample_pos, sun_dir, t_sun, sun_steps);

        // view side 透過率 (現位置までの累積、オゾン消衰込み)
        const f32 beta_m_ext = beta_m + MieAbsorption();
        const FVec3 tau_view{
            beta_r.x * view_od_r + beta_m_ext * view_od_m + beta_o.x * view_od_o,
            beta_r.y * view_od_r + beta_m_ext * view_od_m + beta_o.y * view_od_o,
            beta_r.z * view_od_r + beta_m_ext * view_od_m + beta_o.z * view_od_o,
        };
        const FVec3 T_view{Exp(-tau_view.x), Exp(-tau_view.y), Exp(-tau_view.z)};

        const FVec3 T_combined{T_sun.x * T_view.x, T_sun.y * T_view.y, T_sun.z * T_view.z};
        inscatter_r = inscatter_r + T_combined * (d_r);
        inscatter_m = inscatter_m + T_combined * (d_m);
    }

    // 多重散乱 (Hillaire 近似): 2 次以降の散乱は方向依存が薄れ «等方» に近い。単散乱の蓄積
    // (inscatter_r/m) を等方位相 1/4π で再評価し ms 強度を掛けて加算 → 空が暗くなりすぎず、
    // 地平線/薄明が自然に満ちる (Rayleigh+Mie+ozone の完全 LUT は将来の GPU 化で)。
    const f32 kIso = 1.0f / (4.0f * kPi);
    const f32 kMs  = 0.18f;                  // 多重散乱の強さ (視覚調整、控えめにして白飛び回避)
    const FVec3 result{
        sun_intensity.x * (beta_r.x * (phase_r + kMs * kIso) * inscatter_r.x + beta_m * (phase_m + kMs * kIso) * inscatter_m.x),
        sun_intensity.y * (beta_r.y * (phase_r + kMs * kIso) * inscatter_r.y + beta_m * (phase_m + kMs * kIso) * inscatter_m.y),
        sun_intensity.z * (beta_r.z * (phase_r + kMs * kIso) * inscatter_r.z + beta_m * (phase_m + kMs * kIso) * inscatter_m.z),
    };
    return result;
}

/**
 * 地表球へ当たる下半球 ray を Lambert 地表 + view-path haze として評価する。
 * 真下は地表反射、地平線へ近づくほど長い大気経路の散乱へ連続的に移る。
 */
FVec3 GroundHemisphere(FVec3 viewer, FVec3 view_dir, FVec3 sun_dir,
                       FVec3 sun_intensity, FVec3 ground_albedo, u32 ray_steps,
                       u32 sun_steps) noexcept {
    const f32 t_ground = RaySphereNear(viewer, view_dir, kGroundRadius);
    if (t_ground <= 0.0f) {
        return SingleScatter(viewer, view_dir, sun_dir, sun_intensity,
                             ray_steps, sun_steps);
    }

    const FVec3 ground_point = viewer + view_dir * t_ground;
    const f32 ground_len = Sqrt(Dot(ground_point, ground_point));
    const FVec3 ground_normal =
        ground_len > 1.0f ? ground_point * (1.0f / ground_len) : FVec3{0, 1, 0};
    const FVec3 surface_origin = ground_point + ground_normal * 1.0f;
    const FVec3 view_t =
        Transmittance(viewer, view_dir, t_ground, ray_steps);

    FVec3 direct{0, 0, 0};
    const f32 n_dot_l = Dot(ground_normal, sun_dir);
    if (n_dot_l > 0.0f) {
        const f32 t_sun =
            RaySphereOuter(surface_origin, sun_dir, kAtmosphereRadius);
        const FVec3 sun_t =
            Transmittance(surface_origin, sun_dir, t_sun, sun_steps);
        const f32 lambert = n_dot_l / kPi;
        direct = FVec3{
            sun_intensity.x * sun_t.x * lambert,
            sun_intensity.y * sun_t.y * lambert,
            sun_intensity.z * sun_t.z * lambert,
        };
    }

    // A near-horizontal sky ray supplies low-frequency skylight and the
    // asymptotic haze colour. It matters most near the geometric horizon;
    // for a downward ray the short view path leaves the ground term dominant.
    const f32 xz_len =
        Sqrt(view_dir.x * view_dir.x + view_dir.z * view_dir.z);
    FVec3 horizon_dir =
        xz_len > 1e-5f
            ? FVec3{view_dir.x / xz_len, 0.002f, view_dir.z / xz_len}
            : FVec3{0.0f, 0.002f, 1.0f};
    const f32 horizon_len = Sqrt(Dot(horizon_dir, horizon_dir));
    horizon_dir = horizon_dir * (1.0f / horizon_len);
    const FVec3 horizon =
        SingleScatter(viewer, horizon_dir, sun_dir, sun_intensity,
                      ray_steps, sun_steps);

    ground_albedo = FVec3{
        ground_albedo.x > 0.0f ? ground_albedo.x : 0.0f,
        ground_albedo.y > 0.0f ? ground_albedo.y : 0.0f,
        ground_albedo.z > 0.0f ? ground_albedo.z : 0.0f,
    };
    const FVec3 ground_radiance{
        ground_albedo.x * (direct.x + horizon.x * 0.35f),
        ground_albedo.y * (direct.y + horizon.y * 0.35f),
        ground_albedo.z * (direct.z + horizon.z * 0.35f),
    };
    return FVec3{
        ground_radiance.x * view_t.x + horizon.x * (1.0f - view_t.x),
        ground_radiance.y * view_t.y + horizon.y * (1.0f - view_t.y),
        ground_radiance.z * view_t.z + horizon.z * (1.0f - view_t.z),
    };
}

} // namespace

/** equirect 画像の各方向で単散乱を評価し RGBA float 配列を焼く。 */
TArray<f32> CAtmosphere::BakeEquirect(u32 width, u32 height,
                                     const FAtmosphereParams& params) noexcept {
    TArray<f32> out;
    out.Resize(static_cast<usize>(width) * height * 4u);

    FVec3 sun_dir;
    {
        f32 l2 = params.sun_dir.x*params.sun_dir.x
               + params.sun_dir.y*params.sun_dir.y
               + params.sun_dir.z*params.sun_dir.z;
        if (l2 < 1e-12f) sun_dir = FVec3{0, 1, 0};
        else {
            f32 inv = 1.0f / Sqrt(l2);
            sun_dir = FVec3{params.sun_dir.x*inv, params.sun_dir.y*inv, params.sun_dir.z*inv};
        }
    }
    // 観察者位置: 地表 + 数 m (camera 高度的なもの)。地球中心が原点なので
    // (0, ground_radius + 2, 0) に置く。
    const FVec3 viewer{0, kGroundRadius + 2.0f, 0};
    const u32 ray_steps = params.ray_steps > 0u ? params.ray_steps : 1u;
    const u32 sun_steps = params.sun_steps > 0u ? params.sun_steps : 1u;

    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi;
        const f32 sin_t = Sin(theta);
        const f32 cos_t = Cos(theta);
        for (u32 x = 0; x < width; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 phi = phi_norm * 2.0f * kPi - kPi;
            // equirect 規約: theta=0 が +Y、phi=0 が +Z
            const FVec3 view_dir{sin_t * Sin(phi), cos_t, sin_t * Cos(phi)};

            FVec3 col;
            if (RaySphereNear(viewer, view_dir, kGroundRadius) > 0.0f) {
                col = GroundHemisphere(viewer, view_dir, sun_dir,
                                       params.sun_intensity,
                                       params.ground_albedo,
                                       ray_steps, sun_steps);
            } else {
                col = SingleScatter(viewer, view_dir, sun_dir, params.sun_intensity,
                                    ray_steps, sun_steps);
            }

            const u32 idx = (y * width + x) * 4u;
            out[idx + 0] = col.x;
            out[idx + 1] = col.y;
            out[idx + 2] = col.z;
            out[idx + 3] = 1.0f;
        }
    }
    return out;
}

// ===================== GPU Hillaire 大気 (CSkyAtmosphere) =====================
namespace {

/** AtmoCB レイアウト (HLSL の cbuffer AtmoCB と一致)。 */
struct FAtmoCB {
    FVec4 sunDir;
    FVec4 sunInt;
    FVec4 groundAlbedo;
};
static_assert(sizeof(FAtmoCB) == 48u,
              "AtmoCB must match the HLSL constant-buffer layout");

/** ApCB レイアウト (HLSL の cbuffer ApCB と一致)。 */
struct FApCB {
    FMat4 invViewProj;
    FVec4 camPos;
    FVec4 sunDir;
    FVec4 sunInt;
    FVec4 apParams;
    FVec4 fogColorDensity;
    FVec4 fogParams;
};

/** Fullscreen AP composite の b0。 */
struct FApCompositeCB {
    FMat4 invViewProj;
    FVec4 camPosMaxDist;
    FVec4 compositeParams; // x=0: geometry depth, x=1: cleared-depth far slice
};

ACS_FORCEINLINE f32 FiniteOr(f32 value, f32 fallback) noexcept {
    if (!std::isfinite(static_cast<double>(value))) return fallback;
    // Canonicalize signed zero so exact cache comparison does not turn a
    // numerically identical camera state into a spurious GPU dispatch.
    return value == 0.0f ? 0.0f : value;
}

FVec3 SanitizeVec3(FVec3 value, FVec3 fallback) noexcept {
    return FVec3{
        FiniteOr(value.x, fallback.x),
        FiniteOr(value.y, fallback.y),
        FiniteOr(value.z, fallback.z),
    };
}

FMat4 SanitizeMatrix(const FMat4& value) noexcept {
    FMat4 result = FMat4::Identity();
    for (u32 row = 0; row < 4u; ++row) {
        for (u32 column = 0; column < 4u; ++column) {
            result.m[row][column] =
                FiniteOr(value.m[row][column], result.m[row][column]);
        }
    }
    return result;
}

/** Camera-volume froxel。画面 48²、深度 96 slice で 250 km の雲 range まで解像する。 */
constexpr u32 kApXYRes = kSkyAtmosphereFroxelXyResolution;
constexpr u32 kApZRes  = kSkyAtmosphereFroxelZResolution;

// 共通 HLSL (km 単位、Hillaire/Bruneton Earth)。各 CS に inline する。
#define ATMO_COMMON_HLSL \
"static const float PI = 3.14159265;\n" \
"static const float kBottom = 6360.0;\n" \
"static const float kTop    = 6460.0;\n" \
"static const float3 kRayS  = float3(5.802, 13.558, 33.1) * 0.001;\n" \
"static const float  kRayH  = 8.0;\n" \
"static const float  kMieS  = 3.996 * 0.001;\n" \
"static const float  kMieE  = 4.440 * 0.001;\n" \
"static const float  kMieH  = 1.2;\n" \
"static const float  kMieG  = 0.8;\n" \
"static const float3 kOzoneA = float3(0.650, 1.881, 0.085) * 0.001;\n" \
"void SampleMedium(float altKm, out float3 sR, out float sM, out float3 ext){\n" \
"  float dR=exp(-altKm/kRayH); float dM=exp(-altKm/kMieH);\n" \
"  float dO=saturate(1.0-abs(altKm-25.0)/15.0);\n" \
"  sR=kRayS*dR; sM=kMieS*dM; ext=sR + (kMieE*dM) + (kOzoneA*dO);\n" \
"}\n" \
"float RayleighPhase(float c){ return 3.0/(16.0*PI)*(1.0+c*c); }\n" \
"float HgPhase(float c,float g){ float g2=g*g; float d=1.0+g2-2.0*g*c; return (1.0-g2)/(4.0*PI*max(pow(max(d,1e-4),1.5),1e-6)); }\n" \
"float RaySphere(float3 ro,float3 rd,float r){ float result=-1.0; float b=dot(ro,rd); float c=dot(ro,ro)-r*r; float disc=b*b-c; if(disc>=0.0){ result=-b+sqrt(disc); } return result; }\n" \
"float2 TransParamsToUv(float r,float mu){ float H=sqrt(max(kTop*kTop-kBottom*kBottom,0.0)); float rho=sqrt(max(r*r-kBottom*kBottom,0.0)); float disc=r*r*(mu*mu-1.0)+kTop*kTop; float d=max(0.0,-r*mu+sqrt(max(disc,0.0))); float dMin=kTop-r; float dMax=rho+H; float xMu=(dMax>dMin)?(d-dMin)/(dMax-dMin):0.0; float xR=(H>0.0)?rho/H:0.0; return float2(xMu,xR); }\n" \
"void TransUvToParams(float2 uv,out float r,out float mu){ float H=sqrt(max(kTop*kTop-kBottom*kBottom,0.0)); float rho=H*uv.y; r=sqrt(max(rho*rho+kBottom*kBottom,0.0)); float dMin=kTop-r; float dMax=rho+H; float d=dMin+uv.x*(dMax-dMin); mu=(d<=0.0)?1.0:(H*H-rho*rho-d*d)/(2.0*r*d); mu=clamp(mu,-1.0,1.0); }\n"

// Transmittance LUT (256x64)。各 texel = (viewHeight, cosZenith) → 大気上端への透過率。
const char* kTransCS =
ATMO_COMMON_HLSL
"RWTexture2D<float4> transOut : register(u0);\n"
"[numthreads(8,8,1)]\n"
"void CSTrans(uint3 id : SV_DispatchThreadID){\n"
"  const uint W=256,H=64; if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float r,mu; TransUvToParams(uv,r,mu);\n"
"  float3 P=float3(0,r,0); float3 dir=float3(sqrt(saturate(1.0-mu*mu)),mu,0);\n"
"  float tTop=RaySphere(P,dir,kTop); if(tTop<=0){ transOut[id.xy]=float4(1,1,1,1); return; }\n"
"  const int N=40; float dt=tTop/N; float3 tau=0;\n"
"  [loop] for(int i=0;i<N;i++){ float3 sp=P+dir*(dt*(i+0.5)); float alt=length(sp)-kBottom; float3 sR; float sM; float3 ext; SampleMedium(max(alt,0.0),sR,sM,ext); tau+=ext*dt; }\n"
"  transOut[id.xy]=float4(exp(-tau),1.0);\n"
"}\n";

// Multi-scattering LUT (32x32)。WickedEngine skyAtmosphere_multiScatteredLuminanceLutCS の忠実移植。
// texel=(cosSunZenith=uv.x*2-1, viewHeight=bottom+uv.y*(top-bottom))。64 方向 (8x8) を
// azimuth × uniform-cos-polar で等立体角サンプルし、各方向を march して
// «太陽からの 2 次 in-scatter L» と «等方 transfer multiScatAs1» を蓄積。sphereSolidAngle/64
// × isotropicPhase(1/4π) = 1/64 平均。Fms = (L/64)/(1-(multiScatAs1/64)) で無限多重散乱を幾何級数和。
// ★前回失敗の正規化ミス (球平均/Tsun 二重掛け) を排除し WE と厳密一致。
const char* kMultiCS =
ATMO_COMMON_HLSL
"Texture2D<float4> transLut : register(t0);\n"
"RWTexture2D<float4> msOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){\n"
"  float2 p=saturate(TransParamsToUv(r,mu))*float2(255.0,63.0);\n"
"  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);\n"
"  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"float RaySphereNear(float3 ro,float3 rd,float rad){ float result=-1.0; float b=dot(ro,rd); float c=dot(ro,ro)-rad*rad; float disc=b*b-c; if(disc>=0.0){ result=-b-sqrt(disc); } return result; }\n"
"[numthreads(8,8,1)]\n"
"void CSMulti(uint3 id : SV_DispatchThreadID){\n"
"  const uint W=32,H=32; if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float cosSun=uv.x*2.0-1.0;\n"
"  float r=kBottom + uv.y*(kTop-kBottom);\n"
"  float3 P0=float3(0.0,r,0.0);\n"
"  float3 sun=float3(sqrt(saturate(1.0-cosSun*cosSun)), cosSun, 0.0);\n"
"  float3 Lsum=float3(0,0,0); float3 MSsum=float3(0,0,0);\n"
"  const uint SQ=8u;\n"
"  [loop] for(uint s=0u;s<64u;s++){\n"
  "    float u=(float(s%SQ)+0.5)/float(SQ); float v=(float(s/SQ)+0.5)/float(SQ);\n"
  "    float azimuth=2.0*PI*u; float cosPolar=1.0-2.0*v;\n"
  "    float sinPolar=sqrt(saturate(1.0-cosPolar*cosPolar));\n"
  "    float3 dir=float3(cos(azimuth)*sinPolar, sin(azimuth)*sinPolar, cosPolar);\n"
"    float tTop=RaySphere(P0,dir,kTop); float tGround=RaySphereNear(P0,dir,kBottom);\n"
"    float tMax=tTop; bool hitGround=false; if(tGround>0.0 && tGround<tMax){ tMax=tGround; hitGround=true; }\n"
"    if(tMax<=0.0) continue;\n"
"    const int N=20; float dt=tMax/float(N);\n"
"    float cosVS=dot(dir,sun); float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"    float3 L=float3(0,0,0); float3 ms=float3(0,0,0); float3 Tput=float3(1,1,1);\n"
"    [loop] for(int i=0;i<N;i++){\n"
"      float3 P=P0+dir*(dt*(float(i)+0.5)); float rr=length(P); float alt=rr-kBottom; if(alt<0.0) break;\n"
"      float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext); float3 scat=sR+sM;\n"
"      float muSun=dot(P/rr,sun); float3 Tsun=SampleTrans(rr,muSun);\n"
"      float3 sampleT=exp(-ext*dt);\n"
"      float3 S=Tsun*(sR*phR+sM*phM);\n"                  // globalL=1, MS=0 (1次のみ)
"      float3 Sint=(S - S*sampleT)/max(ext,1e-7); L+=Tput*Sint;\n"
"      float3 MSc=scat;\n"                                // 等方 transfer (位相/太陽なし)
"      float3 MSint=(MSc - MSc*sampleT)/max(ext,1e-7); ms+=Tput*MSint;\n"
"      Tput*=sampleT;\n"
"    }\n"
"    if(hitGround){ float3 Pg=P0+dir*tMax; float rg=length(Pg); float3 ng=Pg/rg; float muG=dot(ng,sun);\n"
"      if(muG>0.0){ float3 TsunG=SampleTrans(rg,muG); L += Tput * TsunG * float3(0.3,0.3,0.3) * (muG/PI); } }\n"
"    Lsum+=L; MSsum+=ms;\n"
"  }\n"
"  float3 InScat=Lsum/64.0; float3 MultiScatAs1=MSsum/64.0;\n"
"  float3 Fms=InScat/max(float3(1,1,1)-MultiScatAs1, 1e-4);\n"
"  msOut[id.xy]=float4(Fms,1.0);\n"
"}\n";

// Equirect bake。view dir 毎に Rayleigh+Mie+ozone 単散乱 + 多重散乱 LUT を積分。Transmittance LUT で太陽透過率を引く。
const char* kBakeCS =
ATMO_COMMON_HLSL
"cbuffer AtmoCB : register(b0){ float4 sunDir; float4 sunInt; float4 groundAlbedo; };\n"
"Texture2D<float4> transLut : register(t0);\n"
"Texture2D<float4> multiLut : register(t1);\n"
"RWTexture2D<float4> bakeOut : register(u0);\n"
"float3 SampleTrans(float r,float mu){\n"
"  float2 p=saturate(TransParamsToUv(r,mu))*float2(255.0,63.0);\n"
"  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);\n"
"  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"float3 SampleMulti(float r,float mu){\n"
"  float2 uv=float2(mu*0.5+0.5,saturate((r-kBottom)/(kTop-kBottom)));\n"
"  float2 p=saturate(uv)*31.0; int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(31,31)); float2 f=frac(p);\n"
"  float3 a=lerp(multiLut.Load(int3(p0.x,p0.y,0)).rgb,multiLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);\n"
"  float3 b=lerp(multiLut.Load(int3(p0.x,p1.y,0)).rgb,multiLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);\n"
"  return lerp(a,b,f.y);\n"
"}\n"
"float RaySphereNearGround(float3 ro,float3 rd,float rad){\n"
"  float result=-1.0; float b=dot(ro,rd); float c=dot(ro,ro)-rad*rad; float disc=b*b-c;\n"
"  if(disc>=0.0){ float t=-b-sqrt(disc); if(t>0.0){ result=t; } } return result;\n"
"}\n"
"[numthreads(8,8,1)]\n"
"void CSBake(uint3 id : SV_DispatchThreadID){\n"
"  uint W,H; bakeOut.GetDimensions(W,H); if(id.x>=W||id.y>=H) return;\n"
"  float2 uv=(float2(id.xy)+0.5)/float2(W,H);\n"
"  float theta=uv.y*PI; float phi=uv.x*2.0*PI-PI; float st=sin(theta),ct=cos(theta);\n"
"  float3 dir=float3(st*sin(phi),ct,st*cos(phi)); float3 sd=normalize(sunDir.xyz);\n"
"  float3 P0=float3(0,kBottom+0.005,0); float tAtm=RaySphere(P0,dir,kTop);\n"
"  float tGround=RaySphereNearGround(P0,dir,kBottom);\n"
"  bool hitGround=tGround>0.0 && tGround<tAtm; float tMax=hitGround?tGround:tAtm;\n"
"  float3 col=float3(0,0,0);\n"
"  if(tMax>0.0){\n"
"    const int N=32; float dt=tMax/N; float cosVS=dot(dir,sd);\n"
"    float phR=RayleighPhase(cosVS); float phM=HgPhase(cosVS,kMieG);\n"
"    float3 L=0; float3 Tview=float3(1,1,1);\n"
"    [loop] for(int i=0;i<N;i++){\n"
"      float3 P=P0+dir*(dt*(i+0.5)); float r=length(P); float alt=r-kBottom; if(alt<0) break;\n"
"      float3 sR; float sM; float3 ext; SampleMedium(alt,sR,sM,ext);\n"
"      float muSun=dot(P/r,sd); float3 Tsun=SampleTrans(r,muSun);\n"
"      float3 MS=SampleMulti(r,muSun);\n"                 // 多重散乱 LUT (WE GetMultipleScattering)
"      float3 Sdir=sR*phR + sM*phM;\n"
"      float3 sampleScatter=Sdir*Tsun + MS*(sR+sM);\n"    // WE: S=Tsun*phaseScat + MS*scattering
"      float3 sampleT=exp(-ext*dt);\n"
"      float3 Sint=(sampleScatter - sampleScatter*sampleT)/max(ext,1e-7);\n"
"      L+=Tview*Sint; Tview*=sampleT;\n"
"    }\n"
"    if(hitGround){\n"
"      float3 Pg=P0+dir*tGround; float rg=max(length(Pg),kBottom); float3 ng=Pg/rg;\n"
"      float muG=max(dot(ng,sd),0.0); float3 TsunG=muG>0.0?SampleTrans(rg,muG):float3(0,0,0);\n"
"      float3 skyIrradiance=max(SampleMulti(rg,muG),0.0)*0.25;\n"
"      float3 groundUnit=max(groundAlbedo.xyz,0.0)*(TsunG*(muG/PI)+skyIrradiance);\n"
"      L+=Tview*groundUnit;\n"
"    }\n"
"    col=L*sunInt.xyz;\n"
"  }\n"
"  col=max(col,0.0);\n"
"  bakeOut[id.xy]=float4(col,1.0);\n"
"}\n";

// aerial perspective + local height fog の camera-volume LUT。
// 48x48x96 froxel、24-step stratified integration。大気 LUT は bilinear 参照し、最近傍由来の
// バンディングを除去する。物理 AP は premultiplied in-scatter と RGB transmittance を別UAVへ、
// local fog は従来の scalar opacity 付き volume へ書く。
const char* kApCS =
ATMO_COMMON_HLSL
R"(
#pragma pack_matrix(row_major)
cbuffer ApCB : register(b0) {
  float4x4 invViewProj;
  float4 camPos;
  float4 sunDir;
  float4 sunInt;
  float4 apParams;          // x=シーン→km係数, y=カメラ高度(km), z=最大距離
  float4 fogColorDensity;   // xyz=散乱アルベド, w=シーン単位あたりの消散係数
  float4 fogParams;         // x=高度減衰, y=基準高度, z=HG g, w=太陽散乱
};
Texture2D<float4> transLut : register(t0);
Texture2D<float4> multiLut : register(t1);
RWTexture3D<float4> apOut : register(u0);
RWTexture3D<float4> apTransOut : register(u1);

float3 LoadTransBilinear(float2 uv) {
  float2 p=saturate(uv)*float2(255.0,63.0);
  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(255,63)); float2 f=frac(p);
  float3 a=lerp(transLut.Load(int3(p0.x,p0.y,0)).rgb,
                transLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);
  float3 b=lerp(transLut.Load(int3(p0.x,p1.y,0)).rgb,
                transLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);
  return lerp(a,b,f.y);
}
float3 LoadMultiBilinear(float2 uv) {
  float2 p=saturate(uv)*float2(31.0,31.0);
  int2 p0=int2(floor(p)); int2 p1=min(p0+1,int2(31,31)); float2 f=frac(p);
  float3 a=lerp(multiLut.Load(int3(p0.x,p0.y,0)).rgb,
                multiLut.Load(int3(p1.x,p0.y,0)).rgb,f.x);
  float3 b=lerp(multiLut.Load(int3(p0.x,p1.y,0)).rgb,
                multiLut.Load(int3(p1.x,p1.y,0)).rgb,f.x);
  return lerp(a,b,f.y);
}
float3 SampleTrans(float r,float mu) {
  return LoadTransBilinear(TransParamsToUv(r,mu));
}
float3 SampleMulti(float r,float mu) {
  return LoadMultiBilinear(float2(mu*0.5+0.5,saturate((r-kBottom)/(kTop-kBottom))));
}
float Hash13(uint3 p) {
  return frac(sin(dot(float3(p),float3(12.9898,78.233,37.719)))*43758.5453);
}
float FogPhase(float cosTheta,float g) {
  g=clamp(g,-0.85,0.85); float g2=g*g;
  float d=max(1.0+g2-2.0*g*cosTheta,1e-3);
  return (1.0-g2)/(d*sqrt(d));
}

void IntegrateAp(uint3 id,uint W,uint H,uint D,
                 out float3 L,out float3 Tview) {
  float2 uv=(float2(id.xy)+0.5)/float2(W,H);
  float sliceN=(float(id.z)+0.5)/float(D);
  float tScene=sliceN*sliceN*apParams.z;                   // 近傍を密にする深度分布
  float4 clip=float4(uv.x*2.0-1.0,-(uv.y*2.0-1.0),1.0,1.0);
  float4 wp=mul(clip,invViewProj);
  float invW=(abs(wp.w)>1e-6)?rcp(wp.w):0.0;
  float3 dir=normalize(wp.xyz*invW-camPos.xyz);
  float3 P0=float3(0.0,kBottom+apParams.y,0.0);
  float3 sd=normalize(sunDir.xyz);
  float cosVS=dot(dir,sd);
  float phR=RayleighPhase(cosVS), phM=HgPhase(cosVS,kMieG);
  float fogPhase=FogPhase(cosVS,fogParams.z);
  float sunLum=max(dot(max(sunInt.xyz,0.0),float3(0.2126,0.7152,0.0722)),1e-4);
  float3 sunTint=max(sunInt.xyz,0.0)/sunLum;

  const int N=24;
  float dtScene=tScene/float(N);
  float jitter=0.15+0.70*Hash13(id);                       // 時間方向にちらつかない安定した層化
  L=float3(0,0,0); Tview=float3(1,1,1);
  [loop] for(int i=0;i<N;i++) {
    float t=dtScene*(float(i)+jitter);
    float3 P=P0+dir*(t*apParams.x);
    float r=length(P), alt=max(r-kBottom,0.0);
    float3 sR; float sM; float3 extKm; SampleMedium(alt,sR,sM,extKm);
    float muSun=dot(P/max(r,1e-5),sd);
    float3 Tsun=SampleTrans(r,muSun), MS=SampleMulti(r,muSun);
    float3 atmoScatterKm=(sR*phR+sM*phM)*Tsun+MS*(sR+sM);

    float worldY=camPos.y+dir.y*t;
    float fogD=fogColorDensity.w
              * exp(-min(max(fogParams.x,0.0)*max(worldY-fogParams.y,0.0),80.0));
    float3 fogLight=fogColorDensity.xyz
                  * (1.0+sunTint*(fogPhase*max(fogParams.w,0.0)));

    // scene-unit basis へ揃えて大気と local fog を同一 Beer-Lambert step で積分。
    float3 totalExt=extKm*apParams.x+fogD;
    float3 totalScatter=atmoScatterKm*sunInt.xyz*apParams.x+fogLight*fogD;
    float3 sampleT=exp(-min(totalExt*dtScene,80.0));
    float3 Sint=totalScatter*(1.0-sampleT)/max(totalExt,1e-7);
    L+=Tview*Sint;
    Tview*=sampleT;
  }
}
[numthreads(4,4,4)]
void CSAp(uint3 id : SV_DispatchThreadID) {
  uint W,H,D; apOut.GetDimensions(W,H,D); if(id.x>=W||id.y>=H||id.z>=D) return;
  float3 L,Tview; IntegrateAp(id,W,H,D,L,Tview);
  float meanT=dot(Tview,float3(1.0/3.0,1.0/3.0,1.0/3.0));
  apOut[id]=float4(L,saturate(1.0-meanT));
  apTransOut[id]=float4(saturate(Tview),1.0);
}
[numthreads(4,4,4)]
void CSLocalFog(uint3 id : SV_DispatchThreadID) {
  uint W,H,D; apOut.GetDimensions(W,H,D); if(id.x>=W||id.y>=H||id.z>=D) return;
  float3 L,Tview; IntegrateAp(id,W,H,D,L,Tview);
  float meanT=dot(Tview,float3(1.0/3.0,1.0/3.0,1.0/3.0));
  apOut[id]=float4(L,saturate(1.0-meanT));
}
)";

// Camera-volume を scene depth で終端して HDR scene へ適用する fullscreen pass。
// physical AP は RGB multiply + premultiplied additive、local fog は scalar alpha blend。
const char* kApCompositeVS = R"(
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  VSOut o;
  o.uv = uv;
  o.pos = float4(uv.x * 2.0 - 1.0, -(uv.y * 2.0 - 1.0), 0.0, 1.0);
  return o;
}
)";

const char* kApCompositePS = R"(
#pragma pack_matrix(row_major)
cbuffer ApCompositeCB : register(b0) {
  float4x4 invViewProj;
  float4 camPosMaxDist;
  float4 compositeParams;
};
Texture2D sceneDepth : register(t0);
Texture3D apVolume : register(t1);
Texture2D cloudDepth : register(t2);
SamplerState sceneDepth_sampler : register(s0);
SamplerState apVolume_sampler : register(s1);
SamplerState cloudDepth_sampler : register(s2);
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float PhysicalSlice(VSOut v) {
  float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0).r;
  if (depth >= 1.0) discard;
  float2 ndc = float2(v.uv.x * 2.0 - 1.0, 1.0 - v.uv.y * 2.0);
  float4 wp = mul(float4(ndc, depth, 1.0), invViewProj);
  float safeW = abs(wp.w) > 1e-6 ? wp.w : (wp.w < 0.0 ? -1e-6 : 1e-6);
  float3 worldPos = wp.xyz / safeW;
  float maxDist = max(camPosMaxDist.w, 1e-3);
  float dist = length(worldPos - camPosMaxDist.xyz);
  return sqrt(saturate(dist / maxDist));
}
float4 PSMultiply(VSOut v) : SV_TARGET {
  float slice = PhysicalSlice(v);
  float4 transfer = apVolume.SampleLevel(
      apVolume_sampler, float3(v.uv, slice), 0.0);
  bool valid = transfer.a >= 0.5 && transfer.a <= 1.01
            && all(transfer.rgb == transfer.rgb)
            && all(transfer.rgb >= 0.0) && all(transfer.rgb <= 1.001);
  // A missing/unbound u1 is normally zero-filled. Never multiply the scene by
  // that invalid transfer: fail open to identity until the volume is valid.
  return valid ? float4(saturate(transfer.rgb), 1.0)
               : float4(1.0, 1.0, 1.0, 1.0);
}
float4 PSAddScatter(VSOut v) : SV_TARGET {
  float slice = PhysicalSlice(v);
  float3 inScatter = apVolume.SampleLevel(
      apVolume_sampler, float3(v.uv, slice), 0.0).rgb;
  bool valid = all(inScatter == inScatter)
            && all(abs(inScatter) < 65504.0);
  return float4(valid ? max(inScatter, 0.0) : float3(0,0,0), 1.0);
}
float4 PSMain(VSOut v) : SV_TARGET {
  float depth = sceneDepth.SampleLevel(sceneDepth_sampler, v.uv, 0.0).r;
  float maxDist = max(camPosMaxDist.w, 1e-3);
  float dist;
  if (depth >= 1.0) {
    // Physical-atmosphere mode leaves the already baked sky untouched.
    // Local-fog mode covers sky/cloud pixels. Clouds use their resolved
    // distance; only a clear sky reaches the local volume's far slice.
    if (compositeParams.x <= 0.5) discard;
    dist = maxDist;
    if (compositeParams.y > 0.5) {
      float resolvedCloudDepth =
          cloudDepth.SampleLevel(cloudDepth_sampler, v.uv, 0.0).r;
      if (resolvedCloudDepth <= 250000.0)
        dist = min(dist, resolvedCloudDepth);
    }
  } else {
    // Geometry is always terminated at the reconstructed surface distance.
    float2 ndc = float2(v.uv.x * 2.0 - 1.0, 1.0 - v.uv.y * 2.0);
    float4 wp = mul(float4(ndc, depth, 1.0), invViewProj);
    float safeW = abs(wp.w) > 1e-6 ? wp.w : (wp.w < 0.0 ? -1e-6 : 1e-6);
    float3 worldPos = wp.xyz / safeW;
    dist = length(worldPos - camPosMaxDist.xyz);
  }
  float slice = sqrt(saturate(dist / maxDist));
  float4 ap = apVolume.SampleLevel(apVolume_sampler, float3(v.uv, slice), 0.0);
  float opacity = saturate(ap.a);
  float3 straightScatter = opacity > 1e-5 ? max(ap.rgb, 0.0) / opacity : 0.0;
  return float4(straightScatter, opacity);
}
)";

} // namespace

bool CSkyAtmosphere::SameVolumeCacheKey(
    const FVolumeCacheKey& lhs,
    const FVolumeCacheKey& rhs) noexcept {
    return std::memcmp(&lhs, &rhs, sizeof(FVolumeCacheKey)) == 0;
}

TResult<void> CSkyAtmosphere::Init(IRhiDevice& device, EFormat hdr_format) noexcept {
    m_Ready = false;
    m_LutsReady = false;
    m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    m_PhysicalApCacheValid = false;
    m_LocalFogCacheValid = false;
    m_PhysicalApDispatchCount = 0;
    m_LocalFogDispatchCount = 0;
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kTransCS;
        sd.entry_point = "CSTrans"; sd.debug_name = "Atmo.TransCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_TransCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_TransCs.Get(); pd.uav_slots = 1; pd.uav_names[0] = "transOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_TransPipe = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kBakeCS;
        sd.entry_point = "CSBake"; sd.debug_name = "Atmo.BakeCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_BakeCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_BakeCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "AtmoCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut"; pd.uav_slots = 1; pd.uav_names[0] = "bakeOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_BakePipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 256; td.height = 64; td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_TransLut = Move(r.Value()); }
    // Multi-scattering LUT (32x32 RGBA16F UAV/SRV) + compute pipeline (WE multiScatteredLuminanceLut)。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kMultiCS;
        sd.entry_point = "CSMulti"; sd.debug_name = "Atmo.MultiCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_MultiCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_MultiCs.Get(); pd.srv_slots = 1; pd.srv_names[0] = "transLut";
        pd.uav_slots = 1; pd.uav_names[0] = "msOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_MultiPipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = 32; td.height = 32; td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_MultiLut = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_Cb = Move(r.Value()); }
    // aerial perspective + local fog 用に 48x48x96 RGBA16F froxel volume、
    // compute pipeline、CB を構築する。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kApCS;
        sd.entry_point = "CSAp"; sd.debug_name = "Atmo.ApCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Compute; sd.hlsl_source = kApCS;
        sd.entry_point = "CSLocalFog"; sd.debug_name = "Atmo.LocalFogCS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCs = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_ApCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut";
        pd.uav_slots = 2; pd.uav_names[0] = "apOut"; pd.uav_names[1] = "apTransOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApPipe = Move(r.Value()); }
    {   FComputePipelineDesc pd{}; pd.cs = m_LocalFogCs.Get(); pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCB";
        pd.srv_slots = 2; pd.srv_names[0] = "transLut"; pd.srv_names[1] = "multiLut";
        pd.uav_slots = 1; pd.uav_names[0] = "apOut";
        auto r = CreateRhiComputePipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogPipe = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ApVol = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_ApTransVol = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = sizeof(FApCB); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCb = Move(r.Value()); }
    {   FTextureDesc td{}; td.width = kApXYRes; td.height = kApXYRes; td.depth = kApZRes;
        td.format = EFormat::R16G16B16A16_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogVol = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = sizeof(FApCB); bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCb = Move(r.Value()); }
    // fullscreen depth-aware AP composite。この pass では depth を SRV として sample し、
    // DSV には bind しない。
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Vertex; sd.hlsl_source = kApCompositeVS;
        sd.entry_point = "VSMain"; sd.debug_name = "Atmo.ApCompositeVS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositeVs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSMain"; sd.debug_name = "Atmo.ApCompositePS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositePs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSMultiply"; sd.debug_name = "Atmo.ApMultiplyPS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApMultiplyPs = Move(r.Value()); }
    {   FShaderDesc sd{}; sd.stage = EShaderStage::Pixel; sd.hlsl_source = kApCompositePS;
        sd.entry_point = "PSAddScatter"; sd.debug_name = "Atmo.ApAddScatterPS";
        auto r = CreateRhiShader(device, sd); if (r.IsErr()) return Err<void>(r.Error()); m_ApAddPs = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApCompositePs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AlphaBlend;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 3;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.texture_names[2] = "cloudDepth";
        pd.static_sampler_count = 3;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.static_samplers[2].filter = ESamplerFilter::Point;
        pd.static_samplers[2].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[2].address_v = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositePipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApMultiplyPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::Multiply;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 2;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.static_sampler_count = 2;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApMultiplyPipe = Move(r.Value()); }
    {   FPipelineDesc pd{};
        pd.vs = m_ApCompositeVs.Get(); pd.ps = m_ApAddPs.Get();
        pd.topology = EPrimitiveTopology::TriangleList;
        pd.rt_format = hdr_format; pd.depth_format = EFormat::Unknown;
        pd.depth_test = false; pd.depth_write = false;
        pd.cull_mode = ECullMode::None; pd.blend_mode = EBlendMode::AdditivePreserveAlpha;
        pd.cbuffer_slots = 1; pd.cbuffer_names[0] = "ApCompositeCB";
        pd.texture_slots = 2;
        pd.texture_names[0] = "sceneDepth";
        pd.texture_names[1] = "apVolume";
        pd.static_sampler_count = 2;
        pd.static_samplers[0].filter = ESamplerFilter::Point;
        pd.static_samplers[0].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[0].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].filter = ESamplerFilter::Linear;
        pd.static_samplers[1].address_u = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_v = ESamplerAddress::Clamp;
        pd.static_samplers[1].address_w = ESamplerAddress::Clamp;
        pd.layout_count = 0; pd.vertex_stride = 0;
        auto r = CreateRhiPipeline(device, pd); if (r.IsErr()) return Err<void>(r.Error()); m_ApAddPipe = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_ApCompositeCb = Move(r.Value()); }
    {   FBufferDesc bd{}; bd.size = 256; bd.usage = EBufferUsage::Uniform; bd.cpu_writable = true;
        auto r = CreateRhiBuffer(device, bd); if (r.IsErr()) return Err<void>(r.Error()); m_LocalFogCompositeCb = Move(r.Value()); }
    m_Ready = true;
    ACS_LOG_INFO("CSkyAtmosphere: scattering LUTs and aerial-perspective volume initialized");
    return Ok();
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspective(IRhiDevice& device, IRhiCommandList& cl,
                                                    const FMat4& inv_view_proj, FVec3 cam_pos,
                                                    FVec3 sun_dir, FVec3 sun_intensity,
                                                    f32 max_dist_scene, f32 scene_to_km,
                                                    f32 cam_alt_km) noexcept {
    return BuildAerialPerspective(device, cl, inv_view_proj, cam_pos, sun_dir, sun_intensity,
                                  max_dist_scene, scene_to_km, cam_alt_km,
                                  FVolumetricFogParams{});
}

IRhiTexture* CSkyAtmosphere::BuildAerialPerspective(IRhiDevice& /*device*/, IRhiCommandList& cl,
                                                    const FMat4& inv_view_proj, FVec3 cam_pos,
                                                    FVec3 sun_dir, FVec3 sun_intensity,
                                                    f32 max_dist_scene, f32 scene_to_km,
                                                    f32 cam_alt_km,
                                                    const FVolumetricFogParams& fog) noexcept {
    m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    if (!m_Ready || !m_ApVol || !m_ApTransVol || !m_TransLut ||
        !m_ApPipe || !m_LocalFogPipe) {
        return nullptr;
    }
    const FMat4 sanitizedInvViewProj = SanitizeMatrix(inv_view_proj);
    cam_pos = SanitizeVec3(cam_pos, FVec3{0, 0, 0});
    FVec3 sd = SanitizeVec3(sun_dir, FVec3{0, 1, 0});
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    sun_intensity = SanitizeVec3(sun_intensity, FVec3{0, 0, 0});
    sun_intensity = FVec3{
        sun_intensity.x > 0.0f ? sun_intensity.x : 0.0f,
        sun_intensity.y > 0.0f ? sun_intensity.y : 0.0f,
        sun_intensity.z > 0.0f ? sun_intensity.z : 0.0f,
    };
    max_dist_scene = FiniteOr(max_dist_scene, 0.01f);
    scene_to_km = FiniteOr(scene_to_km, 0.0f);
    cam_alt_km = FiniteOr(cam_alt_km, 0.0f);
    if (max_dist_scene < 0.01f) max_dist_scene = 0.01f;
    if (scene_to_km < 0.0f) scene_to_km = 0.0f;
    if (cam_alt_km < 0.0f) cam_alt_km = 0.0f;

    FVec3 fogColor =
        SanitizeVec3(fog.color, FVolumetricFogParams{}.color);
    fogColor = FVec3{
        fogColor.x > 0.0f ? fogColor.x : 0.0f,
        fogColor.y > 0.0f ? fogColor.y : 0.0f,
        fogColor.z > 0.0f ? fogColor.z : 0.0f,
    };
    f32 fogDensity = FiniteOr(fog.density, 0.0f);
    f32 fogFalloff = FiniteOr(fog.height_falloff, 0.0f);
    f32 fogBase = FiniteOr(fog.height_base, 0.0f);
    f32 fogG = FiniteOr(fog.anisotropy, 0.0f);
    f32 fogSun = FiniteOr(fog.sun_scatter, 0.0f);
    if (fogDensity < 0.0f) fogDensity = 0.0f;
    if (fogFalloff < 0.0f) fogFalloff = 0.0f;
    if (fogG < -0.85f) fogG = -0.85f;
    if (fogG >  0.85f) fogG =  0.85f;
    if (fogSun < 0.0f) fogSun = 0.0f;

    FApCB cb{};
    cb.invViewProj = sanitizedInvViewProj;
    cb.camPos = FVec4{cam_pos.x, cam_pos.y, cam_pos.z, 0.0f};
    cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{sun_intensity.x, sun_intensity.y, sun_intensity.z, 0.0f};
    cb.apParams = FVec4{scene_to_km, cam_alt_km, max_dist_scene, static_cast<f32>(kApZRes)};
    cb.fogColorDensity = FVec4{fogColor.x, fogColor.y, fogColor.z, fogDensity};
    cb.fogParams = FVec4{fogFalloff, fogBase, fogG, fogSun};
    // Keep the long-range atmosphere volume free of local fog. Its 250 km
    // range is intentionally coarse near the camera; local fog is integrated
    // into a separate 2.5 km volume below.
    FApCB atmosphereCb = cb;
    atmosphereCb.fogColorDensity.w = 0.0f;

    FVolumeCacheKey physicalKey{};
    physicalKey.invViewProj = atmosphereCb.invViewProj;
    physicalKey.camPos = atmosphereCb.camPos;
    physicalKey.sunDir = atmosphereCb.sunDir;
    physicalKey.sunInt = atmosphereCb.sunInt;
    physicalKey.apParams = atmosphereCb.apParams;
    const bool physicalEnabled = scene_to_km > 0.0f;
    const bool physicalDirty =
        physicalEnabled &&
        (!m_PhysicalApCacheValid ||
         !SameVolumeCacheKey(m_PhysicalApCacheKey, physicalKey));
    // Transmittance / multi-scattering LUT は Earth 定数だけで決まり、camera/sun には非依存。
    // 初回だけ焼き、毎フレームは camera-volume 本体の更新に GPU 時間を集中する。
    if (!m_LutsReady) {
        cl.SetComputePipeline(*m_TransPipe);
        cl.BindUav(0, *m_TransLut);
        cl.Dispatch(32, 8, 1);
        cl.SetComputePipeline(*m_MultiPipe);
        cl.SetTexture(0, *m_TransLut);
        cl.BindUav(0, *m_MultiLut);
        cl.Dispatch(4, 4, 1);
        m_LutsReady = true;
    }
    // Rebuild only when one of the exact sanitized physical inputs changed.
    // A static editor camera therefore pays the 48x48x96x24 integration once.
    if (physicalDirty) {
        m_ApCb->Update(&atmosphereCb, sizeof(atmosphereCb));
        cl.SetComputePipeline(*m_ApPipe);
        cl.SetConstantBuffer(0, *m_ApCb);
        cl.SetTexture(0, *m_TransLut);
        cl.SetTexture(1, *m_MultiLut);
        cl.BindUav(0, *m_ApVol);
        cl.BindUav(1, *m_ApTransVol);
        cl.Dispatch(kApXYRes / 4, kApXYRes / 4, kApZRes / 4);
        m_PhysicalApCacheKey = physicalKey;
        m_PhysicalApCacheValid = true;
        ++m_PhysicalApDispatchCount;
    }

    // The physical sky already includes atmospheric scattering.  Keep a
    // separate local-fog transfer volume so its far slice can be applied to
    // clear depth without double-applying Rayleigh/Mie.  A dedicated CB is
    // required: both dispatches may execute after the CPU-side updates.
    m_LocalFogVolumeValid = fogDensity > 1e-7f && m_LocalFogVol && m_LocalFogCb;
    if (m_LocalFogVolumeValid) {
        FApCB fogOnlyCb = cb;
        fogOnlyCb.apParams.x = 0.0f; // suppress atmospheric extinction/scatter
        m_LocalFogMaxDistance =
            max_dist_scene < kLocalVolumetricFogMaxDistance
                ? max_dist_scene
                : kLocalVolumetricFogMaxDistance;
        fogOnlyCb.apParams.z = m_LocalFogMaxDistance;

        FVolumeCacheKey localFogKey{};
        localFogKey.invViewProj = fogOnlyCb.invViewProj;
        localFogKey.camPos = fogOnlyCb.camPos;
        localFogKey.sunDir = fogOnlyCb.sunDir;
        localFogKey.sunInt = fogOnlyCb.sunInt;
        localFogKey.apParams = fogOnlyCb.apParams;
        localFogKey.fogColorDensity = fogOnlyCb.fogColorDensity;
        localFogKey.fogParams = fogOnlyCb.fogParams;
        const bool localFogDirty =
            !m_LocalFogCacheValid ||
            !SameVolumeCacheKey(m_LocalFogCacheKey, localFogKey);
        if (localFogDirty) {
            m_LocalFogCb->Update(&fogOnlyCb, sizeof(fogOnlyCb));
            cl.SetComputePipeline(*m_LocalFogPipe);
            cl.SetConstantBuffer(0, *m_LocalFogCb);
            cl.SetTexture(0, *m_TransLut);
            cl.SetTexture(1, *m_MultiLut);
            cl.BindUav(0, *m_LocalFogVol);
            cl.Dispatch(kApXYRes / 4, kApXYRes / 4, kApZRes / 4);
            m_LocalFogCacheKey = localFogKey;
            m_LocalFogCacheValid = true;
            ++m_LocalFogDispatchCount;
        }
    }
    return physicalEnabled ? m_ApVol.Get() : nullptr;
}

void CSkyAtmosphere::CompositeAerialPerspective(IRhiCommandList& cl,
                                                IRhiTexture& depth,
                                                IRhiTexture& ap_volume,
                                                IRhiTexture& transmittance_volume,
                                                const FMat4& inv_view_proj,
                                                FVec3 cam_pos,
                                                f32 max_dist_scene,
                                                u32 screen_width,
                                                u32 screen_height) noexcept {
    if (!m_Ready || !m_ApMultiplyPipe || !m_ApAddPipe ||
        !m_ApCompositeCb ||
        screen_width == 0 || screen_height == 0) {
        return;
    }
    FApCompositeCB cb{};
    cb.invViewProj = inv_view_proj;
    cb.camPosMaxDist = FVec4{cam_pos.x, cam_pos.y, cam_pos.z,
                            max_dist_scene > 0.001f ? max_dist_scene : 0.001f};
    cb.compositeParams = FVec4{0.0f, 0.0f, 0.0f, 0.0f};
    m_ApCompositeCb->Update(&cb, sizeof(cb));

    FViewport vp{};
    vp.width = static_cast<f32>(screen_width);
    vp.height = static_cast<f32>(screen_height);
    cl.SetViewport(vp);
    FScissorRect sr{};
    sr.right = static_cast<i32>(screen_width);
    sr.bottom = static_cast<i32>(screen_height);
    cl.SetScissor(sr);

    // Exact wavelength-dependent transfer:
    //   scene.rgb = scene.rgb * T.rgb + L.rgb
    // A pipeline switch invalidates root/resource bindings on both RHIs, so
    // every pass deliberately rebinds its complete resource set.
    cl.SetPipeline(*m_ApMultiplyPipe);
    cl.SetConstantBuffer(0, *m_ApCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, transmittance_volume);
    cl.Draw(3, 0);

    cl.SetPipeline(*m_ApAddPipe);
    cl.SetConstantBuffer(0, *m_ApCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, ap_volume);
    cl.Draw(3, 0);
}

void CSkyAtmosphere::CompositeLocalFog(
    IRhiCommandList& cl, IRhiTexture& depth, IRhiTexture& local_fog_volume,
    IRhiTexture* cloud_depth, const FMat4& inv_view_proj,
    FVec3 cam_pos, f32 max_dist_scene,
    u32 screen_width, u32 screen_height) noexcept {
    if (!m_Ready || !m_ApCompositePipe || !m_LocalFogCompositeCb ||
        screen_width == 0 || screen_height == 0) {
        return;
    }

    FApCompositeCB cb{};
    cb.invViewProj = inv_view_proj;
    cb.camPosMaxDist = FVec4{cam_pos.x, cam_pos.y, cam_pos.z,
                            max_dist_scene > 0.001f ? max_dist_scene : 0.001f};
    cb.compositeParams =
        FVec4{1.0f, cloud_depth != nullptr ? 1.0f : 0.0f, 0.0f, 0.0f};
    m_LocalFogCompositeCb->Update(&cb, sizeof(cb));

    FViewport vp{};
    vp.width = static_cast<f32>(screen_width);
    vp.height = static_cast<f32>(screen_height);
    cl.SetViewport(vp);
    FScissorRect sr{};
    sr.right = static_cast<i32>(screen_width);
    sr.bottom = static_cast<i32>(screen_height);
    cl.SetScissor(sr);
    cl.SetPipeline(*m_ApCompositePipe);
    cl.SetConstantBuffer(0, *m_LocalFogCompositeCb);
    cl.SetTexture(0, depth);
    cl.SetTexture(1, local_fog_volume);
    cl.SetTexture(2, cloud_depth != nullptr ? *cloud_depth : depth);
    cl.Draw(3, 0);
}

bool CSkyAtmosphere::BakeEquirect(IRhiDevice& device, IRhiCommandList& cl,
                                  const FAtmosphereParams& params,
                                  u32 width, u32 height, TArray<f32>& out) noexcept {
    if (!m_Ready) return false;
    FVec3 sd = params.sun_dir;
    {   f32 l2 = sd.x*sd.x + sd.y*sd.y + sd.z*sd.z;
        if (l2 < 1e-12f) sd = FVec3{0, 1, 0};
        else { f32 inv = 1.0f / Sqrt(l2); sd = FVec3{sd.x*inv, sd.y*inv, sd.z*inv}; } }
    FAtmoCB cb{}; cb.sunDir = FVec4{sd.x, sd.y, sd.z, 0.0f};
    cb.sunInt = FVec4{params.sun_intensity.x, params.sun_intensity.y, params.sun_intensity.z, 0.0f};
    cb.groundAlbedo = FVec4{
        params.ground_albedo.x > 0.0f ? params.ground_albedo.x : 0.0f,
        params.ground_albedo.y > 0.0f ? params.ground_albedo.y : 0.0f,
        params.ground_albedo.z > 0.0f ? params.ground_albedo.z : 0.0f,
        0.0f};
    m_Cb->Update(&cb, sizeof(cb));

    // 1) 大気 LUT は定数なので初回だけ焼く。AP と equirect bake のどちらが先でも共有する。
    if (!m_LutsReady) {
        cl.SetComputePipeline(*m_TransPipe);
        cl.BindUav(0, *m_TransLut);
        cl.Dispatch(32, 8, 1);
        cl.SetComputePipeline(*m_MultiPipe);
        cl.SetTexture(0, *m_TransLut);
        cl.BindUav(0, *m_MultiLut);
        cl.Dispatch(4, 4, 1);
        m_LutsReady = true;
    }

    // 2) equirect texture を (再) 確保 (RGBA32F、readback 用)。
    if (m_EqW != width || m_EqH != height || !m_Equirect) {
        FTextureDesc td{}; td.width = width; td.height = height;
        td.format = EFormat::R32G32B32A32_Float; td.is_uav = true;
        auto r = CreateRhiTexture(device, td); if (r.IsErr()) return false;
        m_Equirect = Move(r.Value()); m_EqW = width; m_EqH = height;
    }
    // 3) equirect bake (transLut SRV を読みつつ)。
    cl.SetComputePipeline(*m_BakePipe);
    cl.SetConstantBuffer(0, *m_Cb);
    cl.SetTexture(0, *m_TransLut);
    cl.SetTexture(1, *m_MultiLut);
    cl.BindUav(0, *m_Equirect);
    cl.Dispatch((width + 7) / 8, (height + 7) / 8, 1);

    // 4) CPU へ読み戻す (ReadTexture が Flush+WaitIdle → 上の dispatch を実行してから copy)。
    out.Resize(static_cast<usize>(width) * height * 4u);
    return device.ReadTexture(*m_Equirect, out.Data(),
                              static_cast<u32>(out.Size() * sizeof(f32)));
}

void CSkyAtmosphere::Shutdown() noexcept {
    m_TransPipe.Reset(); m_BakePipe.Reset();
    m_TransCs.Reset();   m_BakeCs.Reset();
    m_MultiPipe.Reset(); m_MultiCs.Reset(); m_MultiLut.Reset();   // 多重散乱 LUT (UAF 防止)
    m_TransLut.Reset();  m_Equirect.Reset(); m_Cb.Reset();
    m_ApMultiplyPipe.Reset(); m_ApMultiplyPs.Reset();
    m_ApAddPipe.Reset(); m_ApAddPs.Reset();
    m_ApCompositePipe.Reset(); m_ApCompositePs.Reset(); m_ApCompositeVs.Reset();
    m_ApCompositeCb.Reset(); m_LocalFogCompositeCb.Reset();
    m_ApPipe.Reset(); m_ApCs.Reset(); m_ApVol.Reset(); m_ApTransVol.Reset();
    m_ApCb.Reset(); // aerial perspective (UAF 防止)
    m_LocalFogPipe.Reset(); m_LocalFogCs.Reset();
    m_LocalFogVol.Reset(); m_LocalFogCb.Reset(); m_LocalFogVolumeValid = false;
    m_LocalFogMaxDistance = kLocalVolumetricFogMaxDistance;
    m_PhysicalApCacheValid = false; m_LocalFogCacheValid = false;
    m_PhysicalApDispatchCount = 0; m_LocalFogDispatchCount = 0;
    m_EqW = 0; m_EqH = 0; m_LutsReady = false; m_Ready = false;
}

} // namespace acs
