// SPDX-License-Identifier: Apache-2.0
// ALightComponent3D (3D の光) の動作確認テスト
//
// ノードのワールド変換から向きと位置が導かれること、種類が違えば書き出さないこと、
// そして「消えているのと区別が付かない」設定を弾くことを検証する。
// 描画を伴わないのでヘッドレスで走る。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/LightComponent3D.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 度をラジアンへ直す。 */
constexpr f32 Deg(f32 d) noexcept { return d * 0.01745329252f; }

} // namespace

ACS_TEST(ALightComponent3D, DefaultsToDownwardDirectionalLight) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();

    EXPECT_TRUE(light.LightKind() == ELight3DKind::Directional);
    EXPECT_TRUE(light.IsEmitting());

    // 無回転なら真上から下を照らす。FDirLight の既定と揃えてある。
    const FVec3 dir = light.WorldDirection();
    EXPECT_NEAR(dir.x,  0.0f, 1.0e-4f);
    EXPECT_NEAR(dir.y, -1.0f, 1.0e-4f);
    EXPECT_NEAR(dir.z,  0.0f, 1.0e-4f);
}

ACS_TEST(ALightComponent3D, DirectionFollowsNodeRotation) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();

    // X 軸まわりに 90 度回すと、真下向きが前向き (-Z or +Z) になる。
    node->Local().rotation = FQuat::Euler(Deg(90.0f), 0.0f, 0.0f);

    const FVec3 dir = light.WorldDirection();
    EXPECT_NEAR(dir.y, 0.0f, 1.0e-3f);            // もう真下ではない。
    EXPECT_NEAR(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z, 1.0f, 1.0e-3f);  // 長さは 1 のまま。
}

ACS_TEST(ALightComponent3D, DirectionInheritsParentRotation) {
    // 親に付ければ一緒に回る。付け替えても向きを手で直さなくてよい。
    TObjectPtr<ANode> parent = NewObject<ANode>();
    parent->Local().rotation = FQuat::Euler(Deg(90.0f), 0.0f, 0.0f);

    TObjectPtr<ANode> child = NewObject<ANode>();
    ALightComponent3D& light = child->AddComponent<ALightComponent3D>();
    parent->AddChild(Move(child));

    const FVec3 dir = light.WorldDirection();
    EXPECT_NEAR(dir.y, 0.0f, 1.0e-3f);
}

ACS_TEST(ALightComponent3D, FillDirectionalScalesColorByIntensity) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();
    light.SetColor(FVec3{ 0.5f, 0.25f, 0.125f });
    light.SetIntensity(4.0f);

    FDirLight out{};
    EXPECT_TRUE(light.FillDirectional(out));
    EXPECT_NEAR(out.color.x, 2.0f,  1.0e-4f);
    EXPECT_NEAR(out.color.y, 1.0f,  1.0e-4f);
    EXPECT_NEAR(out.color.z, 0.5f,  1.0e-4f);
    EXPECT_NEAR(out.direction.y, -1.0f, 1.0e-4f);
}

ACS_TEST(ALightComponent3D, FillPointUsesWorldPosition) {
    TObjectPtr<ANode> parent = NewObject<ANode>();
    parent->Local().position = FVec3{ 10.0f, 0.0f, 0.0f };

    TObjectPtr<ANode> child = NewObject<ANode>();
    child->Local().position = FVec3{ 0.0f, 5.0f, 0.0f };
    ALightComponent3D& light = child->AddComponent<ALightComponent3D>();
    light.SetLightKind(ELight3DKind::Point);
    light.SetRange(25.0f);
    parent->AddChild(Move(child));

    FPointLight out{};
    EXPECT_TRUE(light.FillPoint(out));
    EXPECT_NEAR(out.position.x, 10.0f, 1.0e-4f);   // 親の分も足されたワールド位置。
    EXPECT_NEAR(out.position.y,  5.0f, 1.0e-4f);
    EXPECT_NEAR(out.range,      25.0f, 1.0e-4f);
}

ACS_TEST(ALightComponent3D, FillRejectsMismatchedKindWithoutTouchingOutput) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();
    light.SetLightKind(ELight3DKind::Point);

    FDirLight dir{};
    dir.color = FVec3{ 7.0f, 7.0f, 7.0f };
    EXPECT_TRUE(!light.FillDirectional(dir));
    EXPECT_NEAR(dir.color.x, 7.0f, 1.0e-4f);       // 触っていない。

    light.SetLightKind(ELight3DKind::Directional);
    FPointLight point{};
    point.range = 3.0f;
    EXPECT_TRUE(!light.FillPoint(point));
    EXPECT_NEAR(point.range, 3.0f, 1.0e-4f);
}

ACS_TEST(ALightComponent3D, ZeroIntensityStopsEmitting) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();

    light.SetIntensity(0.0f);
    EXPECT_TRUE(!light.IsEmitting());

    // 計算しても何も足さない光を集めても無駄なので、書き出しも断る。
    FDirLight out{};
    EXPECT_TRUE(!light.FillDirectional(out));

    light.SetIntensity(-5.0f);
    EXPECT_NEAR(light.Intensity(), 0.0f, 1.0e-6f); // 負値は 0 に丸める。
}

ACS_TEST(ALightComponent3D, RejectsZeroRange) {
    TObjectPtr<ANode> node = NewObject<ANode>();
    ALightComponent3D& light = node->AddComponent<ALightComponent3D>();

    const f32 before = light.Range();
    light.SetRange(0.0f);
    EXPECT_NEAR(light.Range(), before, 1.0e-6f);   // 0 だと消えているのと区別が付かない。

    light.SetRange(-1.0f);
    EXPECT_NEAR(light.Range(), before, 1.0e-6f);

    light.SetRange(50.0f);
    EXPECT_NEAR(light.Range(), 50.0f, 1.0e-6f);
}
