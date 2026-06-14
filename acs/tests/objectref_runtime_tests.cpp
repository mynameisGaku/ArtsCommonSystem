// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: オブジェクト参照の «実行時解決 + 追従» の検証 (GPU 非依存)。
//   ・FNode2D::FindBySerialId が subtree から直列化 ID 一致ノードを返す
//   ・FFollow2DComponent が target ID を生きた FNode2D* へ解決し、speed で追従する
//     (= エディタで «参照を渡した» 値が実行時に効く)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Node2D.h"
#include "gameframework/Follow2DComponent.h"
#include "memory/UniquePtr.h"

#include <cmath>

using namespace acs;
using namespace acs::game;

// SerialId による subtree 検索(子孫も辿る)。
ACS_TEST(ObjectRef, FindBySerialId) {
    FNode2D root;
    FNode2D& a = root.AddChild(MakeUnique<FNode2D>()); a._SetSerialId(5);
    FNode2D& b = root.AddChild(MakeUnique<FNode2D>()); b._SetSerialId(6);
    FNode2D& c = b.AddChild(MakeUnique<FNode2D>());    c._SetSerialId(7);
    EXPECT_TRUE(root.FindBySerialId(5) == &a);
    EXPECT_TRUE(root.FindBySerialId(6) == &b);
    EXPECT_TRUE(root.FindBySerialId(7) == &c);   // 子孫も探す
    EXPECT_TRUE(root.FindBySerialId(99) == nullptr);
    EXPECT_TRUE(root.FindBySerialId(-1) == nullptr);
}

// FFollow2DComponent が target ID を解決して追従する(エディタで渡した参照が実行時に効く)。
ACS_TEST(ObjectRef, FollowResolvesAndMoves) {
    FNode2D root;
    FNode2D& target = root.AddChild(MakeUnique<FNode2D>());
    target._SetSerialId(5);
    target.Local().position = FVec2{ 100.0f, 0.0f };

    FNode2D& follower = root.AddChild(MakeUnique<FNode2D>());
    follower._SetSerialId(6);
    follower.Local().position = FVec2{ 0.0f, 0.0f };
    FFollow2DComponent& f = follower.AddComponent<FFollow2DComponent>();
    f.target = 5;        // SerialId 5 (= target) を参照
    f.speed  = 10.0f;

    // 1 tick (dt=1) → target 方向へ 10 units 近づく。
    f.OnUpdate(1.0f);
    EXPECT_TRUE(std::abs(follower.Local().position.x - 10.0f) < 1e-3f);
    EXPECT_TRUE(std::abs(follower.Local().position.y) < 1e-3f);

    // 十分 tick すると到達して止まる(行き過ぎない)。
    for (int i = 0; i < 20; ++i) f.OnUpdate(1.0f);
    EXPECT_TRUE(std::abs(follower.Local().position.x - 100.0f) < 1e-3f);

    // target=-1(なし)なら動かない。
    follower.Local().position = FVec2{ 0.0f, 0.0f };
    f.target = -1;
    f.OnUpdate(1.0f);
    EXPECT_TRUE(follower.Local().position.x == 0.0f);
}
