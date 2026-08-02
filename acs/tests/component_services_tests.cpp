// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework: コンポーネント → シーンサービス アクセス経路の検証 (GPU 非依存)。
//   ANode が root に配線した CSceneServices を子/コンポーネントが walk-to-root で参照でき、
//   OnAttachServices が «高々 1 回» 発火し、OnUpdate で Physics/Camera を読めることを確認する。
//   通常 AScene2D Play と editor インプロセス Play が **同一経路** で動く土台。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/SceneServices.h"
#include "gameframework/SceneTextLoader.h"
#include "gameframework/PolygonRenderer2D.h"
#include "foundation/Platform.h"
#include "math/Vec.h"
#include "math/Collision2D.h"

#include <cstdio>
#include <cstring>

using namespace acs;
using namespace acs::game;

namespace {

// OnAttachServices で service 参照をキャッシュし、OnUpdate でそれを読むテストコンポーネント。
struct AServiceProbeComponent : public AComponent {
    const void* Kind() const noexcept override { return ComponentKindOf<AServiceProbeComponent>(); }

    int                attach_count = 0;
    CSceneServices*    seen         = nullptr;
    CCollisionWorld2D* phys         = nullptr;
    CCamera2D*         cam          = nullptr;
    int                updates      = 0;
    u32                shapes_seen  = 0;
    FVec2              cam_pos_seen{ 0.0f, 0.0f };

