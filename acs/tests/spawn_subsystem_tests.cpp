// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: ASpawn2DSubsystem の検証 (GPU 非依存)。
//   ・Owner()(= AScene)経由でシーン root へプレハブを生成し、指定位置へ配置する
//   ・= サブシステムが «世界に手が届く»(Owner コンテキスト)の実証
//   ・owner 未設定(AScene でない)なら nullptr
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Spawn2DSubsystem.h"
#include "gameframework/Scene.h"
#include "gameframework/SubsystemCatalog.h"
#include "gameframework/ANode.h"

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

// Owner(AScene)の root へプレハブを生成し、指定位置に配置する。
ACS_TEST(SpawnSubsystem, SpawnsIntoSceneRootAtPosition) {
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    AScene scene;
    EXPECT_TRUE(scene._InitWorldSubsystems(nullptr));
    ASpawn2DSubsystem* spawner = scene.GetSubsystem<ASpawn2DSubsystem>();
    EXPECT_TRUE(spawner != nullptr);
    if (spawner == nullptr) return;

    const u32 before = scene.Root().ChildCount();
    ANode* n = spawner->SpawnPrefabText(kBullet, FVec2{ 100.0f, 50.0f });
    EXPECT_TRUE(n != nullptr);
    EXPECT_TRUE(n->Parent() == &scene.Root());         // シーン root の子として生成
    EXPECT_EQ(scene.Root().ChildCount(), before + 1u);
    EXPECT_TRUE(n->Position2D().x == 100.0f && n->Position2D().y == 50.0f);  // 指定位置
    EXPECT_EQ(n->ChildCount(), 1u);                    // 子 Tip も生成
    EXPECT_EQ(n->SerialId(), 1);                       // SerialId 復元(参照解決の土台)

    // 複数スポーンは独立に積み上がる。
    ANode* m = spawner->SpawnPrefabText(kBullet, FVec2{ 0.0f, 0.0f });
    EXPECT_TRUE(m != nullptr && m != n);
    EXPECT_EQ(scene.Root().ChildCount(), before + 2u);
}

// owner が scene owner でない(未設定)なら nullptr で安全。
ACS_TEST(SpawnSubsystem, NoOwnerIsSafe) {
    ASpawn2DSubsystem orphan;                          // owner 未設定
    EXPECT_TRUE(orphan.SpawnPrefabText(kBullet, FVec2{ 0.0f, 0.0f }) == nullptr);
    EXPECT_TRUE(orphan.SpawnPrefabText(nullptr, FVec2{ 0.0f, 0.0f }) == nullptr);

    int wrong_owner = 0;
    orphan.ManagementAccess().SetOwner(&wrong_owner);
    EXPECT_TRUE(orphan.SpawnPrefabText(kBullet, FVec2{ 0.0f, 0.0f }) == nullptr);
}

/** シーン統一後は素の AScene も root を持つので、生成先へ接続され spawn できる。 */
ACS_TEST(SpawnSubsystem, PlainSceneOwnerSpawnsIntoRoot)
{
    EXPECT_TRUE(AcsRegisterGameFrameworkSubsystems());
    AScene scene;
    EXPECT_TRUE(scene._InitWorldSubsystems(nullptr));
    ASpawn2DSubsystem* const spawner = scene.GetSubsystem<ASpawn2DSubsystem>();
    EXPECT_TRUE(spawner != nullptr);
    if (spawner != nullptr) {
        // 未 bind / 誤 owner の null 安全は OrphanOwnerIsSafe 側が独立に固定している。
        ANode* const spawned = spawner->SpawnPrefabText(kBullet, FVec2{0.0f, 0.0f});
        EXPECT_TRUE(spawned != nullptr);
        EXPECT_TRUE(scene.Root().ChildCount() > 0u);
    }
}
