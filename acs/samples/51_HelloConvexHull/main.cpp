// SPDX-License-Identifier: Apache-2.0
// HelloConvexHull — 点群/メッシュから 3D 凸包を生成して検証するコンソール。
// cube の頂点 → 凸包 8 頂点 / 12 三角形、内部点が除外されること、生成した凸包を
// CMeshCollider に載せてレイキャストできることを assert する。ヘッドレス完結。
#include "collision/ConvexHull3.h"
#include "collision/MeshCollider.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "memory/SharedPtr.h"
#include "container/Array.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace { bool Near(f32 a, f32 b, f32 e) noexcept { return std::fabs(a - b) < e; } }

int main() {
    std::puts("=== ACS HelloConvexHull ===");
    bool ok = true;

    // ---- (1) cube メッシュ頂点 (24 個・8 位置) → 凸包 8 頂点 / 12 三角形 ----
    TSharedPtr<FMeshAsset> cube = Primitive::MakeCube(2.0f);
    TArray<FVec3> cube_pos;
    for (usize i = 0; i < cube->Vertices().Size(); ++i) cube_pos.PushBack(cube->Vertices()[i].position);

    TArray<FVec3> hv; TArray<u32> hi;
    if (BuildConvexHull3(cube_pos.Data(), static_cast<u32>(cube_pos.Size()), hv, hi).IsErr()) {
        std::puts("cube hull FAILED"); return 2;
    }
    const bool cube_ok = (hv.Size() == 8) && (hi.Size() == 36);  // 8 頂点, 12 三角形
    std::printf("  cube hull: verts=%u tris=%u (expect 8 / 12)\n",
                static_cast<u32>(hv.Size()), static_cast<u32>(hi.Size() / 3));
    ok = ok && cube_ok;

    // 生成した凸包を collider にしてレイキャスト (上面 y=1 に命中)
    CMeshCollider hull_col;
    if (hull_col.BuildFromTriangles(hv.Data(), static_cast<u32>(hv.Size()),
                                    hi.Data(), static_cast<u32>(hi.Size())).IsOk()) {
        FRayHit3 h = hull_col.Raycast(FRay3{ {0, 5, 0}, {0, -1, 0} });
        const bool ray_ok = h.hit && Near(h.point.y, 1.0f, 0.01f);
        std::printf("  hull collider: raycast hit=%d y=%.2f\n", h.hit ? 1 : 0, h.point.y);
        ok = ok && ray_ok;
    } else { std::puts("hull collider build FAILED"); ok = false; }

    // ---- (2) 8 隅 + 内部点 → 内部点が凸包から除外される ----
    FVec3 pts[9] = {
        {-1,-1,-1},{1,-1,-1},{1,1,-1},{-1,1,-1},
        {-1,-1, 1},{1,-1, 1},{1,1, 1},{-1,1, 1},
        {0,0,0}      // 内部点
    };
    TArray<FVec3> hv2; TArray<u32> hi2;
    if (BuildConvexHull3(pts, 9, hv2, hi2).IsErr()) { std::puts("box hull FAILED"); return 3; }
    const bool box_ok = (hv2.Size() == 8) && (hi2.Size() == 36);  // 内部点除外 → 8 頂点
    std::printf("  box+interior: verts=%u tris=%u (interior excluded -> 8 / 12)\n",
                static_cast<u32>(hv2.Size()), static_cast<u32>(hi2.Size() / 3));
    ok = ok && box_ok;

    std::printf("\nresult: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