    void OnAttachServices(CSceneServices& svc) noexcept override {
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

struct FSceneTextTempFile {
    FSceneTextTempFile() noexcept {
        const DWORD prefix = ::GetTempPathA(MAX_PATH, path);
        if (prefix == 0u || prefix >= MAX_PATH) {
            path[0] = '\0';
            return;
        }
        const int written = std::snprintf(
            path + prefix, sizeof(path) - prefix,
            "acs_scene_text_%lu.acscene",
            static_cast<unsigned long>(::GetCurrentProcessId()));
        if (written <= 0 || static_cast<usize>(written) >= sizeof(path) - prefix) {
            path[0] = '\0';
            return;
        }
        ::DeleteFileA(path);
    }

    ~FSceneTextTempFile() noexcept {
        if (path[0] != '\0') ::DeleteFileA(path);
    }

    bool Write(const void* data, usize size) noexcept {
        if (path[0] == '\0') return false;
        std::FILE* file = std::fopen(path, "wb");
        if (file == nullptr) return false;
        const bool wrote_all = std::fwrite(data, 1u, size, file) == size;
        const bool closed = std::fclose(file) == 0;
        return wrote_all && closed;
    }

    char path[MAX_PATH + 64]{};
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
    ANode root;
    const FSceneBounds b = LoadAcsceneText(scene, root);

    EXPECT_TRUE(b.valid);
    EXPECT_EQ(root.ChildCount(), 1u);          // Mover のみ root 直下 (Child は付け替えで Mover 配下)
    ANode* mover = root.Child(0);
    EXPECT_TRUE(mover != nullptr);
    EXPECT_EQ(mover->ChildCount(), 1u);        // Child が入れ子
    ANode* child = mover->Child(0);
    EXPECT_TRUE(child != nullptr);
    EXPECT_NEAR(mover->Position2D().x, -200.0f, 1e-2f);
    EXPECT_NEAR(child->World2D().position.x, -170.0f, 1e-2f);   // Mover(-200) ∘ local(30) = -170 (親に追従)
}

ACS_TEST(SceneTextLoader, CheckedLoadReportsCountsAndCommitsAtomically) {
    const char* scene =
        "ACSCENE v1\n"
        "2\n"
        "2 1 30.0 0.0 0.0 1.0 1.0 48.0 0.9 0.1 0.9 1.0 Child\n"
        "1 -1 -200.0 0.0 0.0 1.0 1.0 48.0 0.2 0.7 0.9 1.0 Parent\n"
        "NFLG 2 0 1 4\n"
        "SEL -1 0\n";

    ANode root;
    ANode* loaded_root = nullptr;
    const FSceneTextLoadResult result =
        TryLoadAcsceneText(scene, root, nullptr, nullptr, nullptr, &loaded_root);

    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Error == ESceneTextLoadError::None);
    EXPECT_EQ(result.NodesLoaded, 2u);
    EXPECT_EQ(result.DirectivesRead, 2u);
    EXPECT_TRUE(result.Bounds.valid);
    EXPECT_EQ(root.ChildCount(), 1u);
    EXPECT_TRUE(loaded_root == root.Child(0));
    EXPECT_EQ(loaded_root->ChildCount(), 1u);
    EXPECT_FALSE(loaded_root->Child(0)->IsVisible());
    EXPECT_EQ(loaded_root->Child(0)->DrawLayer(), 4);
}

ACS_TEST(SceneTextLoader, InvalidDocumentPreservesTreeRequestsAndOutRoot) {
    const char* duplicate_ids =
        "ACSCENE v1\n"
        "2\n"
        "7 -1 0 0 0 1 1 48 1 1 1 1 First\n"
        "7 -1 1 0 0 1 1 48 1 1 1 1 Duplicate\n";

    ANode root;
    ANode& existing = root.AddChild(NewObject<ANode>());
    TArray<FSpriteRequest> sprites;
    FSpriteRequest marker;
    marker.node = &existing;
    sprites.PushBack(marker);
    ANode* published_root = &existing;

    const FSceneTextLoadResult result =
        TryLoadAcsceneText(duplicate_ids, root, &sprites, nullptr, nullptr, &published_root);

    EXPECT_FALSE(result.Succeeded());
    EXPECT_TRUE(result.Error == ESceneTextLoadError::DuplicateNodeId);
    EXPECT_EQ(result.Line, 4u);
    EXPECT_EQ(root.ChildCount(), 1u);
    EXPECT_TRUE(root.Child(0) == &existing);
    EXPECT_EQ(sprites.Size(), 1u);
    EXPECT_TRUE(sprites[0].node == &existing);
    EXPECT_TRUE(published_root == &existing);
}

ACS_TEST(SceneTextLoader, RejectsMissingParentsCyclesAndMalformedDirectives) {
    const char* missing_parent =
        "ACSCENE v1\n"
        "1\n"
        "1 99 0 0 0 1 1 48 1 1 1 1 Orphan\n";
    const char* cycle =
        "ACSCENE v1\n"
        "2\n"
        "1 2 0 0 0 1 1 48 1 1 1 1 A\n"
        "2 1 0 0 0 1 1 48 1 1 1 1 B\n";
    const char* bad_property =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0 0 0 1 1 48 1 1 1 1 A\n"
        "COMP 1 APrimitiveRenderer2D\n"
        "CPROP 1 0 0 not-a-number\n";

    ANode root;
    FSceneTextLoadResult result = TryLoadAcsceneText(missing_parent, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::InvalidNodeReference);
    EXPECT_EQ(root.ChildCount(), 0u);

    result = TryLoadAcsceneText(cycle, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::HierarchyCycle);
    EXPECT_EQ(root.ChildCount(), 0u);

    result = TryLoadAcsceneText(bad_property, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::InvalidDirective);
    EXPECT_EQ(result.Line, 5u);
    EXPECT_EQ(root.ChildCount(), 0u);
}

ACS_TEST(SceneTextLoader, EnforcesTextLineNodeAndFiniteValueLimits) {
    char overlong_line[kSceneTextMaxLineBytes + 3u]{};
    for (u32 i = 0; i < kSceneTextMaxLineBytes + 1u; ++i) overlong_line[i] = 'X';
    overlong_line[kSceneTextMaxLineBytes + 1u] = '\0';

    ANode root;
    FSceneTextLoadResult result = TryLoadAcsceneText(overlong_line, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::LineTooLong);
    EXPECT_EQ(result.Line, 1u);

    const char* too_many_nodes = "ACSCENE v1\n4097\n";
    result = TryLoadAcsceneText(too_many_nodes, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::NodeLimitExceeded);
    EXPECT_EQ(result.Line, 2u);

    const char* non_finite =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 nan 0 0 1 1 48 1 1 1 1 Invalid\n";
    result = TryLoadAcsceneText(non_finite, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::InvalidNodeRecord);
    EXPECT_EQ(root.ChildCount(), 0u);
}

ACS_TEST(SceneTextLoader, RejectsOutOfRangeIntegerTokensWithoutMutation) {
    const char* oversized_count = "ACSCENE v1\n2147483648\n";
    const char* oversized_id =
        "ACSCENE v1\n"
        "1\n"
        "999999999999999999999999 -1 0 0 0 1 1 48 1 1 1 1 Invalid\n";
    const char* oversized_slot =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0 0 0 1 1 48 1 1 1 1 A\n"
        "COMP 1 APrimitiveRenderer2D\n"
        "CPROP 1 4294967296 0 1\n";

    ANode root;
    FSceneTextLoadResult result = TryLoadAcsceneText(oversized_count, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::MissingNodeCount);
    EXPECT_EQ(root.ChildCount(), 0u);

    result = TryLoadAcsceneText(oversized_id, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::InvalidNodeRecord);
    EXPECT_EQ(root.ChildCount(), 0u);

    result = TryLoadAcsceneText(oversized_slot, root);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::InvalidDirective);
    EXPECT_EQ(root.ChildCount(), 0u);
}

ACS_TEST(SceneTextLoader, FileLoaderRejectsEmbeddedNulTransactionally) {
    const char bytes[] = {
        'A','C','S','C','E','N','E',' ','v','1','\n',
        '0','\n','\0',
        'A','C','S','C','E','N','E',' ','v','1','\n','0','\n'
    };
    FSceneTextTempFile file;
    EXPECT_TRUE(file.Write(bytes, sizeof(bytes)));

    ANode root;
    ANode& existing = root.AddChild(NewObject<ANode>());
    ANode* output = &existing;
    const FSceneTextLoadResult result =
        TryLoadAcsceneFile(file.path, root, nullptr, nullptr, nullptr, &output);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::EmbeddedNul);
    EXPECT_EQ(root.ChildCount(), 1u);
    EXPECT_TRUE(root.Child(0) == &existing);
    EXPECT_TRUE(output == &existing);
}

