// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Transform3D.h / ANode.h / AComponent.h の検証 (3D シーングラフ):
//   ピュアロジック (GPU 不要)。検証項目:
//     - FTransform3D の Compose / ToMat4 が «エンジン独自の行列実装» と一致する
//       (回転方向・合成順を実測でクロスチェック。符号を逆に出さないため)
//     - ANode の階層 transform 合成 (親子 2 段)
//     - コンポーネントの lifecycle (OnAttach/OnUpdate/OnDetach) と取得/除去
//     - 構造変更 (Destroy → OnDespawn → 配列除去, Reparent でワールド階層移動)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Transform3D.h"
#include "gameframework/TransformBatchSoA.h"
#include "gameframework/ANode.h"
#include "gameframework/AComponent.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/MeshComponent3D.h"
#include "gameframework/Light2DComponent.h"
#include "asset/MeshAsset.h"
#include "memory/SharedPtr.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "math/Math.h"
#include "memory/UniquePtr.h"

#include <cstdio>
#include <cstring>

using namespace acs;
using namespace acs::game;

namespace {

// 3 成分の近接比較ヘルパ。
void ExpectVec3Near(FVec3 a, FVec3 b, f32 eps) noexcept {
    EXPECT_NEAR(a.x, b.x, eps);
    EXPECT_NEAR(a.y, b.y, eps);
    EXPECT_NEAR(a.z, b.z, eps);
}

// lifecycle 呼び出し回数を数えるテスト用コンポーネント。
class ACounterComponent3D : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ACounterComponent3D)
    i32 attach = 0, detach = 0, updates = 0, fixed = 0;
    f32 accum = 0.0f;
    i32* detachOut = nullptr;   // OnDetach 後も観測できる外部カウンタ (本体は破棄されるため)
    void OnAttach(ANode&) noexcept override { ++attach; }
    void OnUpdate(f32 dt)   noexcept override { ++updates; accum += dt; }
    void OnFixedUpdate(f32) noexcept override { ++fixed; }
    void OnDetach()         noexcept override { ++detach; if (detachOut) ++*detachOut; }
};

// OnSpawn/OnDespawn を数えるテスト用ノード。
class ACounterNode3D : public ANode {
public:
    i32* spawns = nullptr;
    i32* despawns = nullptr;
    void OnSpawn()   noexcept override { if (spawns)   ++*spawns; }
    void OnDespawn() noexcept override { if (despawns) ++*despawns; }
};

} // namespace

// === FTransform3D ===========================================================

// --- 既定は単位 transform ---------------------------------------------------
ACS_TEST(Transform3D, IdentityDefault) {
    FTransform3D t;
    ExpectVec3Near(t.position, FVec3{0,0,0}, 1e-6f);
    ExpectVec3Near(t.scale,    FVec3{1,1,1}, 1e-6f);
    // 恒等回転 (0,0,0,1)
    EXPECT_NEAR(t.rotation.x, 0.0f, 1e-6f);
    EXPECT_NEAR(t.rotation.y, 0.0f, 1e-6f);
    EXPECT_NEAR(t.rotation.z, 0.0f, 1e-6f);
    EXPECT_NEAR(t.rotation.w, 1.0f, 1e-6f);
}

ACS_TEST(Transform3D, SoABatchMatchesScalarComposeAndSupportsInPlaceUpdate)
{
    constexpr usize kCount = 5u;
    FTransform3D parent{FVec3{3.0f, -2.0f, 8.0f}, FQuat::AxisAngle(FVec3{0, 1, 0}, 0.65f), FVec3{2.0f, 3.0f, 4.0f}};
    FVec3 positions[kCount] = {{1,2,3}, {-4,5,6}, {7,-8,9}, {0,0,0}, {0.5f,1.5f,-2.5f}};
    FQuat rotations[kCount] = {FQuat{}, FQuat::AxisAngle(FVec3{1,0,0}, 0.2f), FQuat::AxisAngle(FVec3{0,0,1}, -0.4f), FQuat{}, FQuat::AxisAngle(FVec3{0,1,0}, 0.8f)};
    FVec3 scales[kCount] = {{1,1,1}, {2,1,1}, {1,2,1}, {1,1,3}, {0.5f,0.75f,1.25f}};
    FTransform3D expected[kCount]{};
    for (usize i = 0u; i < kCount; ++i) {
        expected[i] = parent.Compose(FTransform3D{positions[i], rotations[i], scales[i]});
    }

    EXPECT_TRUE(ComposeTransformBatchSoA(parent, FTransformSoAInput{positions, rotations, scales}, positions, rotations, scales, kCount));
    for (usize i = 0u; i < kCount; ++i) {
        ExpectVec3Near(positions[i], expected[i].position, 1.0e-6f);
        ExpectVec3Near(scales[i], expected[i].scale, 1.0e-6f);
        EXPECT_NEAR(rotations[i].x, expected[i].rotation.x, 1.0e-6f);
        EXPECT_NEAR(rotations[i].y, expected[i].rotation.y, 1.0e-6f);
        EXPECT_NEAR(rotations[i].z, expected[i].rotation.z, 1.0e-6f);
        EXPECT_NEAR(rotations[i].w, expected[i].rotation.w, 1.0e-6f);
    }
}

// --- 回転方向の «実測クロスチェック» ----------------------------------------
// FQuat の Rotate が、エンジン独自の FMat4::RotationZ (独立実装) と一致するかを
// 確認する。+90° about +Z は (1,0,0) → (0,1,0) (2D の CCW 規約と同じ)。
ACS_TEST(Transform3D, QuatRotationMatchesMatrix) {
    const FQuat qz = FQuat::AxisAngle(FVec3{0,0,1}, kHalfPi);
    const FVec3 byQuat = Rotate(qz, FVec3{1,0,0});
    // 独立実装での ground truth (row-vector v*M)
    const FVec3 byMat  = TransformPoint(FVec3{1,0,0}, FMat4::RotationZ(kHalfPi));
    ExpectVec3Near(byQuat, byMat, 1e-5f);
    // 幾何的に (0,1,0) であること (符号を逆に出していないことの直接確認)
    ExpectVec3Near(byQuat, FVec3{0,1,0}, 1e-5f);
}

// --- ToMat4 が Rotate+平行移動と一致する (行列が正しい向き/平行移動) ---------
ACS_TEST(Transform3D, ToMat4MatchesCompose) {
    FTransform3D t;
    t.position = FVec3{10, 2, -3};
    t.rotation = FQuat::AxisAngle(FVec3{0,0,1}, kHalfPi);
    t.scale    = FVec3{2, 3, 4};

    const FMat4 m = t.ToMat4();
    // ローカル点 p をワールドへ: スケール → 回転 → 平行移動。
    const FVec3 p{1, 0, 0};
    const FVec3 byMat = TransformPoint(p, m);
    // 手計算: scale*(1,0,0)=(2,0,0) → +90°Z → (0,2,0) → +pos = (10,4,-3)
    const FVec3 expect = t.position + Rotate(t.rotation, FVec3{ t.scale.x*p.x, t.scale.y*p.y, t.scale.z*p.z });
    ExpectVec3Near(byMat, expect, 1e-4f);
    ExpectVec3Near(byMat, FVec3{10, 4, -3}, 1e-4f);
}

// --- Compose: 親が原点で回転、子オフセット → world 位置が回って出る ----------
ACS_TEST(Transform3D, ComposeRotatesChildOffset) {
    FTransform3D parent;
    parent.position = FVec3{5, 0, 0};
    parent.rotation = FQuat::AxisAngle(FVec3{0,0,1}, kHalfPi);   // +90° about Z

    FTransform3D local;
    local.position = FVec3{1, 0, 0};   // 親フレームで +X に 1

    const FTransform3D world = parent.Compose(local);
    // 親が +90°Z 回転 → 子オフセット (1,0,0) は (0,1,0) に回り、親位置を足す
    ExpectVec3Near(world.position, FVec3{5, 1, 0}, 1e-5f);
}

