// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: 実行時プレハブ生成 (SpawnPrefabText) の検証 (GPU 非依存)。
//   ・.acsprefab テキスト(サブツリーの ACSCENE 直列化)を parent 配下へ生成し、
//     生成サブツリーのルートを返す
//   ・親子構造 / transform / SerialId(= オブジェクト参照の解決キー)が復元される
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ANode.h"
#include "gameframework/PrefabSystem.h"
#include "gameframework/SceneTextLoader.h"

using namespace acs;
using namespace acs::game;

// 2 ノードの最小プレハブ: PrefRoot(id1) と その子 PrefChild(id2)。
// 行: id parent x y rot sx sy base r g b a name
static const char* kPrefab =
    "ACSCENE v1\n"
    "2\n"
    "1 -1 10 20 0 1 1 48 0.5 0.6 0.7 1 PrefRoot\n"
    "2 1 5 3 0 1 1 48 0.2 0.3 0.4 1 PrefChild\n"
    "SEL -1 0\n";

/** FPrefabSystem の世代 ID 検証に使う最小 factory。 */
static TObjectPtr<ANode> SpawnEmptyNode(void*) noexcept {
    return NewObject<ANode>();
}

ACS_TEST(PrefabSpawn, SpawnReturnsRootWithChildren) {
    ANode scene;
    const u32 before = scene.ChildCount();

    ANode* root = SpawnPrefabText(kPrefab, scene);
    EXPECT_TRUE(root != nullptr);

    // 生成ルートは scene の «新しい» 子。
    EXPECT_TRUE(root->Parent() == &scene);
    EXPECT_EQ(scene.ChildCount(), before + 1u);

    // ルートの transform が復元される。
    EXPECT_TRUE(root->Position2D().x == 10.0f && root->Position2D().y == 20.0f);

    // 子 1 個 (PrefChild) が付く。
    EXPECT_EQ(root->ChildCount(), 1u);
    ANode* child = root->Child(0);
    EXPECT_TRUE(child != nullptr);
    EXPECT_TRUE(child->Position2D().x == 5.0f && child->Position2D().y == 3.0f);

    // SerialId が .acsprefab の id から復元される(= オブジェクト参照の解決キー)。
    EXPECT_EQ(root->SerialId(),  1);
    EXPECT_EQ(child->SerialId(), 2);
}

ACS_TEST(PrefabSpawn, NullAndEmptyAreSafe) {
    ANode scene;
    EXPECT_TRUE(SpawnPrefabText(nullptr, scene) == nullptr);
    EXPECT_TRUE(SpawnPrefabText("", scene) == nullptr);               // ヘッダ無し → nullptr
    EXPECT_EQ(scene.ChildCount(), 0u);
}

// 複数回 spawn すると独立したサブツリーが積み上がる(敵/弾の量産)。
ACS_TEST(PrefabSpawn, MultipleSpawnsAreIndependent) {
    ANode scene;
    ANode* a = SpawnPrefabText(kPrefab, scene);
    ANode* b = SpawnPrefabText(kPrefab, scene);
    EXPECT_TRUE(a != nullptr && b != nullptr && a != b);
    EXPECT_EQ(scene.ChildCount(), 2u);
    a->SetPosition2D(FVec2{ 999.0f, 0.0f });          // 片方を動かしても
    EXPECT_TRUE(b->Position2D().x == 10.0f);          // 他方は不変(独立)
}

ACS_TEST(PrefabSystem, ClearAllKeepsOldIdsStaleAfterReregister) {
    FPrefabSystem prefabs;
    const FPrefabId old_id =
        prefabs.Register("BeforeClear", &SpawnEmptyNode);
    EXPECT_TRUE(old_id.IsValid());
    EXPECT_EQ(prefabs.Count(), 1u);

    prefabs.ClearAll();
    EXPECT_EQ(prefabs.Count(), 0u);
    EXPECT_FALSE(prefabs.Spawn(old_id).IsValid());

    const FPrefabId replacement_id =
        prefabs.Register("AfterClear", &SpawnEmptyNode);
    EXPECT_TRUE(replacement_id.IsValid());
    EXPECT_EQ(replacement_id.Index(), old_id.Index());
    EXPECT_FALSE(replacement_id == old_id);
    EXPECT_FALSE(prefabs.Spawn(old_id).IsValid());
    EXPECT_TRUE(prefabs.Spawn(replacement_id).IsValid());
}