ACS_TEST(SceneTextLoader, RejectsImportBeyondSharedNodeDepthLimit) {
    ANode root;
    ANode* deepest = &root;
    for (u32 i = 0u; i < kNodeMaxTreeDepth; ++i) {
        deepest = &deepest->AddChild(NewObject<ANode>());
    }
    EXPECT_EQ(deepest->TreeDepth(), kNodeMaxTreeDepth);

    const char* scene =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0 0 0 1 1 48 1 1 1 1 TooDeep\n";
    const FSceneTextLoadResult result = TryLoadAcsceneText(scene, *deepest);
    EXPECT_TRUE(result.Error == ESceneTextLoadError::TreeDepthLimitExceeded);
    EXPECT_EQ(deepest->ChildCount(), 0u);
}

// «平坦生成 → 後で reparent» で World が親に追従する (editor インプロセス Play の再構築機構)。
// editor のノード列が親より先に子の順でも、全ノードを root 直下に作ってから付け替えれば
// 子の World() が親を合成し、親を動かすと子が追従する。Reparent は Local を保持する。
ACS_TEST(ComponentServices, FlatBuildThenReparentFollowsParent) {
    ANode root;
    ANode& a = root.AddChild(NewObject<ANode>());   // 親になる (平坦時は root 直下)
    ANode& b = root.AddChild(NewObject<ANode>());   // 子になる
    a.SetPosition2D(FVec2{ 100.0f, 0.0f });
    b.SetPosition2D(FVec2{ 10.0f, 0.0f });           // 付け替え後は a 基準のローカル

    EXPECT_NEAR(b.World2D().position.x, 10.0f, 1e-3f);     // 付け替え前: root 直下なので 10

    b.Reparent(a);
    root.ResolveStructuralChanges();
    EXPECT_NEAR(b.World2D().position.x, 110.0f, 1e-3f);    // a(100) ∘ b.local(10) = 110

    a.SetPosition2D(FVec2{ 200.0f, 0.0f });          // 親を動かす
    EXPECT_NEAR(b.World2D().position.x, 210.0f, 1e-3f);    // 子が追従 (200 + 10)
}

// root 配線 → 子/孫が walk-to-root で services を解決する。
ACS_TEST(ComponentServices, RootWiringAndChildWalk) {
    ANode root;
    CSceneServices svc(ESvc::Input);
    root._SetSceneServices(&svc);
    EXPECT_TRUE(root.SceneServices() == &svc);

    ANode& child = root.AddChild(NewObject<ANode>());
    ANode& grand = child.AddChild(NewObject<ANode>());
    EXPECT_TRUE(child.SceneServices() == &svc);
    EXPECT_TRUE(grand.SceneServices() == &svc);
}