// --- Compose: scale は component-wise 積、Identity 合成は恒等 ----------------
ACS_TEST(Transform3D, ComposeScaleAndIdentity) {
    FTransform3D parent;
    parent.scale = FVec3{2, 3, 4};
    FTransform3D local;
    local.scale = FVec3{5, 1, 0.5f};
    const FTransform3D w = parent.Compose(local);
    ExpectVec3Near(w.scale, FVec3{10, 3, 2}, 1e-5f);

    // Identity.Compose(x) == x かつ x.Compose(Identity) == x (位置/スケール)
    FTransform3D x;
    x.position = FVec3{1, 2, 3};
    x.scale    = FVec3{2, 2, 2};
    x.rotation = FQuat::AxisAngle(FVec3{0,1,0}, 0.7f);
    const FTransform3D a = FTransform3D::Identity().Compose(x);
    const FTransform3D b = x.Compose(FTransform3D::Identity());
    ExpectVec3Near(a.position, x.position, 1e-5f);
    ExpectVec3Near(b.position, x.position, 1e-5f);
    ExpectVec3Near(a.scale,    x.scale,    1e-5f);
    ExpectVec3Near(b.scale,    x.scale,    1e-5f);
}

// --- SetEulerDeg が editor の Rx*Ry*Rz 行列と一致する (実測クロスチェック) ----
ACS_TEST(Transform3D, EulerMatchesEditorMatrix) {
    // editor_abi の Node3DModel と同じ回転合成 Rx*Ry*Rz を独立に組んで ground truth に。
    struct FE { f32 x, y, z; };
    const FE euls[] = { {30, 0, 0}, {0, 45, 0}, {0, 0, 60}, {20, 35, -50}, {-15, 80, 25} };
    const FVec3 vs[] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.5f, -0.3f, 0.8f} };
    for (const FE& e : euls) {
        FTransform3D t;
        t.SetEulerDeg(FVec3{ e.x, e.y, e.z });
        const FMat4 R = FMat4::RotationX(e.x * kDeg2Rad)
                      * FMat4::RotationY(e.y * kDeg2Rad)
                      * FMat4::RotationZ(e.z * kDeg2Rad);
        for (const FVec3 v : vs) {
            const FVec3 byQuat = Rotate(t.rotation, v);
            const FVec3 byMat  = TransformPoint(v, R);
            ExpectVec3Near(byQuat, byMat, 1e-4f);
        }
    }
}

// --- SetEulerDeg → EulerDeg の往復一致 (非ジンバル域) ------------------------
ACS_TEST(Transform3D, EulerRoundTrip) {
    struct FE { f32 x, y, z; };
    const FE euls[] = { {0, 0, 0}, {30, 0, 0}, {0, 40, 0}, {0, 0, 70},
                       {25, -35, 50}, {-60, 20, -80} };   // |Y|<85° に収める
    for (const FE& e : euls) {
        FTransform3D t;
        t.SetEulerDeg(FVec3{ e.x, e.y, e.z });
        const FVec3 out = t.EulerDeg();
        EXPECT_NEAR(out.x, e.x, 1e-2f);
        EXPECT_NEAR(out.y, e.y, 1e-2f);
        EXPECT_NEAR(out.z, e.z, 1e-2f);
    }
}

// --- 単一軸 Z+90° は (1,0,0)→(0,1,0) (既存 2D/Quat 規約と一致) ---------------
ACS_TEST(Transform3D, EulerSingleAxisZ) {
    FTransform3D t;
    t.SetEulerDeg(FVec3{ 0, 0, 90 });
    ExpectVec3Near(Rotate(t.rotation, FVec3{1, 0, 0}), FVec3{0, 1, 0}, 1e-4f);
}

// --- Compose の world 回転が独立行列積 (M_local * M_parent) と一致する ---------
ACS_TEST(Transform3D, ComposeRotationMatchesMatrixProduct) {
    auto eulMat = [](f32 x, f32 y, f32 z) {
        return FMat4::RotationX(x * kDeg2Rad) * FMat4::RotationY(y * kDeg2Rad) * FMat4::RotationZ(z * kDeg2Rad);
    };
    FTransform3D parent; parent.SetEulerDeg(FVec3{ 20, 35, -10 });
    FTransform3D local;  local.SetEulerDeg(FVec3{ -15, 40, 25 });
    const FTransform3D world = parent.Compose(local);
    // 導出: M(world) = M_local * M_parent (Rotate(q,v)=v*M、a*b は a 先適用)
    const FMat4 expect = eulMat(-15, 40, 25) * eulMat(20, 35, -10);
    const FVec3 vs[] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.4f, 0.7f, -0.5f} };
    for (const FVec3 v : vs) {
        ExpectVec3Near(Rotate(world.rotation, v), TransformPoint(v, expect), 1e-4f);
    }
}

// === ANode 階層 ===========================================================

// --- 親の transform が子の World() に合成される (2 段) -----------------------
ACS_TEST(Node3D, WorldComposesThroughHierarchy) {
    ANode root;
    root.Local().position = FVec3{10, 0, 0};
    root.Local().rotation = FQuat::AxisAngle(FVec3{0,0,1}, kHalfPi);

    ANode& child = root.AddChild(NewObject<ANode>());
    child.Local().position = FVec3{2, 0, 0};   // 親フレームで +X に 2

    const FTransform3D w = child.World();
    // root が +90°Z → 子 (2,0,0) は (0,2,0) → +root位置 = (10,2,0)
    ExpectVec3Near(w.position, FVec3{10, 2, 0}, 1e-5f);

    // 孫 (3 段目) も合成される
    ANode& g = child.AddChild(NewObject<ANode>());
    g.Local().position = FVec3{0, 1, 0};
    // child の world 回転も +90°Z (root と同じ) なので (0,1,0) は (-1,0,0) に回り、child world (10,2,0) に足す
    ExpectVec3Near(g.World().position, FVec3{9, 2, 0}, 1e-5f);
}

// --- root の World == Local (親なし) ----------------------------------------
ACS_TEST(Node3D, RootWorldEqualsLocal) {
    ANode root;
    root.Local().position = FVec3{1, 2, 3};
    ExpectVec3Near(root.World().position, FVec3{1, 2, 3}, 1e-6f);
}

// === コンポーネント lifecycle ===============================================

// --- AddComponent で OnAttach、UpdateTree で OnUpdate ------------------------
ACS_TEST(Node3D, ComponentLifecycle) {
    ANode root;
    i32 detachCount = 0;
    auto& c = root.AddComponent<ACounterComponent3D>();
    c.detachOut = &detachCount;
    EXPECT_EQ(c.attach, 1);
    EXPECT_EQ(c.updates, 0);

    root.UpdateTree(0.5f);
    root.UpdateTree(0.25f);
    EXPECT_EQ(c.updates, 2);
    EXPECT_NEAR(c.accum, 0.75f, 1e-5f);

    root.FixedUpdateTree(0.01f);
    EXPECT_EQ(c.fixed, 1);

    // 取得 / 保有判定
    EXPECT_TRUE(root.HasComponent<ACounterComponent3D>());
    EXPECT_TRUE(root.GetComponent<ACounterComponent3D>() == &c);
    EXPECT_EQ(root.ComponentCount(), 1u);

    // 除去で OnDetach (外部カウンタで観測)
    EXPECT_TRUE(root.RemoveComponent<ACounterComponent3D>());
    EXPECT_EQ(detachCount, 1);
    // c は破棄済みなので参照しない。再取得は nullptr。
    EXPECT_TRUE(root.GetComponent<ACounterComponent3D>() == nullptr);
    EXPECT_EQ(root.ComponentCount(), 0u);
}

