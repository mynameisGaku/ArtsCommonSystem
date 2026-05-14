// Physical atmospheric scattering 実装 (Phase 34c)
#include "render/Atmosphere.h"
#include "math/Math.h"
#include "foundation/Move.h"

namespace acs {

namespace {

constexpr f32 kGroundRadius     = 6360000.0f;       // m
constexpr f32 kAtmosphereRadius = 6420000.0f;       // m
constexpr f32 kRayleighH        = 8000.0f;          // 8 km scale height
constexpr f32 kMieH             = 1200.0f;          // 1.2 km

inline Vec3 RayleighBeta() noexcept {
    return Vec3{5.802e-6f, 13.558e-6f, 33.1e-6f};
}
inline f32  MieBeta() noexcept       { return 3.996e-6f; }
inline f32  MieAbsorption() noexcept { return 4.4e-6f; }
inline f32  MieG() noexcept          { return 0.8f; }

// Ray-sphere intersection、戻り値 t (ray origin から hit までの距離)。
// ro は地球中心からのオフセット、rd は単位ベクトル前提。常に外側→外側で
// 大きい方の解を返す (ray が大気圏内にあるとき、外側に出る距離)。
f32 RaySphereOuter(Vec3 ro, Vec3 rd, f32 radius) noexcept {
    f32 b = Dot(ro, rd);
    f32 c = Dot(ro, ro) - radius * radius;
    f32 disc = b*b - c;
    if (disc < 0) return -1.0f;
    return -b + Sqrt(disc);
}

// Henyey-Greenstein phase (mie 用)
ACS_FORCEINLINE f32 PhaseHG(f32 cosTheta, f32 g) noexcept {
    f32 g2 = g * g;
    f32 d = 1.0f + g2 - 2.0f * g * cosTheta;
    return (1.0f - g2) / (4.0f * kPi * Pow(d, 1.5f));
}

// Rayleigh phase
ACS_FORCEINLINE f32 PhaseRayleigh(f32 cosTheta) noexcept {
    return (3.0f / (16.0f * kPi)) * (1.0f + cosTheta * cosTheta);
}

// 高度 h (m, 地表 0 が原点) での Rayleigh / Mie 密度比
ACS_FORCEINLINE f32 DensityRayleigh(f32 h) noexcept { return Exp(-h / kRayleighH); }
ACS_FORCEINLINE f32 DensityMie(f32 h)      noexcept { return Exp(-h / kMieH); }

// transmittance T(P, Q) を点 P から点 Q (= P + dir * t) へ計算。
// 累積光学厚さは sun_steps 段で trapezoid 積分。
Vec3 Transmittance(Vec3 P_earth_centered, Vec3 dir, f32 t_max, u32 steps) noexcept {
    if (t_max <= 0.0f || steps == 0) return Vec3{1, 1, 1};
    f32 step_len = t_max / static_cast<f32>(steps);
    f32 optical_depth_r = 0;
    f32 optical_depth_m = 0;
    for (u32 i = 0; i < steps; ++i) {
        Vec3 sample_pos = P_earth_centered + dir * (step_len * (static_cast<f32>(i) + 0.5f));
        f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) alt = 0;
        optical_depth_r += DensityRayleigh(alt) * step_len;
        optical_depth_m += DensityMie(alt)      * step_len;
    }
    Vec3 beta_r = RayleighBeta();
    f32  beta_m_ext = MieBeta() + MieAbsorption();
    Vec3 tau{
        beta_r.x * optical_depth_r + beta_m_ext * optical_depth_m,
        beta_r.y * optical_depth_r + beta_m_ext * optical_depth_m,
        beta_r.z * optical_depth_r + beta_m_ext * optical_depth_m,
    };
    return Vec3{Exp(-tau.x), Exp(-tau.y), Exp(-tau.z)};
}

// 単散乱: view ray r(t) = ro + rd * t、ro は地球中心オフセットの座標。
// 太陽方向 sun_dir からの寄与を view ray 全体に渡って積算。
Vec3 SingleScatter(Vec3 ro, Vec3 rd, Vec3 sun_dir, Vec3 sun_intensity,
                  u32 ray_steps, u32 sun_steps) noexcept {
    // 大気圏外との交点まで march
    f32 t_atm = RaySphereOuter(ro, rd, kAtmosphereRadius);
    if (t_atm <= 0) return Vec3{0, 0, 0};

    f32 cos_view_sun = Dot(rd, sun_dir);
    f32 phase_r = PhaseRayleigh(cos_view_sun);
    f32 phase_m = PhaseHG(cos_view_sun, MieG());

    f32 step_len = t_atm / static_cast<f32>(ray_steps);
    Vec3 beta_r = RayleighBeta();
    f32  beta_m = MieBeta();

    Vec3 inscatter_r{0, 0, 0};
    Vec3 inscatter_m{0, 0, 0};
    // view ray 沿いに伝播した累積光学厚さ (T_view)
    f32 view_od_r = 0, view_od_m = 0;

    for (u32 i = 0; i < ray_steps; ++i) {
        Vec3 sample_pos = ro + rd * (step_len * (static_cast<f32>(i) + 0.5f));
        f32 alt = Sqrt(Dot(sample_pos, sample_pos)) - kGroundRadius;
        if (alt < 0) {
            // 地面に潜った点、寄与なし
            continue;
        }
        f32 d_r = DensityRayleigh(alt) * step_len;
        f32 d_m = DensityMie(alt)      * step_len;
        view_od_r += d_r;
        view_od_m += d_m;

        // 太陽光線が sample_pos へ届く透過率
        f32 t_sun = RaySphereOuter(sample_pos, sun_dir, kAtmosphereRadius);
        if (t_sun <= 0) continue;
        Vec3 T_sun = Transmittance(sample_pos, sun_dir, t_sun, sun_steps);

        // view side 透過率 (現位置までの累積)
        f32 beta_m_ext = beta_m + MieAbsorption();
        Vec3 tau_view{
            beta_r.x * view_od_r + beta_m_ext * view_od_m,
            beta_r.y * view_od_r + beta_m_ext * view_od_m,
            beta_r.z * view_od_r + beta_m_ext * view_od_m,
        };
        Vec3 T_view{Exp(-tau_view.x), Exp(-tau_view.y), Exp(-tau_view.z)};

        Vec3 T_combined{T_sun.x * T_view.x, T_sun.y * T_view.y, T_sun.z * T_view.z};
        inscatter_r = inscatter_r + T_combined * (d_r);
        inscatter_m = inscatter_m + T_combined * (d_m);
    }

    Vec3 result{
        sun_intensity.x * (beta_r.x * phase_r * inscatter_r.x + beta_m * phase_m * inscatter_m.x),
        sun_intensity.y * (beta_r.y * phase_r * inscatter_r.y + beta_m * phase_m * inscatter_m.y),
        sun_intensity.z * (beta_r.z * phase_r * inscatter_r.z + beta_m * phase_m * inscatter_m.z),
    };
    return result;
}

} // namespace

