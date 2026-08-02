// SPDX-License-Identifier: Apache-2.0
// CCollisionWorld2D (Pillar F) の動作確認テスト — 特に巨大 extent の退避経路
//
// 巨大 (1e30 級) の形状 / クエリはセル範囲が i32 全域にクランプされ、修正前は
// セル二重ループが事実上終わらなかった (フリーズ)。修正後は形状側が
// m_HugeShapes へ退避され、クエリ側は全 slot 線形走査へフォールバックする。
// 本テストが「数秒で完走する」こと自体が回帰の検出になる。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/CollisionWorld2D.h"

using namespace acs;
using namespace acs::game;

namespace {

/** out に id が含まれるか。 */
bool Contains(const TArray<FShapeId>& out, FShapeId id) noexcept
{
    for (u32 i = 0; i < out.Size(); ++i) {
        if (out[i] == id) return true;
    }
    return false;
}

} // namespace

ACS_TEST(CollisionWorld2D, BasicOverlapAndRemove) {
    CCollisionWorld2D w;
    w.Init(64.0f);
    const FShapeId box  = w.AddAabb(FAabb2{ {100, 0}, {16, 16} });
    const FShapeId ball = w.AddCircle(FCircle{ {0, 0}, 16.0f });
    EXPECT_EQ(w.ShapeCount(), 2u);

    TArray<FShapeId> hits;
    w.OverlapAabb(FAabb2{ {0, 0}, {8, 8} }, hits);
    EXPECT_TRUE(Contains(hits, ball));
    EXPECT_FALSE(Contains(hits, box));

    w.Remove(ball);
    w.OverlapAabb(FAabb2{ {0, 0}, {8, 8} }, hits);
    EXPECT_EQ(hits.Size(), 0u);
}

ACS_TEST(CollisionWorld2D, HugeShapeGoesToOverflowListAndIsStillFound) {
    CCollisionWorld2D w;
    w.Init(64.0f);
    // セル範囲が i32 全域にクランプされる巨大 AABB。修正前はグリッド挿入の
    // 二重ループが終わらずここでフリーズしていた。
    const FShapeId huge  = w.AddAabb(FAabb2{ {0, 0}, {1e30f, 1e30f} });
    const FShapeId small = w.AddAabb(FAabb2{ {10, 10}, {4, 4} });

    // 小さいクエリでも巨大形状は退避リスト経由で必ず候補に入る。
    TArray<FShapeId> hits;
    w.OverlapAabb(FAabb2{ {10, 10}, {1, 1} }, hits);
    EXPECT_TRUE(Contains(hits, huge));
    EXPECT_TRUE(Contains(hits, small));

    // 巨大形状が居てもレイキャストは完走して何かに当たる (small は確実に射線上)。
    FRayHit2 rh;
    FShapeId hit_id;
    EXPECT_TRUE(w.Raycast(FRay2{ {10, -50}, {0, 1} }, 100.0f, rh, hit_id));

    // 除去後は候補から消える。
    w.Remove(huge);
    w.OverlapAabb(FAabb2{ {10, 10}, {1, 1} }, hits);
    EXPECT_FALSE(Contains(hits, huge));
    EXPECT_TRUE(Contains(hits, small));
}

ACS_TEST(CollisionWorld2D, HugeQueryFallsBackToLinearScan) {
    CCollisionWorld2D w;
    w.Init(64.0f);
    const FShapeId a = w.AddAabb(FAabb2{ { 1000,  1000}, {8, 8} });
    const FShapeId b = w.AddAabb(FAabb2{ {-1000, -1000}, {8, 8} });

    // クエリ範囲が巨大でも全 slot 線形走査で完走し、全形状を正しく返す。
    TArray<FShapeId> hits;
    w.OverlapAabb(FAabb2{ {0, 0}, {1e30f, 1e30f} }, hits);
    EXPECT_TRUE(Contains(hits, a));
    EXPECT_TRUE(Contains(hits, b));
    EXPECT_EQ(hits.Size(), 2u);

    // 巨大円クエリも同様に完走する。
    w.OverlapCircle(FCircle{ {0, 0}, 1e30f }, hits);
    EXPECT_TRUE(Contains(hits, a));
    EXPECT_TRUE(Contains(hits, b));
}
