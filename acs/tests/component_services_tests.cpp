// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: コンポーネント → シーンサービス アクセス経路の検証 (GPU 非依存)。
//   FNode2D が root に配線した FSceneServices を子/コンポーネントが walk-to-root で参照でき、
//   OnAttachServices が «高々 1 回» 発火し、OnUpdate で Physics/Camera を読めることを確認する。
//   通常 FScene2D Play と editor インプロセス Play が **同一経路** で動く土台。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Node2D.h"
#include "gameframework/Component2D.h"
#include "gameframework/SceneServices.h"
#include "gameframework/SceneTextLoader.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

using namespace acs;
using namespace acs::game;

namespace {

// OnAttachServices で service 参照をキャッシュし、OnUpdate でそれを読むテストコンポーネント。
struct FSvcProbe : public FComponent2D {
    const void* Kind() const noexcept override { return ComponentKindOf<FSvcProbe>(); }

    int                attach_count = 0;
    FSceneServices*    seen         = nullptr;
    FCollisionWorld2D* phys         = nullptr;
    FCamera2D*         cam          = nullptr;
    int                updates      = 0;
    u32                shapes_seen  = 0;
    FVec2              cam_pos_seen{ 0.0f, 0.0f };

    void OnAttachServices(FSceneServices& svc) noexcept override {
        ++attach_count;
        seen = &svc;
        if (svc.Has(ESvc::Physics2D)) phys = &svc.Physics();
        if (svc.Has(ESvc::Camera2D))  cam  = &svc.Camera();
    }
    void OnUpdate(f32 /*dt*/) noexcept override {
        ++updates;
        if (phys != nullptr) shapes_seen  = phys->ShapeCount();
        if (cam  != nullptr) cam_pos_seen = cam->Position();
    }
};

} // namespace

// ACSCENE テキストローダ: 順不同 (子が先) でもノード/親子/transform を正しく復元する。
// = スタンドアロン (Build & Run) がエディタ編集シーンを読む経路の土台。
ACS_TEST(SceneTextLoader, ParsesNodesAndParenting) {
    const char* scene =
        "ACSCENE v1\n"
        "2\n"
        "2 1 30.0 0.0 0.0 1.0 1.0 48.00 0.9 0.1 0.9 1.0 Child\n"     // 子が先 (順不同)
        "1 -1 -200.0 0.0 0.0 1.0 1.0 48.00 0.25 0.7 0.95 1.0 Mover\n"
        "SEL -1 0\n";
    FNode2D root;
    const FSceneBounds b = LoadAcsceneText(scene, root);

    EXPECT_TRUE(b.valid);
    EXPECT_EQ(root.ChildCount(), 1u);          // Mover のみ root 直下 (Child は付け替えで Mover 配下)
    FNode2D* mover = root.Child(0);
    EXPECT_TRUE(mover != nullptr);
    EXPECT_EQ(mover->ChildCount(), 1u);        // Child が入れ子
    FNode2D* child = mover->Child(0);
    EXPECT_TRUE(child != nullptr);
    EXPECT_NEAR(mover->Local().position.x, -200.0f, 1e-2f);
    EXPECT_NEAR(child->World().position.x, -170.0f, 1e-2f);   // Mover(-200) ∘ local(30) = -170 (親に追従)
}

// «平坦生成 → 後で reparent» で World が親に追従する (editor インプロセス Play の再構築機構)。
// editor のノード列が親より先に子の順でも、全ノードを root 直下に作ってから付け替えれば
// 子の World() が親を合成し、親を動かすと子が追従する。Reparent は Local を保持する。
ACS_TEST(ComponentServices, FlatBuildThenReparentFollowsParent) {
    FNode2D root;
    FNode2D& a = root.AddChild(MakeUnique<FNode2D>());   // 親になる (平坦時は root 直下)
    FNode2D& b = root.AddChild(MakeUnique<FNode2D>());   // 子になる
    a.Local().position = FVec2{ 100.0f, 0.0f };
    b.Local().position = FVec2{ 10.0f, 0.0f };           // 付け替え後は a 基準のローカル

    EXPECT_NEAR(b.World().position.x, 10.0f, 1e-3f);     // 付け替え前: root 直下なので 10

    b.Reparent(a);
    root.ResolveStructuralChanges();
    EXPECT_NEAR(b.World().position.x, 110.0f, 1e-3f);    // a(100) ∘ b.local(10) = 110

    a.Local().position = FVec2{ 200.0f, 0.0f };          // 親を動かす
    EXPECT_NEAR(b.World().position.x, 210.0f, 1e-3f);    // 子が追従 (200 + 10)
}