// --- 無効ノードは update をスキップ -----------------------------------------
ACS_TEST(Node3D, DisabledSkipsUpdate) {
    ANode root;
    auto& c = root.AddComponent<ACounterComponent3D>();
    root.SetEnabled(false);
    root.UpdateTree(1.0f);
    EXPECT_EQ(c.updates, 0);
    root.SetEnabled(true);
    root.UpdateTree(1.0f);
    EXPECT_EQ(c.updates, 1);
}

// === 構造変更 ===============================================================

// --- Destroy → ResolveStructuralChanges で OnDespawn + 配列除去 -------------
ACS_TEST(Node3D, DestroyReapsWithDespawn) {
    i32 spawns = 0, despawns = 0;
    ANode root;

    auto childUP = NewObject<ACounterNode3D>();
    childUP->spawns = &spawns; childUP->despawns = &despawns;
    ANode& child = root.AddChild(Move(childUP));
    EXPECT_EQ(spawns, 1);              // AddChild で OnSpawn
    EXPECT_EQ(root.ChildCount(), 1u);

    child.Destroy();
    EXPECT_TRUE(child.IsPendingDestroy());
    EXPECT_EQ(despawns, 0);            // まだ reap されていない

    root.ResolveStructuralChanges();
    EXPECT_EQ(despawns, 1);            // OnDespawn が呼ばれた
    EXPECT_EQ(root.ChildCount(), 0u);  // 配列から除去
}

// --- 破棄時に子コンポーネントの OnDetach も発火 -----------------------------
ACS_TEST(Node3D, DestroyDetachesComponents) {
    i32 detachCount = 0;
    ANode root;
    ANode& child = root.AddChild(NewObject<ANode>());
    auto& c = child.AddComponent<ACounterComponent3D>();
    c.detachOut = &detachCount;
    EXPECT_EQ(c.detach, 0);
    child.Destroy();
    root.ResolveStructuralChanges();
    // child は破棄されたので c も無効。OnDetach は外部カウンタで観測する。
    EXPECT_EQ(detachCount, 1);
    EXPECT_EQ(root.ChildCount(), 0u);
}

// --- Reparent でワールド階層を移動 (フレーム境界で適用) ---------------------
ACS_TEST(Node3D, ReparentMovesSubtree) {
    ANode root;
    ANode& a = root.AddChild(NewObject<ANode>());
    ANode& b = root.AddChild(NewObject<ANode>());
    ANode& child = a.AddChild(NewObject<ANode>());
    EXPECT_EQ(a.ChildCount(), 1u);
    EXPECT_EQ(b.ChildCount(), 0u);

    child.Reparent(b);
    EXPECT_TRUE(child.IsPendingReparent());
    root.ResolveStructuralChanges();

    EXPECT_EQ(a.ChildCount(), 0u);
    EXPECT_EQ(b.ChildCount(), 1u);
    EXPECT_TRUE(b.Child(0) == &child);
    EXPECT_TRUE(child.Parent() == &b);
}

// --- cycle を作る Reparent は拒否される -------------------------------------
ACS_TEST(Node3D, ReparentRejectsCycle) {
    ANode root;
    ANode& a = root.AddChild(NewObject<ANode>());
    ANode& b = a.AddChild(NewObject<ANode>());
    // a を自分の子孫 b の下に移すのは cycle → 無視される
    a.Reparent(b);
    EXPECT_TRUE(!a.IsPendingReparent());
    root.ResolveStructuralChanges();
    EXPECT_TRUE(a.Parent() == &root);
    EXPECT_EQ(a.ChildCount(), 1u);
}

// --- 名前の設定/取得 --------------------------------------------------------
ACS_TEST(Node3D, NameSetGet) {
    ANode n(FStringView("Cube"));
    EXPECT_TRUE(n.Name() == FStringView("Cube"));
    n.SetName(FStringView("Renamed"));
    EXPECT_TRUE(n.Name() == FStringView("Renamed"));
}

// === FScene3D ===============================================================

// --- Spawn で root 配下にノードが増える / NodeCount -------------------------
ACS_TEST(Scene3D, SpawnAndCount) {
    FScene3D scene;
    EXPECT_EQ(scene.NodeCount(), 1u);                 // root のみ
    EXPECT_EQ(scene.Root().ChildCount(), 0u);

    ANode& a = scene.Spawn(FStringView("A"));
    ANode& b = scene.Spawn(FStringView("B"));
    scene.Spawn(FStringView("A.child"), &a);
    (void)b;
    EXPECT_EQ(scene.Root().ChildCount(), 2u);          // A, B
    EXPECT_EQ(a.ChildCount(), 1u);
    EXPECT_EQ(scene.NodeCount(), 4u);                  // root + A + B + A.child
}

// --- FindByName で root / 子 / 不在を解決 -----------------------------------
ACS_TEST(Scene3D, FindByName) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("Player"));
    scene.Spawn(FStringView("Weapon"), &a);

    EXPECT_TRUE(scene.FindByName(FStringView("Root")) == &scene.Root());
    EXPECT_TRUE(scene.FindByName(FStringView("Player")) == &a);
    EXPECT_TRUE(scene.FindByName(FStringView("Weapon")) != nullptr);
    EXPECT_TRUE(scene.FindByName(FStringView("Weapon"))->Parent() == &a);
    EXPECT_TRUE(scene.FindByName(FStringView("Nope")) == nullptr);
}

// --- Update が subtree に伝播し、Destroy が次 Update で reap される ----------
ACS_TEST(Scene3D, UpdatePropagatesAndReaps) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("A"));
    auto& c = a.AddComponent<ACounterComponent3D>();

    scene.Update(0.5f);
    EXPECT_EQ(c.updates, 1);                            // root→A へ伝播
    scene.FixedUpdate(0.02f);
    EXPECT_EQ(c.fixed, 1);

    a.Destroy();
    EXPECT_EQ(scene.NodeCount(), 2u);                  // まだ reap 前
    scene.Update(0.016f);                              // ここで ResolveStructuralChanges
    EXPECT_EQ(scene.NodeCount(), 1u);                  // A が除去され root のみ
    EXPECT_EQ(scene.Root().ChildCount(), 0u);
}

// --- generational id: Spawn 登録 / Get / IdOf 往復 -------------------------
ACS_TEST(Scene3D, IdRegistryGetAndIdOf) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("A"));
    const FNodeId ida = a.Id();
    EXPECT_TRUE(ida.IsValid());
    EXPECT_TRUE(scene.Get(ida) == &a);
    EXPECT_TRUE(scene.IsValid(ida));
    EXPECT_TRUE(scene.IdOf(&a) == ida);
    // root も登録済み (constructor)
    EXPECT_TRUE(scene.Get(scene.Root().Id()) == &scene.Root());
    // 未登録 / invalid id は nullptr
    EXPECT_TRUE(scene.Get(FNodeId{}) == nullptr);
    EXPECT_TRUE(!scene.IsValid(FNodeId{}));
}

ACS_TEST(Scene3D, TrySpawnSuccessRegistersExactlyOnce) {
    FScene3D scene;
    const FScene3DSpawnResult result =
        scene.TrySpawn(FStringView("Checked"));
    EXPECT_TRUE(result.Succeeded());
    EXPECT_TRUE(result.Node != nullptr);
    EXPECT_TRUE(result.Id.IsValid());
    EXPECT_TRUE(result.Node->Id() == result.Id);
    EXPECT_TRUE(result.Node->Parent() == &scene.Root());
    EXPECT_TRUE(scene.Get(result.Id) == result.Node);
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_EQ(scene.RegisteredCount(), 2u);
}

