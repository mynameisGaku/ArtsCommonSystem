// SPDX-License-Identifier: Apache-2.0
// HelloIbl — CPU 側 equirect 環境マップ生成の実装。
#include "IblEnvBuilder.h"
#include "IblTypes.h"

#include "render/Sky.h"
#include "math/Vec.h"
#include "math/Math.h"

using namespace acs;

namespace helloibl {

void BuildEquirectFromSky(const Sky& sky, Array<f32>& buf) noexcept {
    if (buf.Size() == 0) {
        buf.Resize(static_cast<usize>(kEquirectWidth) * kEquirectHeight * 4u);
    }
    auto safe_sqrt_n = [](Vec3 v) noexcept {
        f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
        if (len2 < 1e-12f) return Vec3{0, 1, 0};
        f32 inv = 1.0f / Sqrt(len2);
        return Vec3{v.x * inv, v.y * inv, v.z * inv};
    };
    auto smoothstep = [](f32 a, f32 b, f32 x) noexcept {
        f32 t = (x - a) / (b - a);
        t = Saturate(t);
        return t * t * (3.0f - 2.0f * t);
    };

    const Vec3 sun_dir = safe_sqrt_n(sky.SunDirection());
    const Vec3 sun_col = sky.SunColor();
    const Vec3 zenith  = sky.ZenithColor();
    const Vec3 horizon = sky.HorizonColor();
    const Vec3 ground  = sky.GroundColor();
    const f32  sun_r   = sky.SunRadius();
    const f32  sun_g   = sky.SunGlow();

    for (u32 y = 0; y < kEquirectHeight; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kEquirectHeight) * kPi;
        const f32 sinT = Sin(theta), cosT = Cos(theta);
        for (u32 x = 0; x < kEquirectWidth; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kEquirectWidth);
            const f32 phi = phi_norm * 2.0f * kPi - kPi;
            // equirect 規約: phi=0 が +Z、theta=0 が +Y
            const Vec3 dir{ sinT * Sin(phi), cosT, sinT * Cos(phi) };

            Vec3 sky_col;
            if (dir.y >= 0.0f) {
                const f32 k = Pow(Saturate(dir.y), 0.6f);
                sky_col = horizon * (1.0f - k) + zenith * k;
            } else {
                const f32 k = Pow(Saturate(-dir.y), 0.6f);
                sky_col = horizon * (1.0f - k) + ground * k;
            }
            const f32 c = Saturate(dir.x * sun_dir.x + dir.y * sun_dir.y + dir.z * sun_dir.z);
            const f32 ang = 1.0f - c;
            if (ang < sun_r) {
                sky_col = sun_col;
            } else if (ang < sun_g) {
                const f32 k = 1.0f - smoothstep(sun_r, sun_g, ang);
                sky_col = sky_col * (1.0f - k) + sun_col * k;
            }
            const u32 idx = (y * kEquirectWidth + x) * 4u;
            buf[idx + 0] = sky_col.x;
            buf[idx + 1] = sky_col.y;
            buf[idx + 2] = sky_col.z;
            buf[idx + 3] = 1.0f;
        }
    }
}

void BuildStudioHdrEquirect(Array<f32>& buf) noexcept {
    if (buf.Size() == 0) {
        buf.Resize(static_cast<usize>(kEquirectWidth) * kEquirectHeight * 4u);
    }
    const f32 background[3] = {0.03f, 0.03f, 0.04f};
    struct Panel { f32 phi; f32 r, g, b; };
    const Panel panels[4] = {
        {0.0f,             8.0f, 2.0f, 1.5f},      // 前方 (赤橙)
        {kPi * 0.5f,       1.5f, 8.0f, 2.5f},      // 右   (黄緑)
        {kPi,              1.5f, 2.5f, 8.0f},      // 後方 (青)
        {kPi * 1.5f,       8.0f, 7.5f, 4.0f},      // 左   (白橙、暖色キー)
    };
    const f32 target_theta = kPi * 0.4f;      // 地平のやや上 (60°≒0.4π)
    const f32 panel_radius = 0.10f;           // panel が広がる角度 (≈18°)

    for (u32 y = 0; y < kEquirectHeight; ++y) {
        const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kEquirectHeight) * kPi;
        for (u32 x = 0; x < kEquirectWidth; ++x) {
            const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kEquirectWidth);
            const f32 phi      = phi_norm * 2.0f * kPi - kPi;  // [-π, π]

            f32 r = background[0], g = background[1], b = background[2];
            for (u32 i = 0; i < 4; ++i) {
                f32 dphi = phi - panels[i].phi;
                while (dphi >  kPi) dphi -= 2.0f * kPi;
                while (dphi < -kPi) dphi += 2.0f * kPi;
                const f32 dtheta = theta - target_theta;
                const f32 d2 = dphi * dphi + dtheta * dtheta;
                if (d2 < panel_radius * panel_radius) {
                    // gaussian-ish falloff
                    const f32 k = 1.0f - d2 / (panel_radius * panel_radius);
                    r += panels[i].r * k;
                    g += panels[i].g * k;
                    b += panels[i].b * k;
                }
            }
            const u32 idx = (y * kEquirectWidth + x) * 4u;
            buf[idx + 0] = r;
            buf[idx + 1] = g;
            buf[idx + 2] = b;
            buf[idx + 3] = 1.0f;
        }
    }
}

} // namespace helloibl
