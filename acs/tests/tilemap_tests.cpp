// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Tilemap (LoadTiledJson) + TilemapPhysics (BuildRigidColliders):
//   Tiled JSON の取り込みと、タイル地形→剛体コライダ生成 + 動的体が地形に乗ることを検証。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Tilemap.h"
#include "gameframework/TilemapPhysics.h"
#include "gameframework/TilemapQuery.h"
#include "gameframework/TilemapNav.h"
#include "gameframework/Pathfinding.h"
#include "gameframework/RigidWorld2D.h"
#include "math/Vec.h"
#include "container/Array.h"

#include <cstring>   // std::strlen
#include <cmath>     // std::fabs

using namespace acs;
using namespace acs::game;

// Tiled Map Editor の JSON マップを取り込む (row-major data、GID→FTileId)。
ACS_TEST(Tilemap, LoadTiledJson) {
    FTilemap m;
    const char* js =
        R"({"width":3,"height":2,"tilewidth":16,"layers":[)"
        R"({"type":"tilelayer","data":[1,0,2,0,3,0]}]})";
    const auto r = m.LoadTiledJson(js, std::strlen(js));
    EXPECT_TRUE(!r.IsErr());

    EXPECT_EQ(m.Width(), 3u);
    EXPECT_EQ(m.Height(), 2u);
    EXPECT_EQ(m.LayerCount(), 1u);
    // data row-major: row0=[1,0,2], row1=[0,3,0]。
    EXPECT_EQ(m.GetTile(0, 0, 0).value, static_cast<u16>(1));
    EXPECT_EQ(m.GetTile(2, 0, 0).value, static_cast<u16>(2));
    EXPECT_EQ(m.GetTile(1, 1, 0).value, static_cast<u16>(3));
    EXPECT_TRUE(m.GetTile(1, 0, 0).IsEmpty());
}

// 不正な JSON / tilelayer 欠如はエラー。
ACS_TEST(Tilemap, LoadTiledJsonRejectsInvalid) {
    FTilemap m;
    const char* bad = R"({"width":0,"height":0,"layers":[]})";
    EXPECT_TRUE(m.LoadTiledJson(bad, std::strlen(bad)).IsErr());
}

// 行マージ: 連続する solid タイルは 1 つの幅広 AABB にまとまる。
ACS_TEST(TilemapPhysics, GreedyRowMerge) {
    FTilemap map;
    map.Init(8, 4, 1, 1.0f);
    for (u32 x = 0; x < 8; ++x) map.SetTile(x, 3, FTileId(1), 0);   // 全幅の床 → 1 コライダ

    FRigidWorld2D world;
    EXPECT_EQ(BuildRigidColliders(map, 0, world, 0.0f, 0.5f), 1u);

    // 隙間あり: x=0,1 solid / 2 empty / 3,4 solid → 2 run → 2 コライダ。
    FTilemap map2;
    map2.Init(8, 1, 1, 1.0f);
    map2.SetTile(0, 0, FTileId(1)); map2.SetTile(1, 0, FTileId(1));
    map2.SetTile(3, 0, FTileId(1)); map2.SetTile(4, 0, FTileId(1));
    FRigidWorld2D world2;
    EXPECT_EQ(BuildRigidColliders(map2, 0, world2, 0.0f, 0.5f), 2u);
}

// 動的円がタイル地形 (生成コライダ) の上に落ちて静止する。
ACS_TEST(TilemapPhysics, BodyRestsOnTileFloor) {
    FTilemap map;
    map.Init(8, 8, 1, 1.0f);
    for (u32 x = 0; x < 8; ++x) map.SetTile(x, 5, FTileId(1), 0);   // y=5 の行が床

    FRigidWorld2D world;
    EXPECT_EQ(BuildRigidColliders(map, 0, world, 0.0f, 0.5f), 1u);

    const FVec2 floorC      = map.TileToWorld(3, 5);   // 床タイル中心
    const f32   colliderTop = floorC.y - 0.5f;          // half.y = tile/2 = 0.5
    const f32   ballR       = 0.4f;
    const u32   ball = world.AddCircle(FVec2{ floorC.x, floorC.y - 4.0f }, ballR, 1.0f, 0.0f);

    for (int i = 0; i < 600; ++i) world.Step(1.0f / 60.0f, FVec2{ 0.0f, 10.0f });   // +Y=下

    EXPECT_NEAR(world.Position(ball).y, colliderTop - ballR, 0.05f);   // 床上で静止
    EXPECT_TRUE(std::fabs(world.Velocity(ball).y) < 0.3f);
}

