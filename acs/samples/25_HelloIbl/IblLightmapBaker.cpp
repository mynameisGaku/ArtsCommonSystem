// SPDX-License-Identifier: Apache-2.0
// HelloIbl — 床用 lightmap CPU baker の実装。
#include "IblLightmapBaker.h"

#include "render/RenderAssets.h"
#include "container/Array.h"
#include "math/Vec.h"
#include "math/Math.h"

using namespace acs;

namespace helloibl {

TResult<TUniquePtr<IRhiTexture>> BakeFloorLightmap(IRhiDevice& dev) noexcept {
    constexpr u32 kSize = 256;
    TArray<u8> rgba; rgba.Resize(static_cast<usize>(kSize) * kSize * 4u);

    // 球グリッドのパラメータ (OnCustomFrame の draw 計算と一致)
    constexpr u32 kGrid = 5;
    constexpr f32 kSpacing = 1.4f;
    constexpr f32 kSphereR = 0.55f;
    constexpr f32 kCenterY = 2.5f;       // py = (gy - 2)*1.4 + 2.5
    constexpr f32 kCenterZ = 3.0f;
    constexpr f32 kPlaneY = -0.6f;
    constexpr f32 kPlaneSize = 40.0f;    // MakePlane(40,40)
    const FVec3 kSkyColor{0.85f, 0.92f, 1.0f};   // 弱い暖青 (Day sky horizon 近似)

    for (u32 y = 0; y < kSize; ++y) {
        for (u32 x = 0; x < kSize; ++x) {
            const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kSize);
            const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kSize);
            // FPlane UV は [0,1] が plane center (40x40) を覆う想定。
            // MakePlane の uv 規約に合わせ、中心 (0, kPlaneY, kCenterZ) からの XZ。
            const f32 wx = (u - 0.5f) * kPlaneSize;
            const f32 wz = (v - 0.5f) * kPlaneSize + kCenterZ;

            // 8 方向の hemisphere ray を analytical でテスト (上半球、jitter なし)
            constexpr u32 kRays = 8;
            u32 hit_count = 0;
            for (u32 r = 0; r < kRays; ++r) {
                // hemisphere direction (上半球の固定方向)
                const f32 ang = static_cast<f32>(r) * (2.0f * kPi / static_cast<f32>(kRays));
                const f32 spread = 0.7f;        // sin(45°) 程度の広がり
                const FVec3 rd{spread * Cos(ang), 1.0f - spread * 0.5f, spread * Sin(ang)};
                // 球グリッド 25 個と FRay-FSphere intersection
                bool hit_any = false;
                for (u32 gy = 0; gy < kGrid && !hit_any; ++gy) {
                    for (u32 gx = 0; gx < kGrid && !hit_any; ++gx) {
                        const f32 px = (static_cast<f32>(gx) - 2.0f) * kSpacing;
                        const f32 py = (static_cast<f32>(gy) - 2.0f) * kSpacing + kCenterY;
                        const FVec3 sp{px, py, kCenterZ};
                        const FVec3 oc{wx - sp.x, kPlaneY - sp.y, wz - sp.z};
                        const f32 b = oc.x*rd.x + oc.y*rd.y + oc.z*rd.z;
                        const f32 c = oc.x*oc.x + oc.y*oc.y + oc.z*oc.z - kSphereR*kSphereR;
                        const f32 disc = b*b - c;
                        if (disc > 0.0f) {
                            const f32 t = -b - Sqrt(disc);
                            if (t > 0.01f && t < 20.0f) hit_any = true;
                        }
                    }
                }
                if (hit_any) ++hit_count;
            }
            const f32 vis = 1.0f - static_cast<f32>(hit_count) / static_cast<f32>(kRays);
            // 結果は sky_color * visibility (RGB)。8-bit に量子化。
            const FVec3 lm{kSkyColor.x * vis, kSkyColor.y * vis, kSkyColor.z * vis};
            const usize idx = (static_cast<usize>(y) * kSize + x) * 4u;
            rgba[idx + 0] = static_cast<u8>(Saturate(lm.x) * 255.0f);
            rgba[idx + 1] = static_cast<u8>(Saturate(lm.y) * 255.0f);
            rgba[idx + 2] = static_cast<u8>(Saturate(lm.z) * 255.0f);
            rgba[idx + 3] = 255;
        }
    }

    TextureDesc td{};
    td.width = kSize; td.height = kSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = rgba.Data();
    td.initial_data_size = rgba.Size();
    auto r = CreateRhiTexture(dev, td);
    if (r.IsErr()) {
        return Err<TUniquePtr<IRhiTexture>>(r.Error());
    }
    return Ok(Move(r.Value()));
}

} // namespace helloibl