// root 配線 → 子/孫が walk-to-root で services を解決する。
ACS_TEST(ComponentServices, RootWiringAndChildWalk) {
    FNode2D root;
    FSceneServices svc(ESvc::Input);
    root._SetSceneServices(&svc);
    EXPECT_TRUE(root.SceneServices() == &svc);

    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FNode2D& grand = child.AddChild(MakeUnique<FNode2D>());
    EXPECT_TRUE(child.SceneServices() == &svc);
    EXPECT_TRUE(grand.SceneServices() == &svc);
}

// 未配線ツリーは nullptr を返し、services を触らないコンポーネントは tick で安全。
ACS_TEST(ComponentServices, UnwiredIsNullAndSafe) {
    FNode2D root;
    EXPECT_TRUE(root.SceneServices() == nullptr);

    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FSvcProbe& p = child.AddComponent<FSvcProbe>();
    EXPECT_TRUE(p.SceneServices() == nullptr);
    EXPECT_FALSE(p.HasSceneServices());
    EXPECT_EQ(p.attach_count, 0);      // 配線なし → OnAttachServices 未発火

    root.UpdateTree(0.016f);           // cached service 無し → クラッシュせず
    EXPECT_EQ(p.updates, 1);
    EXPECT_EQ(p.shapes_seen, 0u);
}

// «構築 → 後で配線» 経路: _ActivateServices が既存コンポーネントに 1 回だけ発火する。
ACS_TEST(ComponentServices, ActivateFiresOncePreExisting) {
    FNode2D root;
    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FSvcProbe& p = child.AddComponent<FSvcProbe>();   // 配線前 attach → まだ発火しない
    EXPECT_EQ(p.attach_count, 0);

    FSceneServices svc(ESvc::Camera2D | ESvc::Physics2D);
    root._ActivateServices(svc);
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.seen == &svc);

    root._ActivateServices(svc);       // 二度目は再発火しない (二重発火防止)
    EXPECT_EQ(p.attach_count, 1);
}

// «配線 → 後で spawn» 経路 (editor Play と同じ): attach 時に即発火する。
ACS_TEST(ComponentServices, SpawnAfterWiringFiresImmediately) {
    FNode2D root;
    FSceneServices svc(ESvc::Physics2D);
    root._SetSceneServices(&svc);                     // create 時に配線 (editor シムと同じ)

    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FSvcProbe& p = child.AddComponent<FSvcProbe>();   // 配線済 → 即発火
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.seen == &svc);
}

// editor Play と同一の流れ (create 時配線 → attach → tick) で Physics/Camera を読める。
ACS_TEST(ComponentServices, ReadsPhysicsAndCameraDuringTick) {
    FNode2D root;
    FSceneServices svc(ESvc::Physics2D | ESvc::Camera2D);
    svc.Physics().AddAabb(Aabb2{ FVec2{ 0.0f, 0.0f }, FVec2{ 1.0f, 1.0f } });
    svc.Physics().AddAabb(Aabb2{ FVec2{ 5.0f, 0.0f }, FVec2{ 1.0f, 1.0f } });
    svc.Camera().SetPosition(FVec2{ 3.0f, 7.0f });

    root._SetSceneServices(&svc);
    FNode2D& child = root.AddChild(MakeUnique<FNode2D>());
    FSvcProbe& p = child.AddComponent<FSvcProbe>();
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.phys != nullptr);    // OnAttachServices でキャッシュ済
    EXPECT_TRUE(p.cam  != nullptr);

    root.UpdateTree(0.016f);
    EXPECT_EQ(p.shapes_seen, 2u);                      // 同じ physics world を読んでいる
    EXPECT_NEAR(p.cam_pos_seen.x, 3.0f, 1e-4f);        // 同じ camera を読んでいる
    EXPECT_NEAR(p.cam_pos_seen.y, 7.0f, 1e-4f);
}