ACS_TEST(Scene3D, TrySpawnRejectsForeignParentWithoutMutation) {
    FScene3D scene;
    FScene3D other_scene;
    ANode& foreign_parent = other_scene.Spawn(FStringView("Foreign"));
    const u32 tree_before = scene.NodeCount();
    const u32 registered_before = scene.RegisteredCount();
    const u32 foreign_children_before = foreign_parent.ChildCount();

    const FScene3DSpawnResult result =
        scene.TrySpawn(FStringView("Rejected"), &foreign_parent);
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EScene3DSpawnError::InvalidParent));
    EXPECT_TRUE(result.Node == nullptr);
    EXPECT_EQ(scene.NodeCount(), tree_before);
    EXPECT_EQ(scene.RegisteredCount(), registered_before);
    EXPECT_EQ(foreign_parent.ChildCount(), foreign_children_before);

    // 互換 API も外部 parent を変更せず、安全な root sentinel を返す。
    ANode& sentinel = scene.Spawn(FStringView("RejectedLegacy"), &foreign_parent);
    EXPECT_TRUE(&sentinel == &scene.Root());
    EXPECT_EQ(scene.NodeCount(), tree_before);
    EXPECT_EQ(scene.RegisteredCount(), registered_before);
    EXPECT_EQ(foreign_parent.ChildCount(), foreign_children_before);
}

ACS_TEST(Scene3D, TrySpawnRollsBackPoolWhenParentIsPendingDestroy) {
    FScene3D scene;
    ANode& parent = scene.Spawn(FStringView("Parent"));
    parent.Destroy();
    const u32 tree_before = scene.NodeCount();
    const u32 registered_before = scene.RegisteredCount();

    const FScene3DSpawnResult result =
        scene.TrySpawn(FStringView("Rejected"), &parent);
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EScene3DSpawnError::ChildAttachRejected));
    EXPECT_EQ(static_cast<u32>(result.AddChildResult),
              static_cast<u32>(EAddChildResult::ParentPendingDestroy));
    EXPECT_TRUE(result.Node == nullptr);
    EXPECT_EQ(scene.NodeCount(), tree_before);
    EXPECT_EQ(scene.RegisteredCount(), registered_before);
    EXPECT_EQ(parent.ChildCount(), 0u);
}

ACS_TEST(Scene3D, TrySpawnRollsBackPoolAtTreeDepthLimit) {
    FScene3D scene;
    ANode* parent = &scene.Root();
    for (u32 depth = 1u; depth <= kNodeMaxTreeDepth; ++depth) {
        const FScene3DSpawnResult added =
            scene.TrySpawn(FStringView("DepthNode"), parent);
        EXPECT_TRUE(added.Succeeded());
        if (!added.Succeeded()) return;
        parent = added.Node;
    }
    const u32 tree_before = scene.NodeCount();
    const u32 registered_before = scene.RegisteredCount();

    const FScene3DSpawnResult rejected =
        scene.TrySpawn(FStringView("TooDeep"), parent);
    EXPECT_EQ(static_cast<u32>(rejected.Error),
              static_cast<u32>(EScene3DSpawnError::ChildAttachRejected));
    EXPECT_EQ(static_cast<u32>(rejected.AddChildResult),
              static_cast<u32>(EAddChildResult::TreeDepthLimitExceeded));
    EXPECT_TRUE(rejected.Node == nullptr);
    EXPECT_EQ(scene.NodeCount(), tree_before);
    EXPECT_EQ(scene.RegisteredCount(), registered_before);
    EXPECT_EQ(parent->ChildCount(), 0u);
}

ACS_TEST(NodePool, DuplicateAndForeignRegistrationAreRejected) {
    FNodePool first_pool;
    FNodePool second_pool;
    first_pool.Init(4u);
    second_pool.Init(4u);
    auto node = NewObject<ANode>(FStringView("Registered"));

    const FNodePoolRegisterResult first =
        first_pool.TryRegisterExistingNode(node.Get());
    EXPECT_TRUE(first.Succeeded());
    EXPECT_EQ(first_pool.ActiveCount(), 1u);

    const FNodePoolRegisterResult duplicate =
        first_pool.TryRegisterExistingNode(node.Get());
    EXPECT_EQ(static_cast<u32>(duplicate.Error),
              static_cast<u32>(ENodePoolRegisterError::AlreadyRegistered));
    EXPECT_TRUE(duplicate.Id == first.Id);
    EXPECT_EQ(first_pool.ActiveCount(), 1u);
    EXPECT_TRUE(first_pool.RegisterExistingNode(node.Get()) == first.Id);
    EXPECT_EQ(first_pool.ActiveCount(), 1u);

    const FNodePoolRegisterResult foreign =
        second_pool.TryRegisterExistingNode(node.Get());
    EXPECT_EQ(static_cast<u32>(foreign.Error),
              static_cast<u32>(ENodePoolRegisterError::RegisteredByAnotherPool));
    EXPECT_EQ(second_pool.ActiveCount(), 0u);
    EXPECT_TRUE(node->Id() == first.Id);

    first_pool.Unregister(first.Id);
    EXPECT_TRUE(!node->Id().IsValid());
    const FNodePoolRegisterResult transferred =
        second_pool.TryRegisterExistingNode(node.Get());
    EXPECT_TRUE(transferred.Succeeded());
    EXPECT_EQ(second_pool.ActiveCount(), 1u);
    second_pool.Unregister(transferred.Id);
}

ACS_TEST(NodePool, ClearAllReusesSlotsWithoutCapacityGrowth) {
    FNodePool pool;
    pool.Init(4u);
    auto first_node = NewObject<ANode>();
    auto second_node = NewObject<ANode>();
    const FNodeId first = pool.RegisterExistingNode(first_node.Get());
    const FNodeId second = pool.RegisterExistingNode(second_node.Get());
    EXPECT_TRUE(first.IsValid());
    EXPECT_TRUE(second.IsValid());
    EXPECT_EQ(pool.ActiveCount(), 2u);
    const u32 capacity_before = pool.Capacity();

    pool.ClearAll();
    EXPECT_EQ(pool.ActiveCount(), 0u);
    EXPECT_TRUE(!first_node->Id().IsValid());
    EXPECT_TRUE(!second_node->Id().IsValid());

    auto replacement_a = NewObject<ANode>();
    auto replacement_b = NewObject<ANode>();
    EXPECT_TRUE(pool.RegisterExistingNode(replacement_a.Get()).IsValid());
    EXPECT_TRUE(pool.RegisterExistingNode(replacement_b.Get()).IsValid());
    EXPECT_EQ(pool.ActiveCount(), 2u);
    EXPECT_EQ(pool.Capacity(), capacity_before);
    pool.ClearAll();
}

// --- Destroy(id): Update まで valid、reap 後は stale -------------------------
ACS_TEST(Scene3D, DestroyByIdGoesStaleAfterUpdate) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("A"));
    const FNodeId ida = a.Id();
    EXPECT_EQ(scene.RegisteredCount(), 2u);            // root + A

    EXPECT_TRUE(scene.Destroy(ida));
    EXPECT_TRUE(scene.IsValid(ida));                   // まだ reap 前 = valid
    scene.Update(0.016f);                              // purge → reap
    EXPECT_TRUE(!scene.IsValid(ida));                  // stale 化
    EXPECT_TRUE(scene.Get(ida) == nullptr);
    EXPECT_EQ(scene.RegisteredCount(), 1u);            // root のみ
    EXPECT_EQ(scene.NodeCount(), 1u);
    // root は破棄不可
    EXPECT_TRUE(!scene.Destroy(scene.Root().Id()));
}

