// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/RigidBody2D.{h,cpp} の検証:
//   ARigidBody2D コンポーネントを ANode に付け、StepRigidBodies で物理を進めると
//   ノード自身の Local 位置が物理で駆動される (落下→静止、相互作用) ことを確認する。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/RigidBody2D.h"
#include "gameframework/RigidWorld2D.h"
#include "gameframework/ANode.h"
#include "math/Vec.h"

#include <cmath>   // std::fabs

using namespace acs;
using namespace acs::game;

// node に ARigidBody2D を付けると、物理ステップで node 自体が落ちて床上で止まる。
ACS_TEST(RigidBody2D, ComponentDrivesNodeFall) {
    CRigidWorld2D world;
    world.AddStaticAabb(FVec2{ 0.0f, 10.0f }, FVec2{ 20.0f, 0.5f }, 0.0f);   // 床上端 9.5

    auto root = NewObject<ANode>();
    ANode& crate = root->AddChild(NewObject<ANode>());
    crate.SetPosition2D(FVec2{ 0.0f, 0.0f });
    ARigidBody2D& rb = crate.AddComponent<ARigidBody2D>(world);
    rb.SetBox(FVec2{ 0.5f, 0.5f }, 1.0f, 0.0f);
    EXPECT_TRUE(rb.IsRegistered());

    for (int i = 0; i < 600; ++i) StepRigidBodies(world, *root, 1.0f / 60.0f, FVec2{ 0.0f, 10.0f });

    // node の Local 位置が物理で更新され、床上 (y~9.0) で静止する。
    EXPECT_NEAR(crate.Position2D().y, 9.0f, 0.05f);
    EXPECT_TRUE(std::fabs(rb.Velocity().y) < 0.3f);
}

// 2 つの剛体ノードが弾性衝突し、速度交換 + node 位置の分離が反映される。
ACS_TEST(RigidBody2D, TwoBodiesInteractThroughNodes) {
    CRigidWorld2D world;
    auto root = NewObject<ANode>();

    ANode& a = root->AddChild(NewObject<ANode>());
    a.SetPosition2D(FVec2{ -2.0f, 0.0f });
    ARigidBody2D& ra = a.AddComponent<ARigidBody2D>(world);
    ra.SetCircle(0.5f, 1.0f, 1.0f);
    ra.SetVelocity(FVec2{ 3.0f, 0.0f });

    ANode& b = root->AddChild(NewObject<ANode>());
    b.SetPosition2D(FVec2{ 2.0f, 0.0f });
    ARigidBody2D& rb = b.AddComponent<ARigidBody2D>(world);
    rb.SetCircle(0.5f, 1.0f, 1.0f);
    rb.SetVelocity(FVec2{ -3.0f, 0.0f });

    for (int i = 0; i < 200; ++i) StepRigidBodies(world, *root, 1.0f / 60.0f, FVec2{ 0.0f, 0.0f });

    EXPECT_NEAR(ra.Velocity().x, -3.0f, 0.3f);                       // 速度交換
    EXPECT_NEAR(rb.Velocity().x,  3.0f, 0.3f);
    EXPECT_TRUE(a.Position2D().x < b.Position2D().x);       // node も分離
}

// 再登録 (SetCircle→SetBox) は古いボディを置き換え、ghost を残さない。
ACS_TEST(RigidBody2D, ReregisterReplacesBody) {
    CRigidWorld2D world;
    auto root = NewObject<ANode>();
    ANode& n = root->AddChild(NewObject<ANode>());
    n.SetPosition2D(FVec2{ 0.0f, 0.0f });
    ARigidBody2D& rb = n.AddComponent<ARigidBody2D>(world);

    rb.SetCircle(0.5f, 1.0f);
    EXPECT_EQ(world.Count(), 1u);
    rb.SetBox(FVec2{ 0.5f, 0.5f }, 1.0f);   // 置換
    EXPECT_EQ(world.Count(), 1u);           // ghost が増えない (2 にならない)
}

// owner ノード破棄で OnDetach がボディをワールドから除去する (ghost/leak 防止)。
ACS_TEST(RigidBody2D, DestroyDeregistersBody) {
    CRigidWorld2D world;
    auto root = NewObject<ANode>();
    ANode& n = root->AddChild(NewObject<ANode>());
    ARigidBody2D& rb = n.AddComponent<ARigidBody2D>(world);
    rb.SetCircle(0.5f, 1.0f);
    EXPECT_EQ(world.Count(), 1u);

    n.Destroy();
    root->ResolveStructuralChanges();       // reap → OnDetach → RemoveBody
    EXPECT_EQ(world.Count(), 0u);           // ghost が残らない
}

// SetBox/SetCircle 前 (未登録) は同期が no-op で安全。
ACS_TEST(RigidBody2D, UnregisteredIsSafe) {
    CRigidWorld2D world;
    auto root = NewObject<ANode>();
    ANode& n = root->AddChild(NewObject<ANode>());
    n.SetPosition2D(FVec2{ 5.0f, 5.0f });
    ARigidBody2D& rb = n.AddComponent<ARigidBody2D>(world);
    EXPECT_TRUE(!rb.IsRegistered());

    StepRigidBodies(world, *root, 1.0f / 60.0f, FVec2{ 0.0f, 10.0f });   // 未登録 → no-op
    EXPECT_NEAR(n.Position2D().x, 5.0f, 1e-4f);                       // 位置不変
    EXPECT_NEAR(n.Position2D().y, 5.0f, 1e-4f);
}
