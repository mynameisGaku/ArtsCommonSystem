// SPDX-License-Identifier: Apache-2.0
// HelloSpritePhysics — スプライトの凸包コライダーを CCollisionWorld2D に
// polygon shape として登録し、実際に当たり判定 (overlap / raycast) させる検証
// コンソール。alpha → 凸包 → 物理ワールド統合を assert で確認。ヘッドレス完結。
#include "collision/SpriteCollider.h"
#include "gameframework/CollisionWorld2D.h"
#include "math/Collision2D.h"

#include <cstdio>

using namespace acs;
using acs::game::CCollisionWorld2D;
using acs::game::FShapeId;

namespace {
constexpr u32 kW = 128, kH = 128;
void FillCircle(u8* img, f32 cx, f32 cy, f32 r) noexcept {
    for (u32 i = 0; i < kW * kH * 4u; ++i) img[i] = 0;
    for (u32 y = 0; y < kH; ++y)
        for (u32 x = 0; x < kW; ++x) {
            const f32 dx = static_cast<f32>(x) - cx, dy = static_cast<f32>(y) - cy;
            if (dx * dx + dy * dy <= r * r) {
                const u32 idx = (y * kW + x) * 4u;
                img[idx + 0] = img[idx + 1] = img[idx + 2] = img[idx + 3] = 255;
            }
        }
}
}

int main() {
    std::puts("=== ACS HelloSpritePhysics ===");
    static u8 img[kW * kH * 4];
    FillCircle(img, 64.0f, 64.0f, 40.0f);

    CSpriteCollider sc;
    if (sc.BuildFromAlpha(img, kW, kH, 128, 2.0f).IsErr()) { std::puts("collider FAILED"); return 2; }
    const FConvexPoly2 hull = sc.HullPolygon();

    bool ok = hull.count >= 3 && hull.count <= FConvexPoly2::kMaxVerts;

    CCollisionWorld2D world;
    world.Init(64.0f);
    const FShapeId poly_id = world.AddPolygon(hull);
    const FShapeId wall_id = world.AddAabb(FAabb2{ {300, 300}, {20, 20} });

    TArray<FShapeId> hits;

    // (1) 凸包中心に重なる円 → poly に当たる
    world.OverlapCircle(FCircle{ {64, 64}, 8.0f }, hits);
    const bool t1 = (hits.Size() == 1 && hits[0] == poly_id);

    // (2) 何も無い所の円 → 0 件
    world.OverlapCircle(FCircle{ {64, 260}, 8.0f }, hits);
    const bool t2 = (hits.Size() == 0);

    // (3) 凸包に重なる小ボックス poly → poly に当たる
    FConvexPoly2 probe;
    probe.Add({ 60, 60 }); probe.Add({ 72, 60 }); probe.Add({ 72, 72 }); probe.Add({ 60, 72 });
    world.OverlapPolygon(probe, hits);
    const bool t3 = (hits.Size() == 1 && hits[0] == poly_id);

    // (4) 上からレイ → 凸包の上側 (y < 64) にヒット
    FRayHit2 rh; FShapeId hid;
    const bool hit = world.Raycast(FRay2{ {64, -20}, {0, 1} }, 500.0f, rh, hid);
    const bool t4 = hit && hid == poly_id && rh.point.y < 64.0f && rh.point.y > 10.0f;

    // (5) 壁 (AABB) も poly クエリで当たる
    FConvexPoly2 wall_probe;
    wall_probe.Add({ 290, 290 }); wall_probe.Add({ 310, 290 });
    wall_probe.Add({ 310, 310 }); wall_probe.Add({ 290, 310 });
    world.OverlapPolygon(wall_probe, hits);
    const bool t5 = (hits.Size() == 1 && hits[0] == wall_id);

    std::printf("  hull verts=%u  shapes=%u\n", hull.count, world.ShapeCount());
    std::printf("  overlapCircle(center)=%d  overlapCircle(empty)=%d  overlapPoly=%d\n",
                t1 ? 1 : 0, t2 ? 1 : 0, t3 ? 1 : 0);
    std::printf("  raycast hit=%d y=%.1f  wall overlap=%d\n",
                hit ? 1 : 0, rh.point.y, t5 ? 1 : 0);

    ok = ok && t1 && t2 && t3 && t4 && t5;
    std::printf("\nresult: %s\n", ok ? "ALL PASS" : "FAILED");
    return ok ? 0 : 1;
}
