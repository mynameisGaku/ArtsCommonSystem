// SPDX-License-Identifier: Apache-2.0
// HelloMeshCollider — 3D メッシュから三角形 BVH コライダーを構築して検証する
// コンソールサンプル。Primitive の cube / sphere で collider を作り、レイキャスト
// (ヒット位置・距離・法線) と三角形数・AABB をアサートする。ヘッドレスで完結。
#include "collision/MeshCollider.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "memory/SharedPtr.h"

#include <cstdio>
#include <cmath>

using namespace acs;

namespace {
bool Near(f32 a, f32 b, f32 eps) noexcept { return std::fabs(a - b) < eps; }
}

int main() {
    std::puts("=== ACS HelloMeshCollider ===");
    bool ok = true;

    // ---- (1) Cube ----
    TSharedPtr<FMeshAsset> cube = Primitive::MakeCube(2.0f);
    FMeshCollider col;
    if (col.BuildFromMesh(*cube).IsErr()) { std::puts("cube: build FAILED"); return 2; }

    const Aabb3 b   = col.Bounds();
    const f32   top = b.Max().y;
    const f32   right = b.Max().x;

    // 上から下へ中心を貫くレイ → 上面 (y=top) にヒット、法線 +Y
    RayHit3 down = col.Raycast(Ray3{ {b.center.x, top + 4.0f, b.center.z}, {0, -1, 0} });
    const bool hit_down = down.hit && Near(down.t, 4.0f, 0.01f) &&
                          Near(down.point.y, top, 0.01f) && down.normal.y > 0.9f;

    // 同じ起点で上向き (メッシュから離れる) → ヒットなし
    const bool miss_up = !col.Raycast(Ray3{ {b.center.x, top + 4.0f, b.center.z}, {0, 1, 0} }).hit;

    // 右から左 → 右面 (x=right) にヒット、法線 +X
    RayHit3 side = col.Raycast(Ray3{ {right + 4.0f, b.center.y, b.center.z}, {-1, 0, 0} });
    const bool hit_side = side.hit && Near(side.point.x, right, 0.01f) && side.normal.x > 0.9f;

    const bool tri_ok = col.TriangleCount() == 12;   // cube = 6 面 × 2

    std::printf("  cube : tris=%u nodes=%u bounds(hs=%.1f,%.1f,%.1f)\n",
                col.TriangleCount(), col.NodeCount(),
                b.half_size.x, b.half_size.y, b.half_size.z);
    std::printf("         down: hit=%d t=%.2f y=%.2f ny=%.2f | up miss=%d | side hit=%d nx=%.2f\n",
                down.hit ? 1 : 0, down.t, down.point.y, down.normal.y,
                miss_up ? 1 : 0, side.hit ? 1 : 0, side.normal.x);
    ok = ok && hit_down && miss_up && hit_side && tri_ok;

    // ---- (2) Sphere ----
    TSharedPtr<FMeshAsset> sph = Primitive::MakeSphere(1.0f, 32, 16);
    FMeshCollider scol;
    if (scol.BuildFromMesh(*sph).IsErr()) { std::puts("sphere: build FAILED"); return 3; }
    RayHit3 sh = scol.Raycast(Ray3{ {0, 5, 0}, {0, -1, 0} });
    // 半径 1 の球の頂点 → 上端は y≈1 (三角形分割の誤差を許容)
    const bool sphere_ok = sh.hit && Near(sh.point.y, 1.0f, 0.05f) && sh.normal.y > 0.7f;
    std::printf("  sphere: tris=%u hit=%d y=%.3f ny=%.2f\n",
                scol.TriangleCount(), sh.hit ? 1 : 0, sh.point.y, sh.normal.y);
    ok = ok && sphere_ok;

    std::printf("\nresult: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