Array<f32> Atmosphere::BakeEquirect(u32 width, u32 height,
                                     const AtmosphereParams& params) noexcept {
    Array<f32> out;
    out.Resize(static_cast<usize>(width) * height * 4u);

    Vec3 sun_dir;
    {
        f32 l2 = params.sun_dir.x*params.sun_dir.x
               + params.sun_dir.y*params.sun_dir.y
               + params.sun_dir.z*params.sun_dir.z;
        if (l2 < 1e-12f) sun_dir = Vec3{0, 1, 0};
        else {
            f32 inv = 1.0f / Sqrt(l2);
            sun_dir = Vec3{params.sun_dir.x*inv, params.sun_dir.y*inv, params.sun_dir.z*inv};
        }
    }
    // 観察者位置: 地表 + 数 m (camera 高度的なもの)。地球中心が原点なので
    // (0, ground_radius + 2, 0) に置く。
    Vec3 viewer{0, kGroundRadius + 2.0f, 0};

    for (u32 y = 0; y < height; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(height) * kPi;
        const f32 sinT = Sin(theta);
        const f32 cosT = Cos(theta);
        for (u32 x = 0; x < width; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(width);
            const f32 phi = phi_norm * 2.0f * kPi - kPi;
            // equirect 規約: theta=0 が +Y、phi=0 が +Z
            Vec3 view_dir{sinT * Sin(phi), cosT, sinT * Cos(phi)};

            Vec3 col;
            if (view_dir.y < -0.05f) {
                // 地表より下を向く: 簡易 ground (大気の散乱で青みのある暗い色)
                col = Vec3{0.04f, 0.04f, 0.05f};
            } else {
                col = SingleScatter(viewer, view_dir, sun_dir, params.sun_intensity,
                                    params.ray_steps, params.sun_steps);
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

} // namespace acs