// グリッド DDA レイキャスト: 壁柱への命中・法線・距離、逆向き/距離外の miss、壁内即ヒット。
ACS_TEST(TilemapQuery, RaycastHitsWall) {
    FTilemap map;
    map.Init(8, 8, 1, 1.0f);
    for (u32 y = 0; y < 8; ++y) map.SetTile(5, y, FTileId(1), 0);   // x=5 の壁柱

    // (0.5,3.5) から +x → タイル(5,3) の左面 (world x=5.0) に命中、距離 4.5。
    const FTileRayHit h = RaycastTilemap(map, 0, FVec2{ 0.5f, 3.5f }, FVec2{ 1.0f, 0.0f }, 20.0f);
    EXPECT_TRUE(h.hit);
    EXPECT_EQ(h.tile_x, 5u);
    EXPECT_EQ(h.tile_y, 3u);
    EXPECT_NEAR(h.distance, 4.5f, 1e-3f);
    EXPECT_NEAR(h.point.x, 5.0f, 1e-3f);
    EXPECT_NEAR(h.normal.x, -1.0f, 1e-3f);

    // 逆向き → グリッド外へ抜けて miss。
    EXPECT_TRUE(!RaycastTilemap(map, 0, FVec2{ 0.5f, 3.5f }, FVec2{ -1.0f, 0.0f }, 20.0f).hit);
    // 距離外 (壁は 4.5 先、max 2) → miss。
    EXPECT_TRUE(!RaycastTilemap(map, 0, FVec2{ 0.5f, 3.5f }, FVec2{ 1.0f, 0.0f }, 2.0f).hit);
    // 始点が壁内 → 距離 0 で即ヒット。
    const FTileRayHit inside = RaycastTilemap(map, 0, FVec2{ 5.5f, 3.5f }, FVec2{ 1.0f, 0.0f }, 20.0f);
    EXPECT_TRUE(inside.hit);
    EXPECT_NEAR(inside.distance, 0.0f, 1e-4f);
    EXPECT_EQ(inside.tile_x, 5u);
    EXPECT_NEAR(inside.normal.x, 0.0f, 1e-4f);   // 壁内: 法線は 0 (sentinel)
    EXPECT_NEAR(inside.normal.y, 0.0f, 1e-4f);
}

// 視線判定 (line-of-sight): 間に壁が無ければ clear、有れば blocked。
ACS_TEST(TilemapQuery, LineOfSight) {
    FTilemap map;
    map.Init(10, 3, 1, 1.0f);
    // 何も無い → A(0.5,1.5)→B(9.5,1.5) は見通せる。
    const FTileRayHit clear = RaycastTilemap(map, 0, FVec2{ 0.5f, 1.5f }, FVec2{ 1.0f, 0.0f }, 9.0f);
    EXPECT_TRUE(!clear.hit);

    // x=5 に壁を立てると遮蔽される。
    map.SetTile(5, 1, FTileId(1), 0);
    const FTileRayHit blocked = RaycastTilemap(map, 0, FVec2{ 0.5f, 1.5f }, FVec2{ 1.0f, 0.0f }, 9.0f);
    EXPECT_TRUE(blocked.hit);
    EXPECT_EQ(blocked.tile_x, 5u);
}

// タイル地形上の A* 経路探索: 壁を迂回し、障害物上に乗らない。
ACS_TEST(TilemapNav, PathfindAroundWall) {
    FTilemap map;
    map.Init(10, 5, 1, 1.0f);
    for (u32 y = 0; y < 4; ++y) map.SetTile(5, y, FTileId(1), 0);   // 壁 x=5, y=0..3 (y=4 が隙間)

    NavGrid nav;
    BuildNavGridFromTilemap(map, 0, nav);

    TArray<FVec2> path;
    const bool found = FindTilemapPath(map, nav, map.TileToWorld(0, 0), map.TileToWorld(9, 0), path);
    EXPECT_TRUE(found);
    EXPECT_TRUE(path.Size() > 0u);
    if (path.Size() > 0u) {
        EXPECT_NEAR(path[0].x, map.TileToWorld(0, 0).x, 1e-3f);                    // 始点
        EXPECT_NEAR(path[path.Size() - 1u].x, map.TileToWorld(9, 0).x, 1e-3f);     // 終点
    }
    // どの waypoint も壁タイル上に乗らない。
    for (u32 i = 0; i < path.Size(); ++i) {
        u32 tx, ty;
        EXPECT_TRUE(map.WorldToTile(path[i], tx, ty));
        EXPECT_TRUE(map.GetTile(tx, ty, 0).IsEmpty());
    }
}

// 通路が完全に塞がれていれば経路なし。
ACS_TEST(TilemapNav, NoPathWhenFullyBlocked) {
    FTilemap map;
    map.Init(10, 5, 1, 1.0f);
    for (u32 y = 0; y < 5; ++y) map.SetTile(5, y, FTileId(1), 0);   // 全行を塞ぐ

    NavGrid nav;
    BuildNavGridFromTilemap(map, 0, nav);

    TArray<FVec2> path;
    path.PushBack(FVec2{ 99.0f, 99.0f });   // 事前に汚す
    EXPECT_TRUE(!FindTilemapPath(map, nav, map.TileToWorld(0, 0), map.TileToWorld(9, 0), path));
    EXPECT_EQ(path.Size(), 0u);             // 失敗時も出力は空 (stale を残さない)
}
