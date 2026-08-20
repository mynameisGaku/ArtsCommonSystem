// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: オブジェクト参照の «実行時解決 + 追従» の検証 (GPU 非依存)。
//   ・ANode::FindBySerialId が subtree から直列化 ID 一致ノードを返す
//   ・AFollow2DComponent が target ID を生きた ANode* へ解決し、speed で追従する
//     (= エディタで «参照を渡した» 値が実行時に効く)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ANode.h"
#include "gameframework/Follow2DComponent.h"
#include "memory/UniquePtr.h"

#include <cmath>

using namespace acs;
using namespace acs::game;

// SerialId による subtree 検索(子孫も辿る)。
ACS_TEST(ObjectRef, FindBySerialId) {
    ANode root;
    ANode& a = root.AddChild(NewObject<ANode>()); a.ManagementAccess().SetSerialId(5);
    ANode& b = root.AddChild(NewObject<ANode>()); b.ManagementAccess().SetSerialId(6);
    ANode& c = b.AddChild(NewObject<ANode>());    c.ManagementAccess().SetSerialId(7);
    EXPECT_TRUE(root.FindBySerialId(5) == &a);
    EXPECT_TRUE(root.FindBySerialId(6) == &b);
    EXPECT_TRUE(root.FindBySerialId(7) == &c);   // 子孫も探す
    EXPECT_TRUE(root.FindBySerialId(99) == nullptr);
    EXPECT_TRUE(root.FindBySerialId(-1) == nullptr);
}

// AFollow2DComponent が target ID を解決して追従する(エディタで渡した参照が実行時に効く)。
ACS_TEST(ObjectRef, FollowResolvesAndMoves) {
    ANode root;
    ANode& target = root.AddChild(NewObject<ANode>());
    target.ManagementAccess().SetSerialId(5);
    target.SetPosition2D(FVec2{ 100.0f, 0.0f });

    ANode& follower = root.AddChild(NewObject<ANode>());
    follower.ManagementAccess().SetSerialId(6);
    follower.SetPosition2D(FVec2{ 0.0f, 0.0f });
    AFollow2DComponent& f = follower.AddComponent<AFollow2DComponent>();
    f.target = 5;        // SerialId 5 (= target) を参照
    f.speed  = 10.0f;

    // 1 tick (dt=1) → target 方向へ 10 units 近づく。
    f.OnUpdate(1.0f);
    EXPECT_TRUE(std::abs(follower.Position2D().x - 10.0f) < 1e-3f);
    EXPECT_TRUE(std::abs(follower.Position2D().y) < 1e-3f);

    // 十分 tick すると到達して止まる(行き過ぎない)。
    for (int i = 0; i < 20; ++i) f.OnUpdate(1.0f);
    EXPECT_TRUE(std::abs(follower.Position2D().x - 100.0f) < 1e-3f);

    // target=-1(なし)なら動かない。
    follower.SetPosition2D(FVec2{ 0.0f, 0.0f });
    f.target = -1;
    f.OnUpdate(1.0f);
    EXPECT_TRUE(follower.Position2D().x == 0.0f);
}