// 未配線ツリーは nullptr を返し、services を触らないコンポーネントは tick で安全。
ACS_TEST(ComponentServices, UnwiredIsNullAndSafe) {
    ANode root;
    EXPECT_TRUE(root.SceneServices() == nullptr);

    ANode& child = root.AddChild(NewObject<ANode>());
    AServiceProbeComponent& p = child.AddComponent<AServiceProbeComponent>();
    EXPECT_TRUE(p.SceneServices() == nullptr);
    EXPECT_FALSE(p.HasSceneServices());
    EXPECT_EQ(p.attach_count, 0);      // 配線なし → OnAttachServices 未発火

    root.UpdateTree(0.016f);           // cached service 無し → クラッシュせず
    EXPECT_EQ(p.updates, 1);
    EXPECT_EQ(p.shapes_seen, 0u);
}

// «構築 → 後で配線» 経路: _ActivateServices が既存コンポーネントに 1 回だけ発火する。
ACS_TEST(ComponentServices, ActivateFiresOncePreExisting) {
    ANode root;
    ANode& child = root.AddChild(NewObject<ANode>());
    AServiceProbeComponent& p = child.AddComponent<AServiceProbeComponent>();   // 配線前 attach → まだ発火しない
    EXPECT_EQ(p.attach_count, 0);

    CSceneServices svc(ESvc::Camera2D | ESvc::Physics2D);
    root._ActivateServices(svc);
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.seen == &svc);

    root._ActivateServices(svc);       // 二度目は再発火しない (二重発火防止)
    EXPECT_EQ(p.attach_count, 1);
}

// «配線 → 後で spawn» 経路 (editor Play と同じ): attach 時に即発火する。
ACS_TEST(ComponentServices, SpawnAfterWiringFiresImmediately) {
    ANode root;
    CSceneServices svc(ESvc::Physics2D);
    root._SetSceneServices(&svc);                     // create 時に配線 (editor シムと同じ)

    ANode& child = root.AddChild(NewObject<ANode>());
    AServiceProbeComponent& p = child.AddComponent<AServiceProbeComponent>();   // 配線済 → 即発火
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.seen == &svc);
}

// editor Play と同一の流れ (create 時配線 → attach → tick) で Physics/Camera を読める。
ACS_TEST(ComponentServices, ReadsPhysicsAndCameraDuringTick) {
    ANode root;
    CSceneServices svc(ESvc::Physics2D | ESvc::Camera2D);
    svc.Physics().AddAabb(FAabb2{ FVec2{ 0.0f, 0.0f }, FVec2{ 1.0f, 1.0f } });
    svc.Physics().AddAabb(FAabb2{ FVec2{ 5.0f, 0.0f }, FVec2{ 1.0f, 1.0f } });
    svc.Camera().SetPosition(FVec2{ 3.0f, 7.0f });

    root._SetSceneServices(&svc);
    ANode& child = root.AddChild(NewObject<ANode>());
    AServiceProbeComponent& p = child.AddComponent<AServiceProbeComponent>();
    EXPECT_EQ(p.attach_count, 1);
    EXPECT_TRUE(p.phys != nullptr);    // OnAttachServices でキャッシュ済
    EXPECT_TRUE(p.cam  != nullptr);

    root.UpdateTree(0.016f);
    EXPECT_EQ(p.shapes_seen, 2u);                      // 同じ physics world を読んでいる
    EXPECT_NEAR(p.cam_pos_seen.x, 3.0f, 1e-4f);        // 同じ camera を読んでいる
    EXPECT_NEAR(p.cam_pos_seen.y, 7.0f, 1e-4f);
}

// --- SceneTextLoader: NFLG (ノードフラグ) ----------------------------------------