// --- slot 再利用時の世代不一致で旧 handle が stale 検出される -----------------
ACS_TEST(Scene3D, StaleHandleAfterSlotReuse) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("A"));
    const FNodeId old = a.Id();
    scene.Destroy(old);
    scene.Update(0.016f);                              // a を reap + slot free

    // 新規 Spawn が同じ slot index を再利用するが gen が進む
    ANode& b = scene.Spawn(FStringView("B"));
    const FNodeId fresh = b.Id();
    EXPECT_TRUE(fresh.IsValid());
    // 旧 handle は (gen 不一致で) stale、新 handle は valid
    EXPECT_TRUE(!scene.IsValid(old));
    EXPECT_TRUE(scene.Get(old) == nullptr);
    EXPECT_TRUE(scene.Get(fresh) == &b);
    EXPECT_TRUE(old != fresh);
}

// --- 直接 node->Destroy() でも pool が self-heal する ------------------------
ACS_TEST(Scene3D, DirectDestroyAlsoPurged) {
    FScene3D scene;
    ANode& a = scene.Spawn(FStringView("A"));
    const FNodeId ida = a.Id();
    a.Destroy();                                       // scene.Destroy ではなく直接
    scene.Update(0.016f);
    EXPECT_TRUE(!scene.IsValid(ida));                  // PurgePendingDestroy が外す
    EXPECT_EQ(scene.RegisteredCount(), 1u);
}

ACS_TEST(Scene3D, DestroyingParentPurgesDescendantHandlesBeforeReap) {
    FScene3D scene;
    ANode& parent = scene.Spawn(FStringView("Parent"));
    ANode& child = scene.Spawn(FStringView("Child"), &parent);
    ANode& grandchild = scene.Spawn(FStringView("Grandchild"), &child);
    const FNodeId parent_id = parent.Id();
    const FNodeId child_id = child.Id();
    const FNodeId grandchild_id = grandchild.Id();
    EXPECT_EQ(scene.RegisteredCount(), 4u);

    parent.Destroy();
    scene.Update(0.016f);
    EXPECT_TRUE(!scene.IsValid(parent_id));
    EXPECT_TRUE(!scene.IsValid(child_id));
    EXPECT_TRUE(!scene.IsValid(grandchild_id));
    EXPECT_TRUE(scene.Get(parent_id) == nullptr);
    EXPECT_TRUE(scene.Get(child_id) == nullptr);
    EXPECT_TRUE(scene.Get(grandchild_id) == nullptr);
    EXPECT_EQ(scene.RegisteredCount(), 1u);
    EXPECT_EQ(scene.NodeCount(), 1u);
}

// === FScene3D::Raycast ======================================================

// --- 上からのレイが原点の cube に当たり、t が正しい --------------------------
ACS_TEST(Scene3DRaycast, HitsCubeFromAbove) {
    FScene3D scene;
    ANode& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    cube.Local().position = FVec3{ 0, 0, 0 };

    FRay3 ray{ FVec3{ 0, 5, 0 }, FVec3{ 0, -1, 0 } };   // 真上から下向き
    f32 t = -1.0f;
    const FNodeId hit = scene.Raycast(ray, &t);
    EXPECT_TRUE(hit == cube.Id());
    EXPECT_NEAR(t, 4.5f, 1e-3f);                        // 上面 y=0.5 に y=5 から → t=4.5
}

// --- 外れるレイは invalid を返す --------------------------------------------
ACS_TEST(Scene3DRaycast, MissReturnsInvalid) {
    FScene3D scene;
    ANode& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    FRay3 ray{ FVec3{ 10, 5, 10 }, FVec3{ 0, -1, 0 } };  // cube から遠い
    EXPECT_TRUE(!scene.Raycast(ray).IsValid());
}

// --- 2 つのうち手前のノードを返す -------------------------------------------
ACS_TEST(Scene3DRaycast, ReturnsNearest) {
    FScene3D scene;
    ANode& near_ = scene.Spawn(FStringView("Near"));
    near_.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    near_.Local().position = FVec3{ 0, 0, 2 };
    ANode& far_ = scene.Spawn(FStringView("Far"));
    far_.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    far_.Local().position = FVec3{ 0, 0, 5 };

    FRay3 ray{ FVec3{ 0, 0, -5 }, FVec3{ 0, 0, 1 } };    // +Z 方向
    f32 t = -1.0f;
    const FNodeId hit = scene.Raycast(ray, &t);
    EXPECT_TRUE(hit == near_.Id());
    EXPECT_NEAR(t, 6.5f, 1e-3f);                        // near 手前面 z=1.5 に z=-5 から → 6.5
}

// --- スケールで AABB が拡大し、単位 cube なら外れるレイが当たる (OBB) --------
ACS_TEST(Scene3DRaycast, RespectsScale) {
    FScene3D scene;
    ANode& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    cube.Local().scale = FVec3{ 3, 3, 3 };              // 半幅 1.5

    FRay3 ray{ FVec3{ 1.2f, 5, 0 }, FVec3{ 0, -1, 0 } }; // x=1.2 は単位(0.5)では外れ、3倍(1.5)で当たる
    EXPECT_TRUE(scene.Raycast(ray) == cube.Id());
}

// --- 回転した cube を正しく OBB ピック (90°Z 回転で縦長 box) -----------------
ACS_TEST(Scene3DRaycast, RespectsRotation) {
    FScene3D scene;
    ANode& box = scene.Spawn(FStringView("Box"));
    box.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube);
    box.Local().scale = FVec3{ 3, 0.5f, 0.5f };        // X に長い棒
    // 回転なしなら x=±1.5 まで当たる。Z 90° 回転で «Y に長い» 棒になる → x=1.2 は外れる。
    FRay3 down{ FVec3{ 1.2f, 5, 0 }, FVec3{ 0, -1, 0 } };
    EXPECT_TRUE(scene.Raycast(down) == box.Id());       // 回転前: x=1.2 は半幅1.5内で当たる
    box.Local().SetEulerDeg(FVec3{ 0, 0, 90 });
    EXPECT_TRUE(!scene.Raycast(down).IsValid());        // 回転後: X 半幅が 0.5 になり x=1.2 は外れる
}

// --- AMeshComponent3D の無いノードはピック対象外 ----------------------------
ACS_TEST(Scene3DRaycast, IgnoresNodesWithoutMesh) {
    FScene3D scene;
    ANode& empty = scene.Spawn(FStringView("Empty"));   // mesh component なし
    empty.Local().position = FVec3{ 0, 0, 0 };
    FRay3 ray{ FVec3{ 0, 5, 0 }, FVec3{ 0, -1, 0 } };
    EXPECT_TRUE(!scene.Raycast(ray).IsValid());
    (void)empty;
}

// === AMeshComponent3D =======================================================

// --- 既定値 / プリミティブ・色・パスの設定取得 -----------------------------
ACS_TEST(MeshComponent3D, DataRoundTrip) {
    ANode node(FStringView("Mesh"));
    auto& m = node.AddComponent<AMeshComponent3D>();
    // 既定: Cube・白・影あり
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Cube);
    EXPECT_TRUE(m.CastsShadow());
    EXPECT_NEAR(m.Color().x, 1.0f, 1e-6f);

    m.SetPrimitive(EMeshPrimitive3D::Sphere);
    m.SetColor(FVec4{0.2f, 0.4f, 0.6f, 1.0f});
    m.SetCastsShadow(false);
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Sphere);
    EXPECT_NEAR(m.Color().z, 0.6f, 1e-6f);
    EXPECT_TRUE(!m.CastsShadow());

    // メッシュパス設定で種別が Mesh に切り替わる
    m.SetMeshPath(FStringView("Assets/teapot.obj"));
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Mesh);
    EXPECT_TRUE(m.MeshPath() == FStringView("Assets/teapot.obj"));

    // GetComponent で取り出せる
    EXPECT_TRUE(node.GetComponent<AMeshComponent3D>() == &m);
}

