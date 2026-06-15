// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: FSpawn2DSubsystem の検証 (GPU 非依存)。
//   ・Owner()(= FScene2D)経由でシーン root へプレハブを生成し、指定位置へ配置する
//   ・= サブシステムが «世界に手が届く»(Owner コンテキスト)の実証
//   ・owner 未設定(FScene2D でない)なら nullptr
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/Scene2D.h"
#include "gameframework/Node2D.h"

using namespace acs;
using namespace acs::game;

namespace {
// 2 ノードのプレハブ(Bullet + 子 Tip)。
const char* kBullet =
    "ACSCENE v1\n"
    "2\n"
    "1 -1 0 0 0 1 1 48 0.5 0.6 0.7 1 Bullet\n"
    "2 1 5 0 0 1 1 24 0.9 0.2 0.2 1 Tip\n"
    "SEL -1 0\n";
} // namespace

// Owner(FScene2D)の root へプレハブを生成し、指定位置に配置する。
ACS_TEST(SpawnSubsystem, SpawnsIntoSceneRootAtPosition) {
    FScene2D scene;
    FSpawn2DSubsystem spawner;
    spawner._SetOwner(&scene);                         // World サブシステムの owner = Scene

    const u32 before = scene.Root().ChildCount();
    FNode2D* n = spawner.SpawnPrefabText(kBullet, FVec2{ 100.0f, 50.0f });
    EXPECT_TRUE(n != nullptr);
    EXPECT_TRUE(n->Parent() == &scene.Root());         // シーン root の子として生成
    EXPECT_EQ(scene.Root().ChildCount(), before + 1u);
    EXPECT_TRUE(n->Local().position.x == 100.0f && n->Local().position.y == 50.0f);  // 指定位置
    EXPECT_EQ(n->ChildCount(), 1u);                    // 子 Tip も生成
    EXPECT_EQ(n->SerialId(), 1);                       // SerialId 復元(参照解決の土台)

    // 複数スポーンは独立に積み上がる。
    FNode2D* m = spawner.SpawnPrefabText(kBullet, FVec2{ 0.0f, 0.0f });
    EXPECT_TRUE(m != nullptr && m != n);
    EXPECT_EQ(scene.Root().ChildCount(), before + 2u);
}

// owner が FScene2D でない(未設定)なら nullptr で安全。
ACS_TEST(SpawnSubsystem, NoOwnerIsSafe) {
    FSpawn2DSubsystem orphan;                          // owner 未設定
    EXPECT_TRUE(orphan.SpawnPrefabText(kBullet, FVec2{ 0.0f, 0.0f }) == nullptr);
    EXPECT_TRUE(orphan.SpawnPrefabText(nullptr, FVec2{ 0.0f, 0.0f }) == nullptr);
}
