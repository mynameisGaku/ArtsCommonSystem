// SPDX-License-Identifier: Apache-2.0
// CLightCollector3D (木から光を集める) の動作確認テスト
//
// 階層を辿れること、shader の上限を越えたぶんを数えること、そして点光源が溢れたときに
// 「近い光を残す」ことを検証する。描画を伴わないのでヘッドレスで走る。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/LightCollector3D.h"

using namespace acs;
using namespace acs::game;

namespace {

/**
 * 点光源を 1 つ持つ子ノードを足す。
 *
 * @param parent 足す先。
 * @param position 置く位置。
 * @return 足した光。
 */
ALightComponent3D& AddPointLight(ANode& parent, FVec3 position) noexcept {
    TObjectPtr<ANode> node = NewObject<ANode>();
    node->Local().position = position;
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();
    light.SetLightKind(ELight3DKind::Point);
    parent.AddChild(Move(node));
    return light;
}

} // namespace

ACS_TEST(CLightCollector3D, StartsEmpty) {
    CLightCollector3D lights;

    EXPECT_EQ(lights.DirectionalCount(), 0u);
    EXPECT_EQ(lights.PointCount(), 0u);
    EXPECT_EQ(lights.DroppedCount(), 0u);
}

ACS_TEST(CLightCollector3D, CollectsFromRootAndDescendants) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->AddComponent<ALightComponent3D>();          // root 自身も対象。

    TObjectPtr<ANode> mid = NewObject<ANode>();
    ANode& mid_ref = root->AddChild(Move(mid));
    AddPointLight(mid_ref, FVec3{ 1.0f, 0.0f, 0.0f }); // 孫の階層。

    CLightCollector3D lights;
    lights.CollectFrom(*root);

    EXPECT_EQ(lights.DirectionalCount(), 1u);
    EXPECT_EQ(lights.PointCount(), 1u);
    EXPECT_EQ(lights.DroppedCount(), 0u);
}

ACS_TEST(CLightCollector3D, SkipsNodesWithoutLightsAndDarkLights) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->AddChild(NewObject<ANode>());               // 光を持たないノード。

    ALightComponent3D& dark = AddPointLight(*root, FVec3{ 0.0f, 0.0f, 0.0f });
    dark.SetIntensity(0.0f);                          // 光っていない灯。

    CLightCollector3D lights;
    lights.CollectFrom(*root);

    EXPECT_EQ(lights.PointCount(), 0u);
    EXPECT_EQ(lights.DroppedCount(), 0u);             // 捨てたのではなく、そもそも光っていない。
}

ACS_TEST(CLightCollector3D, ClearingResetsEverything) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->AddComponent<ALightComponent3D>();

    CLightCollector3D lights;
    lights.CollectFrom(*root);
    EXPECT_EQ(lights.DirectionalCount(), 1u);

    lights.Clear();
    EXPECT_EQ(lights.DirectionalCount(), 0u);

    // 毎フレーム呼んでも積み上がらないこと。
    lights.CollectFrom(*root);
    lights.CollectFrom(*root);
    EXPECT_EQ(lights.DirectionalCount(), 1u);
}

ACS_TEST(CLightCollector3D, CountsDirectionalLightsDroppedOverTheLimit) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    for (u32 i = 0u; i < CLightCollector3D::kMaxDirectional + 3u; ++i) {
        TObjectPtr<ANode> node = NewObject<ANode>();
        node->AddComponent<ALightComponent3D>();
        root->AddChild(Move(node));
    }

    CLightCollector3D lights;
    lights.CollectFrom(*root);

    EXPECT_EQ(lights.DirectionalCount(), CLightCollector3D::kMaxDirectional);
    EXPECT_EQ(lights.DroppedCount(), 3u);             // 黙って消さない。
}

ACS_TEST(CLightCollector3D, KeepsTheNearestPointLightsWhenOverTheLimit) {
    // 宣言順で切ると「近くの光が消えて遠くの光が残る」という一番おかしな見え方になる。
    // 遠いものから先に置き、後から近いものを足しても、近い方が残ることを確かめる。
    TObjectPtr<ANode> root = NewObject<ANode>();
    for (u32 i = 0u; i < CLightCollector3D::kMaxPoint; ++i) {
        AddPointLight(*root, FVec3{ 1000.0f + static_cast<f32>(i), 0.0f, 0.0f });
    }
    AddPointLight(*root, FVec3{ 2.0f, 0.0f, 0.0f });   // 視点のすぐ近く。

    CLightCollector3D lights;
    lights.CollectFrom(*root, FVec3{ 0.0f, 0.0f, 0.0f });

    EXPECT_EQ(lights.PointCount(), CLightCollector3D::kMaxPoint);
    EXPECT_EQ(lights.DroppedCount(), 1u);

    bool found_near = false;
    for (u32 i = 0u; i < lights.PointCount(); ++i) {
        if (lights.PointLights()[i].position.x < 10.0f) found_near = true;
    }
    EXPECT_TRUE(found_near);                           // 近い光が残っている。
}

ACS_TEST(CLightCollector3D, IgnoresPointLightsFartherThanTheOnesAlreadyKept) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    for (u32 i = 0u; i < CLightCollector3D::kMaxPoint; ++i) {
        AddPointLight(*root, FVec3{ static_cast<f32>(i), 0.0f, 0.0f });
    }
    AddPointLight(*root, FVec3{ 9999.0f, 0.0f, 0.0f }); // ずっと遠い光。

    CLightCollector3D lights;
    lights.CollectFrom(*root, FVec3{ 0.0f, 0.0f, 0.0f });

    for (u32 i = 0u; i < lights.PointCount(); ++i) {
        EXPECT_TRUE(lights.PointLights()[i].position.x < 100.0f);  // 遠い光は入っていない。
    }
    EXPECT_EQ(lights.DroppedCount(), 1u);
}

ACS_TEST(CLightCollector3D, UsesWorldPositionThroughTheHierarchy) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->Local().position = FVec3{ 100.0f, 0.0f, 0.0f };

    TObjectPtr<ANode> child = NewObject<ANode>();
    child->Local().position = FVec3{ 5.0f, 0.0f, 0.0f };
    ALightComponent3D& light = child->AddComponent<ALightComponent3D>();
    light.SetLightKind(ELight3DKind::Point);
    root->AddChild(Move(child));

    CLightCollector3D lights;
    lights.CollectFrom(*root);

    EXPECT_EQ(lights.PointCount(), 1u);
    EXPECT_NEAR(lights.PointLights()[0].position.x, 105.0f, 1.0e-4f);  // 親の分も足されている。
}