// --- コンストラクタでプリミティブ指定 ---------------------------------------
ACS_TEST(MeshComponent3D, ConstructWithPrimitive) {
    ANode node;
    auto& m = node.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Plane);
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Plane);
}

// === Scene3DSerialize =======================================================

// --- save → load の往復で構造/transform/メッシュ記述が一致する ---------------
ACS_TEST(Scene3DSerialize, RoundTrip) {
    FScene3D a;
    // Box: cube・赤・pos/euler/scale 非自明
    ANode& box = a.Spawn(FStringView("Box"));
    box.Local().position = FVec3{ 1, 2, 3 };
    box.Local().SetEulerDeg(FVec3{ 10, 20, 30 });
    box.Local().scale = FVec3{ 2, 2, 2 };
    { auto& m = box.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Cube); m.SetColor(FVec4{0.9f,0.1f,0.1f,1}); }
    // Ball: sphere・青 (root 直下)
    ANode& ball = a.Spawn(FStringView("Ball"));
    { auto& m = ball.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Sphere); m.SetColor(FVec4{0.2f,0.4f,0.9f,1}); }
    // Child: Box の子 (階層)
    ANode& child = a.Spawn(FStringView("Child"), &box);
    child.Local().position = FVec3{ 0, 1, 0 };
    child.AddComponent<AMeshComponent3D>(EMeshPrimitive3D::Plane);
    // Custom: メッシュパス
    ANode& custom = a.Spawn(FStringView("Custom"));
    { auto& m = custom.AddComponent<AMeshComponent3D>(); m.SetMeshPath(FStringView("Assets/teapot.obj")); }

    char buf[4096];
    const u32 wrote = SaveScene3DText(a, buf, sizeof(buf));
    EXPECT_TRUE(wrote > 0);

    FScene3D b;
    EXPECT_TRUE(LoadScene3DText(b, buf));
    // 構造一致 (root + 4 ノード)
    EXPECT_EQ(b.NodeCount(), a.NodeCount());
    EXPECT_EQ(b.NodeCount(), 5u);

    // Box の transform / mesh
    ANode* bbox = b.FindByName(FStringView("Box"));
    EXPECT_TRUE(bbox != nullptr);
    ExpectVec3Near(bbox->Local().position, FVec3{1,2,3}, 1e-3f);
    ExpectVec3Near(bbox->Local().scale,    FVec3{2,2,2}, 1e-3f);
    ExpectVec3Near(bbox->Local().EulerDeg(), FVec3{10,20,30}, 2e-2f);
    AMeshComponent3D* bmc = bbox->GetComponent<AMeshComponent3D>();
    EXPECT_TRUE(bmc != nullptr);
    EXPECT_TRUE(bmc->Primitive() == EMeshPrimitive3D::Cube);
    EXPECT_NEAR(bmc->Color().x, 0.9f, 1e-3f);

    // 階層: Child の親が Box
    ANode* bchild = b.FindByName(FStringView("Child"));
    EXPECT_TRUE(bchild != nullptr);
    EXPECT_TRUE(bchild->Parent() == bbox);
    ExpectVec3Near(bchild->Local().position, FVec3{0,1,0}, 1e-3f);

    // メッシュパス
    ANode* bcustom = b.FindByName(FStringView("Custom"));
    EXPECT_TRUE(bcustom != nullptr);
    AMeshComponent3D* cmc = bcustom->GetComponent<AMeshComponent3D>();
    EXPECT_TRUE(cmc != nullptr);
    EXPECT_TRUE(cmc->Primitive() == EMeshPrimitive3D::Mesh);
    EXPECT_TRUE(cmc->MeshPath() == FStringView("Assets/teapot.obj"));
}

// --- Load は既存内容を置き換える (二重 Load で増殖しない) -------------------
ACS_TEST(Scene3DSerialize, LoadReplacesExisting) {
    FScene3D a;
    a.Spawn(FStringView("One"));
    a.Spawn(FStringView("Two"));
    char buf[1024];
    SaveScene3DText(a, buf, sizeof(buf));

    FScene3D b;
    b.Spawn(FStringView("OldA"));
    b.Spawn(FStringView("OldB"));
    b.Spawn(FStringView("OldC"));
    EXPECT_EQ(b.NodeCount(), 4u);
    LoadScene3DText(b, buf);
    EXPECT_EQ(b.NodeCount(), 3u);                  // root + One + Two (古い 3 つは消えた)
    EXPECT_TRUE(b.FindByName(FStringView("OldA")) == nullptr);
    EXPECT_TRUE(b.FindByName(FStringView("One")) != nullptr);
    // 2 回 Load しても増えない
    LoadScene3DText(b, buf);
    EXPECT_EQ(b.NodeCount(), 3u);
}

// --- 空シーン (root のみ) の往復 --------------------------------------------
ACS_TEST(Scene3DSerialize, EmptyScene) {
    FScene3D a;
    char buf[256];
    const u32 wrote = SaveScene3DText(a, buf, sizeof(buf));
    EXPECT_TRUE(wrote > 0);                        // root 行は出る
    FScene3D b;
    b.Spawn(FStringView("X"));
    EXPECT_TRUE(LoadScene3DText(b, buf));
    EXPECT_EQ(b.NodeCount(), 1u);                  // root のみ
}

ACS_TEST(Scene3DSerialize, CheckedSaveReportsCapacityWithoutPartialOutput) {
    FScene3D scene;
    scene.Spawn(FStringView("Child"));

    const FScene3DSaveResult query = TrySaveScene3DText(scene, nullptr, 0u);
    EXPECT_EQ(static_cast<u32>(query.Error),
              static_cast<u32>(EScene3DSerializeError::BufferTooSmall));
    EXPECT_EQ(query.BytesWritten, 0u);
    EXPECT_EQ(query.NodeCount, 2u);
    EXPECT_TRUE(query.RequiredBytes > 1u);

    char tiny[16];
    std::memset(tiny, 0x5A, sizeof(tiny));
    const FScene3DSaveResult insufficient =
        TrySaveScene3DText(scene, tiny, sizeof(tiny));
    EXPECT_EQ(static_cast<u32>(insufficient.Error),
              static_cast<u32>(EScene3DSerializeError::BufferTooSmall));
    EXPECT_EQ(insufficient.RequiredBytes, query.RequiredBytes);
    EXPECT_EQ(insufficient.BytesWritten, 0u);
    for (u32 i = 0u; i < sizeof(tiny); ++i)
        EXPECT_EQ(static_cast<u8>(tiny[i]), static_cast<u8>(0x5Au));

    char output[1024];
    const FScene3DSaveResult saved =
        TrySaveScene3DText(scene, output, sizeof(output));
    EXPECT_TRUE(saved.Succeeded());
    EXPECT_EQ(saved.RequiredBytes, saved.BytesWritten + 1u);
    EXPECT_EQ(saved.RequiredBytes, query.RequiredBytes);
    EXPECT_EQ(SaveScene3DText(scene, output, sizeof(output)), saved.BytesWritten);
}