ACS_TEST(SceneTextLoader, NflgSetsVisibleEnabledSortLayer) {
    const char* scene =
        "ACSCENE v1\n"
        "2\n"
        "1 -1 0.0 0.0 0.0 1.0 1.0 48.0 0.6 0.7 0.9 1.0 NodeA\n"
        "2 -1 10.0 0.0 0.0 1.0 1.0 48.0 0.6 0.7 0.9 1.0 NodeB\n"
        "NFLG 1 0 0 3\n"     // NodeA: visible=0, enabled=0, sortLayer=3
        "NFLG 2 1 1 7\n"     // NodeB: visible=1, enabled=1, sortLayer=7
        "SEL -1 0\n";
    ANode root;
    LoadAcsceneText(scene, root);

    EXPECT_EQ(root.ChildCount(), 2u);
    ANode* a = root.Child(0);
    ANode* b = root.Child(1);
    EXPECT_TRUE(a != nullptr && b != nullptr);

    // NodeA: NFLG で visible=false, enabled=false, sortLayer=3
    EXPECT_TRUE(!a->IsVisible());
    EXPECT_TRUE(!a->IsEnabled());
    EXPECT_EQ(a->DrawLayer(), 3);

    // NodeB: NFLG で visible=true, enabled=true, sortLayer=7
    EXPECT_TRUE(b->IsVisible());
    EXPECT_TRUE(b->IsEnabled());
    EXPECT_EQ(b->DrawLayer(), 7);
}

// NFLG が無ければ既定値 (visible=1, enabled=1, sortLayer=0) のまま。
ACS_TEST(SceneTextLoader, NflgDefaultsWithoutLine) {
    const char* scene =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0.0 0.0 0.0 1.0 1.0 48.0 0.6 0.7 0.9 1.0 Solo\n"
        "SEL -1 0\n";
    ANode root;
    LoadAcsceneText(scene, root);

    EXPECT_EQ(root.ChildCount(), 1u);
    ANode* n = root.Child(0);
    EXPECT_TRUE(n != nullptr);
    EXPECT_TRUE(n->IsVisible());
    EXPECT_TRUE(n->IsEnabled());
    EXPECT_EQ(n->DrawLayer(), 0);
}

// --- SceneTextLoader: RPLY (描画用滑らか頂点) ------------------------------------

ACS_TEST(SceneTextLoader, RplyOverridesPolyVertsForRendering) {
    const char* scene =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0.0 0.0 0.0 1.0 1.0 48.0 0.6 0.7 0.9 1.0 PolyNode\n"
        "COMP 1 APrimitiveRenderer2D\n"
        "COMP 1 APolygonRenderer2D\n"
        "POLY 1 4 0.0 0.0 10.0 0.0 10.0 10.0 0.0 10.0\n"   // 角張った四角
        "RPLY 1 8 0.0 0.0 5.0 -1.0 10.0 0.0 11.0 5.0 10.0 10.0 5.0 11.0 0.0 10.0 -1.0 5.0\n"  // 滑らかな8頂点
        "SEL -1 0\n";
    ANode root;
    LoadAcsceneText(scene, root);

    EXPECT_EQ(root.ChildCount(), 1u);
    ANode* n = root.Child(0);
    EXPECT_TRUE(n != nullptr);

    // APolygonRenderer2D が RPLY の 8 頂点で上書きされている
    bool found = false;
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        AComponent* comp = n->ComponentAt(c);
        if (comp != nullptr && comp->ReflectName() != nullptr
            && std::strcmp(comp->ReflectName(), "APolygonRenderer2D") == 0) {
            APolygonRenderer2D* poly = static_cast<APolygonRenderer2D*>(comp);
            EXPECT_EQ(poly->VertCount(), 8u);  // RPLY の 8 頂点 (POLY の 4 を上書き)
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// RPLY が無ければ POLY の頂点がそのまま使われる (後方互換)。
ACS_TEST(SceneTextLoader, PolyUsedWhenNoRply) {
    const char* scene =
        "ACSCENE v1\n"
        "1\n"
        "1 -1 0.0 0.0 0.0 1.0 1.0 48.0 0.6 0.7 0.9 1.0 PolyNode\n"
        "COMP 1 APolygonRenderer2D\n"
        "POLY 1 5 0.0 0.0 10.0 0.0 10.0 10.0 5.0 12.0 0.0 10.0\n"
        "SEL -1 0\n";
    ANode root;
    LoadAcsceneText(scene, root);

    ANode* n = root.Child(0);
    EXPECT_TRUE(n != nullptr);
    bool found = false;
    for (u32 c = 0; c < n->ComponentCount(); ++c) {
        AComponent* comp = n->ComponentAt(c);
        if (comp != nullptr && comp->ReflectName() != nullptr
            && std::strcmp(comp->ReflectName(), "APolygonRenderer2D") == 0) {
            APolygonRenderer2D* poly = static_cast<APolygonRenderer2D*>(comp);
            EXPECT_EQ(poly->VertCount(), 5u);  // POLY の 5 頂点 (RPLY 無し)
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}