ACS_TEST(Scene3DSerialize, CheckedSaveRejectsUnsafeNameWithoutWriting) {
    char long_name[kScene3DSerializeMaxNameBytes + 2u];
    std::memset(long_name, 'N', sizeof(long_name));
    long_name[sizeof(long_name) - 1u] = '\0';

    FScene3D scene;
    scene.Root().SetName(FStringView(long_name));
    char output[512];
    std::memset(output, 0x33, sizeof(output));
    const FScene3DSaveResult result =
        TrySaveScene3DText(scene, output, sizeof(output));
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidName));
    EXPECT_EQ(result.BytesWritten, 0u);
    for (u32 i = 0u; i < sizeof(output); ++i)
        EXPECT_EQ(static_cast<u8>(output[i]), static_cast<u8>(0x33u));
}

ACS_TEST(Scene3DSerialize, InvalidParentAndTruncationAreTransactional) {
    constexpr char kInvalidParent[] =
        "N3D 0 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "N3D 1 99 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Child\n";
    constexpr char kTruncated[] =
        "N3D 0 -1 -1 0 0 0 0 0 0 1 1 1 1 1 Root\n";

    FScene3D scene;
    scene.Spawn(FStringView("Keep"));
    const FScene3DLoadResult invalid_parent =
        TryLoadScene3DText(scene, kInvalidParent, sizeof(kInvalidParent) - 1u);
    EXPECT_EQ(static_cast<u32>(invalid_parent.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidParent));
    EXPECT_EQ(invalid_parent.ErrorLine, 2u);
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);

    const FScene3DLoadResult truncated =
        TryLoadScene3DText(scene, kTruncated, sizeof(kTruncated) - 1u);
    EXPECT_EQ(static_cast<u32>(truncated.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidNumber));
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(Scene3DSerialize, RejectsHugeDuplicateAndNonFiniteDeclarations) {
    constexpr char kOverflowId[] =
        "N3D 2147483648 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n";
    constexpr char kHugeId[] =
        "N3D 2147483647 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n";
    constexpr char kDuplicateId[] =
        "N3D 0 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "N3D 0 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Again\n";
    constexpr char kNotFinite[] =
        "N3D 0 -1 -1 nan 0 0 0 0 0 1 1 1 1 1 1 1 Root\n";
    constexpr char kInvalidPrimitive[] =
        "N3D 0 -1 99 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n";

    FScene3D scene;
    const FScene3DLoadResult overflow =
        TryLoadScene3DText(scene, kOverflowId, sizeof(kOverflowId) - 1u);
    EXPECT_EQ(static_cast<u32>(overflow.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidInteger));
    const FScene3DLoadResult huge =
        TryLoadScene3DText(scene, kHugeId, sizeof(kHugeId) - 1u);
    EXPECT_EQ(static_cast<u32>(huge.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidNodeId));
    const FScene3DLoadResult duplicate =
        TryLoadScene3DText(scene, kDuplicateId, sizeof(kDuplicateId) - 1u);
    EXPECT_EQ(static_cast<u32>(duplicate.Error),
              static_cast<u32>(EScene3DSerializeError::DuplicateNodeId));
    const FScene3DLoadResult non_finite =
        TryLoadScene3DText(scene, kNotFinite, sizeof(kNotFinite) - 1u);
    EXPECT_EQ(static_cast<u32>(non_finite.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidNumber));
    const FScene3DLoadResult primitive =
        TryLoadScene3DText(scene, kInvalidPrimitive, sizeof(kInvalidPrimitive) - 1u);
    EXPECT_EQ(static_cast<u32>(primitive.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidPrimitive));
    EXPECT_EQ(scene.NodeCount(), 1u);
}

ACS_TEST(Scene3DSerialize, RejectsDeepTreeBeforeReplacingDestination) {
    char text[65536];
    u32 cursor = static_cast<u32>(std::snprintf(
        text, sizeof(text),
        "N3D 0 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"));
    for (u32 id = 1u; id <= kScene3DSerializeMaxTreeDepth + 1u; ++id) {
        const int written = std::snprintf(
            text + cursor, sizeof(text) - cursor,
            "N3D %u %u -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Node\n",
            id, id - 1u);
        EXPECT_TRUE(written > 0);
        if (written <= 0) return;
        cursor += static_cast<u32>(written);
    }

    FScene3D scene;
    scene.Spawn(FStringView("Keep"));
    const FScene3DLoadResult result = TryLoadScene3DText(scene, text, cursor);
    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EScene3DSerializeError::TreeDepthLimitExceeded));
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(Scene3DSerialize, RejectsInvalidMeshRecordsAndOversizedInput) {
    constexpr char kOrphanMesh[] = "MSH3D 0 Assets/missing.obj\n";
    constexpr char kDuplicateMesh[] =
        "N3D 0 -1 3 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "MSH3D 0 Assets/a.obj\n"
        "MSH3D 0 Assets/b.obj\n";
    FScene3D scene;
    const FScene3DLoadResult orphan =
        TryLoadScene3DText(scene, kOrphanMesh, sizeof(kOrphanMesh) - 1u);
    EXPECT_EQ(static_cast<u32>(orphan.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidNodeId));
    const FScene3DLoadResult duplicate =
        TryLoadScene3DText(scene, kDuplicateMesh, sizeof(kDuplicateMesh) - 1u);
    EXPECT_EQ(static_cast<u32>(duplicate.Error),
              static_cast<u32>(EScene3DSerializeError::DuplicateMeshPath));
    const FScene3DLoadResult oversized =
        TryLoadScene3DText(scene, "", kScene3DSerializeMaxInputBytes + 1u);
    EXPECT_EQ(static_cast<u32>(oversized.Error),
              static_cast<u32>(EScene3DSerializeError::InputTooLarge));
    EXPECT_TRUE(std::strcmp(Scene3DSerializeErrorName(oversized.Error),
                            "input_too_large") == 0);
}

ACS_TEST(Scene3DSerialize, RejectsLongLineAndEmbeddedNullTransactionally) {
    char long_line[kScene3DSerializeMaxLineBytes + 2u];
    std::memset(long_line, 'X', sizeof(long_line));
    FScene3D scene;
    scene.Spawn(FStringView("Keep"));
    const FScene3DLoadResult too_long =
        TryLoadScene3DText(scene, long_line, sizeof(long_line));
    EXPECT_EQ(static_cast<u32>(too_long.Error),
              static_cast<u32>(EScene3DSerializeError::LineTooLong));

    constexpr char kEmbeddedNull[] = {
        'N', '3', 'D', ' ', '0', '\0', ' ', '-', '1', '\n'
    };
    const FScene3DLoadResult embedded =
        TryLoadScene3DText(scene, kEmbeddedNull, sizeof(kEmbeddedNull));
    EXPECT_EQ(static_cast<u32>(embedded.Error),
              static_cast<u32>(EScene3DSerializeError::InvalidLine));
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(Scene3DSerialize, CheckedLoadReportsCountsAndConsumesExactInput) {
    FScene3D source;
    ANode& mesh_node = source.Spawn(FStringView("Mesh"));
    mesh_node.AddComponent<AMeshComponent3D>().SetMeshPath(
        FStringView("Assets/mesh.obj"));
    char text[1024];
    const FScene3DSaveResult saved =
        TrySaveScene3DText(source, text, sizeof(text));
    EXPECT_TRUE(saved.Succeeded());

    FScene3D destination;
    const FScene3DLoadResult loaded =
        TryLoadScene3DText(destination, text, saved.BytesWritten);
    EXPECT_TRUE(loaded.Succeeded());
    EXPECT_EQ(loaded.BytesConsumed, saved.BytesWritten);
    EXPECT_EQ(loaded.NodeCount, 2u);
    EXPECT_EQ(loaded.MeshPathCount, 1u);
    EXPECT_EQ(destination.NodeCount(), 2u);
}

ACS_TEST(Scene3DSerialize, LoadsEditorV2WithSparseIdsMultipleRootsAndComponents) {
    constexpr char kEditorScene[] =
        "ACS3D v2\r\n"
        "N3D 42 -1 0 1 2 3 10 20 30 2 2 2 0.2 0.4 0.6 1 Main\r\n"
        "FLG3D 42 0 1\r\n"
        "MAT3D 42 0.75 0.2\r\n"
        "CMP3D 42 ALight2DComponent\r\n"
        "CPROP3D 42 0 0 640 0 0 0\r\n"
        "N3D 7 -1 -1 -3 0 4 0 0 0 1 1 1 1 1 1 1 OtherRoot\r\n"
        "EMPTY3D 7\r\n"
        "N3D 99 42 2 0 5 0 0 0 0 1 1 1 1 0.5 0.25 1 Child\r\n"
        "SEL3D 99\r\n";

    FScene3D scene;
    scene.Spawn(FStringView("Old"));
    const FScene3DLoadResult result =
        TryLoadScene3DText(scene, kEditorScene, sizeof(kEditorScene) - 1u);

    EXPECT_TRUE(result.Succeeded());
    EXPECT_EQ(result.NodeCount, 4u); // synthetic root + three editor nodes
    EXPECT_EQ(scene.NodeCount(), 4u);
    EXPECT_TRUE(scene.FindByName(FStringView("Old")) == nullptr);

    ANode* main = scene.Root().FindBySerialId(42);
    ANode* other_root = scene.Root().FindBySerialId(7);
    ANode* child = scene.Root().FindBySerialId(99);
    EXPECT_TRUE(main != nullptr);
    EXPECT_TRUE(other_root != nullptr);
    EXPECT_TRUE(child != nullptr);
    if (main == nullptr || other_root == nullptr || child == nullptr) return;

    EXPECT_TRUE(main->Parent() == &scene.Root());
    EXPECT_TRUE(other_root->Parent() == &scene.Root());
    EXPECT_TRUE(child->Parent() == main);
    EXPECT_TRUE(!main->IsVisible());
    EXPECT_TRUE(main->IsEnabled());
    ExpectVec3Near(main->Local().position, FVec3{1, 2, 3}, 1e-6f);

    AMeshComponent3D* mesh = main->GetComponent<AMeshComponent3D>();
    EXPECT_TRUE(mesh != nullptr);
    if (mesh != nullptr) {
        EXPECT_TRUE(mesh->Primitive() == EMeshPrimitive3D::Cube);
        EXPECT_NEAR(mesh->Color().x, 0.2f, 1e-6f);
        EXPECT_NEAR(mesh->Material().pbr.metallic, 0.75f, 1e-6f);
        EXPECT_NEAR(mesh->Material().pbr.roughness, 0.2f, 1e-6f);
        EXPECT_TRUE(mesh->MaterialLoaded());
    }

    ALight2DComponent* light = main->GetComponent<ALight2DComponent>();
    EXPECT_TRUE(light != nullptr);
    if (light != nullptr)
        EXPECT_NEAR(light->m_Radius, 640.0f, 1e-6f);
}

ACS_TEST(Scene3DSerialize, RejectsUnsupportedEditorDirectiveTransactionally) {
    constexpr char kUnsupported[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "SPR3D 1 Assets/sprite.png\n";

    FScene3D scene;
    scene.Spawn(FStringView("Keep"));
    const FScene3DLoadResult result =
        TryLoadScene3DText(
            scene, kUnsupported, sizeof(kUnsupported) - 1u);

    EXPECT_EQ(static_cast<u32>(result.Error),
              static_cast<u32>(EScene3DSerializeError::UnsupportedDirective));
    EXPECT_EQ(result.ErrorLine, 3u);
    EXPECT_EQ(scene.NodeCount(), 2u);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);
}

ACS_TEST(Scene3DSerialize, RejectsInvalidEditorSelectionTransactionally) {
    constexpr char kMissingSelection[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "SEL3D 999\n";
    constexpr char kNegativeSelection[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "SEL3D -1\n";
    constexpr char kNoSelection[] =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "SEL3D 0\n";

    FScene3D scene;
    scene.Spawn(FStringView("Keep"));

    const FScene3DLoadResult missing = TryLoadScene3DText(
        scene, kMissingSelection, sizeof(kMissingSelection) - 1u);
    EXPECT_EQ(missing.Error, EScene3DSerializeError::InvalidNodeId);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);

    const FScene3DLoadResult negative = TryLoadScene3DText(
        scene, kNegativeSelection, sizeof(kNegativeSelection) - 1u);
    EXPECT_EQ(negative.Error, EScene3DSerializeError::InvalidNodeId);
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) != nullptr);

    const FScene3DLoadResult none = TryLoadScene3DText(
        scene, kNoSelection, sizeof(kNoSelection) - 1u);
    EXPECT_TRUE(none.Succeeded());
    EXPECT_TRUE(scene.FindByName(FStringView("Keep")) == nullptr);
    EXPECT_TRUE(scene.Root().FindBySerialId(1) != nullptr);
}

// --- メッシュアセットを所有し、外部参照を捨てても生存する -------------------
ACS_TEST(MeshComponent3D, OwnsMeshAsset) {
    ANode node(FStringView("Mesh"));
    auto& m = node.AddComponent<AMeshComponent3D>();
    EXPECT_TRUE(!m.HasMeshAsset());
    EXPECT_TRUE(m.Mesh() == nullptr);

    // メッシュを作って 2 頂点入れる
    TSharedPtr<FMeshAsset> mesh = MakeShared<FMeshAsset>();
    mesh->Vertices().PushBack(FMeshVertex{ FVec3{0,0,0}, FVec3{0,1,0}, 0.0f, 0.0f });
    mesh->Vertices().PushBack(FMeshVertex{ FVec3{1,0,0}, FVec3{0,1,0}, 1.0f, 0.0f });
    FMeshAsset* raw = mesh.Get();

    // Asset 基底へアップキャストして所有させる (種別が Mesh に切り替わる)
    m.SetMeshAsset(TSharedPtr<FAsset>(mesh));
    EXPECT_TRUE(m.HasMeshAsset());
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Mesh);
    EXPECT_TRUE(m.Mesh() == raw);
    EXPECT_EQ(m.Mesh()->Vertices().Size(), 2u);

    // 外部の共有参照を捨てる → コンポーネントが強参照を持つので生存
    mesh.Reset();
    EXPECT_TRUE(m.Mesh() == raw);
    EXPECT_EQ(m.Mesh()->Vertices().Size(), 2u);

    // null を渡すと外れる (種別はそのまま Mesh)
    m.SetMeshAsset(TSharedPtr<FAsset>{});
    EXPECT_TRUE(!m.HasMeshAsset());
    EXPECT_TRUE(m.Mesh() == nullptr);
}

ACS_TEST(MeshAsset, MutableAccessAdvancesGeometryRevision) {
    FMeshAsset mesh;
    const FMeshAsset& read_only = mesh;

    const u64 initial = mesh.GeometryRevision();
    (void)read_only.Vertices();
    (void)read_only.Indices();
    (void)read_only.SubMeshes();
    EXPECT_EQ(mesh.GeometryRevision(), initial);

    (void)mesh.Vertices();
    const u64 after_vertices = mesh.GeometryRevision();
    EXPECT_TRUE(after_vertices > initial);

    (void)mesh.Indices();
    const u64 after_indices = mesh.GeometryRevision();
    EXPECT_TRUE(after_indices > after_vertices);

    (void)mesh.SubMeshes();
    const u64 after_submeshes = mesh.GeometryRevision();
    EXPECT_TRUE(after_submeshes > after_indices);

    mesh.MarkGeometryDirty();
    EXPECT_TRUE(mesh.GeometryRevision() > after_submeshes);
}
