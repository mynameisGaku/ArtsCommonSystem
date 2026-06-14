// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// gameframework/Transform3D.h / Node3D.h / Component3D.h の検証 (3D シーングラフ):
//   ピュアロジック (GPU 不要)。検証項目:
//     - FTransform3D の Compose / ToMat4 が «エンジン独自の行列実装» と一致する
//       (回転方向・合成順を実測でクロスチェック。符号を逆に出さないため)
//     - FNode3D の階層 transform 合成 (親子 2 段)
//     - コンポーネントの lifecycle (OnAttach/OnUpdate/OnDetach) と取得/除去
//     - 構造変更 (Destroy → OnDespawn → 配列除去, Reparent でワールド階層移動)
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/Transform3D.h"
#include "gameframework/Node3D.h"
#include "gameframework/Component3D.h"
#include "gameframework/Scene3D.h"
#include "gameframework/Scene3DSerialize.h"
#include "gameframework/MeshComponent3D.h"
#include "asset/MeshAsset.h"
#include "memory/SharedPtr.h"
#include "math/Mat.h"
#include "math/Quat.h"
#include "math/Math.h"
#include "memory/UniquePtr.h"

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
class CounterComponent3D : public FComponent3D {
public:
    ACS_GAME_COMPONENT3D_KIND(CounterComponent3D)
    i32 attach = 0, detach = 0, updates = 0, fixed = 0;
    f32 accum = 0.0f;
    i32* detachOut = nullptr;   // OnDetach 後も観測できる外部カウンタ (本体は破棄されるため)
    void OnAttach(FNode3D&) noexcept override { ++attach; }
    void OnUpdate(f32 dt)   noexcept override { ++updates; accum += dt; }
    void OnFixedUpdate(f32) noexcept override { ++fixed; }
    void OnDetach()         noexcept override { ++detach; if (detachOut) ++*detachOut; }
};

// OnSpawn/OnDespawn を数えるテスト用ノード。
class CounterNode3D : public FNode3D {
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
    struct E { f32 x, y, z; };
    const E euls[] = { {30, 0, 0}, {0, 45, 0}, {0, 0, 60}, {20, 35, -50}, {-15, 80, 25} };
    const FVec3 vs[] = { {1, 0, 0}, {0, 1, 0}, {0, 0, 1}, {0.5f, -0.3f, 0.8f} };
    for (const E& e : euls) {
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
    struct E { f32 x, y, z; };
    const E euls[] = { {0, 0, 0}, {30, 0, 0}, {0, 40, 0}, {0, 0, 70},
                       {25, -35, 50}, {-60, 20, -80} };   // |Y|<85° に収める
    for (const E& e : euls) {
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

// === FNode3D 階層 ===========================================================

// --- 親の transform が子の World() に合成される (2 段) -----------------------
ACS_TEST(Node3D, WorldComposesThroughHierarchy) {
    FNode3D root;
    root.Local().position = FVec3{10, 0, 0};
    root.Local().rotation = FQuat::AxisAngle(FVec3{0,0,1}, kHalfPi);

    FNode3D& child = root.AddChild(MakeUnique<FNode3D>());
    child.Local().position = FVec3{2, 0, 0};   // 親フレームで +X に 2

    const FTransform3D w = child.World();
    // root が +90°Z → 子 (2,0,0) は (0,2,0) → +root位置 = (10,2,0)
    ExpectVec3Near(w.position, FVec3{10, 2, 0}, 1e-5f);

    // 孫 (3 段目) も合成される
    FNode3D& g = child.AddChild(MakeUnique<FNode3D>());
    g.Local().position = FVec3{0, 1, 0};
    // child の world 回転も +90°Z (root と同じ) なので (0,1,0) は (-1,0,0) に回り、child world (10,2,0) に足す
    ExpectVec3Near(g.World().position, FVec3{9, 2, 0}, 1e-5f);
}

// --- root の World == Local (親なし) ----------------------------------------
ACS_TEST(Node3D, RootWorldEqualsLocal) {
    FNode3D root;
    root.Local().position = FVec3{1, 2, 3};
    ExpectVec3Near(root.World().position, FVec3{1, 2, 3}, 1e-6f);
}

// === コンポーネント lifecycle ===============================================

// --- AddComponent で OnAttach、UpdateTree で OnUpdate ------------------------
ACS_TEST(Node3D, ComponentLifecycle) {
    FNode3D root;
    i32 detachCount = 0;
    auto& c = root.AddComponent<CounterComponent3D>();
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
    EXPECT_TRUE(root.HasComponent<CounterComponent3D>());
    EXPECT_TRUE(root.GetComponent<CounterComponent3D>() == &c);
    EXPECT_EQ(root.ComponentCount(), 1u);

    // 除去で OnDetach (外部カウンタで観測)
    EXPECT_TRUE(root.RemoveComponent<CounterComponent3D>());
    EXPECT_EQ(detachCount, 1);
    // c は破棄済みなので参照しない。再取得は nullptr。
    EXPECT_TRUE(root.GetComponent<CounterComponent3D>() == nullptr);
    EXPECT_EQ(root.ComponentCount(), 0u);
}

// --- 無効ノードは update をスキップ -----------------------------------------
ACS_TEST(Node3D, DisabledSkipsUpdate) {
    FNode3D root;
    auto& c = root.AddComponent<CounterComponent3D>();
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
    FNode3D root;

    auto childUP = MakeUnique<CounterNode3D>();
    childUP->spawns = &spawns; childUP->despawns = &despawns;
    FNode3D& child = root.AddChild(Move(childUP));
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
    FNode3D root;
    FNode3D& child = root.AddChild(MakeUnique<FNode3D>());
    auto& c = child.AddComponent<CounterComponent3D>();
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
    FNode3D root;
    FNode3D& a = root.AddChild(MakeUnique<FNode3D>());
    FNode3D& b = root.AddChild(MakeUnique<FNode3D>());
    FNode3D& child = a.AddChild(MakeUnique<FNode3D>());
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
    FNode3D root;
    FNode3D& a = root.AddChild(MakeUnique<FNode3D>());
    FNode3D& b = a.AddChild(MakeUnique<FNode3D>());
    // a を自分の子孫 b の下に移すのは cycle → 無視される
    a.Reparent(b);
    EXPECT_TRUE(!a.IsPendingReparent());
    root.ResolveStructuralChanges();
    EXPECT_TRUE(a.Parent() == &root);
    EXPECT_EQ(a.ChildCount(), 1u);
}

// --- 名前の設定/取得 --------------------------------------------------------
ACS_TEST(Node3D, NameSetGet) {
    FNode3D n(FStringView("Cube"));
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

    FNode3D& a = scene.Spawn(FStringView("A"));
    FNode3D& b = scene.Spawn(FStringView("B"));
    scene.Spawn(FStringView("A.child"), &a);
    (void)b;
    EXPECT_EQ(scene.Root().ChildCount(), 2u);          // A, B
    EXPECT_EQ(a.ChildCount(), 1u);
    EXPECT_EQ(scene.NodeCount(), 4u);                  // root + A + B + A.child
}

// --- FindByName で root / 子 / 不在を解決 -----------------------------------
ACS_TEST(Scene3D, FindByName) {
    FScene3D scene;
    FNode3D& a = scene.Spawn(FStringView("Player"));
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
    FNode3D& a = scene.Spawn(FStringView("A"));
    auto& c = a.AddComponent<CounterComponent3D>();

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
    FNode3D& a = scene.Spawn(FStringView("A"));
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

// --- Destroy(id): Update まで valid、reap 後は stale -------------------------
ACS_TEST(Scene3D, DestroyByIdGoesStaleAfterUpdate) {
    FScene3D scene;
    FNode3D& a = scene.Spawn(FStringView("A"));
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
    FNode3D& a = scene.Spawn(FStringView("A"));
    const FNodeId old = a.Id();
    scene.Destroy(old);
    scene.Update(0.016f);                              // a を reap + slot free

    // 新規 Spawn が同じ slot index を再利用するが gen が進む
    FNode3D& b = scene.Spawn(FStringView("B"));
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
    FNode3D& a = scene.Spawn(FStringView("A"));
    const FNodeId ida = a.Id();
    a.Destroy();                                       // scene.Destroy ではなく直接
    scene.Update(0.016f);
    EXPECT_TRUE(!scene.IsValid(ida));                  // PurgePendingDestroy が外す
    EXPECT_EQ(scene.RegisteredCount(), 1u);
}

// === FScene3D::Raycast ======================================================

// --- 上からのレイが原点の cube に当たり、t が正しい --------------------------
ACS_TEST(Scene3DRaycast, HitsCubeFromAbove) {
    FScene3D scene;
    FNode3D& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    cube.Local().position = FVec3{ 0, 0, 0 };

    Ray3 ray{ FVec3{ 0, 5, 0 }, FVec3{ 0, -1, 0 } };   // 真上から下向き
    f32 t = -1.0f;
    const FNodeId hit = scene.Raycast(ray, &t);
    EXPECT_TRUE(hit == cube.Id());
    EXPECT_NEAR(t, 4.5f, 1e-3f);                        // 上面 y=0.5 に y=5 から → t=4.5
}

// --- 外れるレイは invalid を返す --------------------------------------------
ACS_TEST(Scene3DRaycast, MissReturnsInvalid) {
    FScene3D scene;
    FNode3D& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    Ray3 ray{ FVec3{ 10, 5, 10 }, FVec3{ 0, -1, 0 } };  // cube から遠い
    EXPECT_TRUE(!scene.Raycast(ray).IsValid());
}

// --- 2 つのうち手前のノードを返す -------------------------------------------
ACS_TEST(Scene3DRaycast, ReturnsNearest) {
    FScene3D scene;
    FNode3D& near_ = scene.Spawn(FStringView("Near"));
    near_.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    near_.Local().position = FVec3{ 0, 0, 2 };
    FNode3D& far_ = scene.Spawn(FStringView("Far"));
    far_.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    far_.Local().position = FVec3{ 0, 0, 5 };

    Ray3 ray{ FVec3{ 0, 0, -5 }, FVec3{ 0, 0, 1 } };    // +Z 方向
    f32 t = -1.0f;
    const FNodeId hit = scene.Raycast(ray, &t);
    EXPECT_TRUE(hit == near_.Id());
    EXPECT_NEAR(t, 6.5f, 1e-3f);                        // near 手前面 z=1.5 に z=-5 から → 6.5
}

// --- スケールで AABB が拡大し、単位 cube なら外れるレイが当たる (OBB) --------
ACS_TEST(Scene3DRaycast, RespectsScale) {
    FScene3D scene;
    FNode3D& cube = scene.Spawn(FStringView("Cube"));
    cube.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    cube.Local().scale = FVec3{ 3, 3, 3 };              // 半幅 1.5

    Ray3 ray{ FVec3{ 1.2f, 5, 0 }, FVec3{ 0, -1, 0 } }; // x=1.2 は単位(0.5)では外れ、3倍(1.5)で当たる
    EXPECT_TRUE(scene.Raycast(ray) == cube.Id());
}

// --- 回転した cube を正しく OBB ピック (90°Z 回転で縦長 box) -----------------
ACS_TEST(Scene3DRaycast, RespectsRotation) {
    FScene3D scene;
    FNode3D& box = scene.Spawn(FStringView("Box"));
    box.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube);
    box.Local().scale = FVec3{ 3, 0.5f, 0.5f };        // X に長い棒
    // 回転なしなら x=±1.5 まで当たる。Z 90° 回転で «Y に長い» 棒になる → x=1.2 は外れる。
    Ray3 down{ FVec3{ 1.2f, 5, 0 }, FVec3{ 0, -1, 0 } };
    EXPECT_TRUE(scene.Raycast(down) == box.Id());       // 回転前: x=1.2 は半幅1.5内で当たる
    box.Local().SetEulerDeg(FVec3{ 0, 0, 90 });
    EXPECT_TRUE(!scene.Raycast(down).IsValid());        // 回転後: X 半幅が 0.5 になり x=1.2 は外れる
}

// --- FMeshComponent3D の無いノードはピック対象外 ----------------------------
ACS_TEST(Scene3DRaycast, IgnoresNodesWithoutMesh) {
    FScene3D scene;
    FNode3D& empty = scene.Spawn(FStringView("Empty"));   // mesh component なし
    empty.Local().position = FVec3{ 0, 0, 0 };
    Ray3 ray{ FVec3{ 0, 5, 0 }, FVec3{ 0, -1, 0 } };
    EXPECT_TRUE(!scene.Raycast(ray).IsValid());
    (void)empty;
}

// === FMeshComponent3D =======================================================

// --- 既定値 / プリミティブ・色・パスの設定取得 -----------------------------
ACS_TEST(MeshComponent3D, DataRoundTrip) {
    FNode3D node(FStringView("Mesh"));
    auto& m = node.AddComponent<FMeshComponent3D>();
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
    EXPECT_TRUE(node.GetComponent<FMeshComponent3D>() == &m);
}

// --- コンストラクタでプリミティブ指定 ---------------------------------------
ACS_TEST(MeshComponent3D, ConstructWithPrimitive) {
    FNode3D node;
    auto& m = node.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Plane);
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Plane);
}

// === Scene3DSerialize =======================================================

// --- save → load の往復で構造/transform/メッシュ記述が一致する ---------------
ACS_TEST(Scene3DSerialize, RoundTrip) {
    FScene3D a;
    // Box: cube・赤・pos/euler/scale 非自明
    FNode3D& box = a.Spawn(FStringView("Box"));
    box.Local().position = FVec3{ 1, 2, 3 };
    box.Local().SetEulerDeg(FVec3{ 10, 20, 30 });
    box.Local().scale = FVec3{ 2, 2, 2 };
    { auto& m = box.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Cube); m.SetColor(FVec4{0.9f,0.1f,0.1f,1}); }
    // Ball: sphere・青 (root 直下)
    FNode3D& ball = a.Spawn(FStringView("Ball"));
    { auto& m = ball.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Sphere); m.SetColor(FVec4{0.2f,0.4f,0.9f,1}); }
    // Child: Box の子 (階層)
    FNode3D& child = a.Spawn(FStringView("Child"), &box);
    child.Local().position = FVec3{ 0, 1, 0 };
    child.AddComponent<FMeshComponent3D>(EMeshPrimitive3D::Plane);
    // Custom: メッシュパス
    FNode3D& custom = a.Spawn(FStringView("Custom"));
    { auto& m = custom.AddComponent<FMeshComponent3D>(); m.SetMeshPath(FStringView("Assets/teapot.obj")); }

    char buf[4096];
    const u32 wrote = SaveScene3DText(a, buf, sizeof(buf));
    EXPECT_TRUE(wrote > 0);

    FScene3D b;
    EXPECT_TRUE(LoadScene3DText(b, buf));
    // 構造一致 (root + 4 ノード)
    EXPECT_EQ(b.NodeCount(), a.NodeCount());
    EXPECT_EQ(b.NodeCount(), 5u);

    // Box の transform / mesh
    FNode3D* bbox = b.FindByName(FStringView("Box"));
    EXPECT_TRUE(bbox != nullptr);
    ExpectVec3Near(bbox->Local().position, FVec3{1,2,3}, 1e-3f);
    ExpectVec3Near(bbox->Local().scale,    FVec3{2,2,2}, 1e-3f);
    ExpectVec3Near(bbox->Local().EulerDeg(), FVec3{10,20,30}, 2e-2f);
    FMeshComponent3D* bmc = bbox->GetComponent<FMeshComponent3D>();
    EXPECT_TRUE(bmc != nullptr);
    EXPECT_TRUE(bmc->Primitive() == EMeshPrimitive3D::Cube);
    EXPECT_NEAR(bmc->Color().x, 0.9f, 1e-3f);

    // 階層: Child の親が Box
    FNode3D* bchild = b.FindByName(FStringView("Child"));
    EXPECT_TRUE(bchild != nullptr);
    EXPECT_TRUE(bchild->Parent() == bbox);
    ExpectVec3Near(bchild->Local().position, FVec3{0,1,0}, 1e-3f);

    // メッシュパス
    FNode3D* bcustom = b.FindByName(FStringView("Custom"));
    EXPECT_TRUE(bcustom != nullptr);
    FMeshComponent3D* cmc = bcustom->GetComponent<FMeshComponent3D>();
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

// --- メッシュアセットを所有し、外部参照を捨てても生存する -------------------
ACS_TEST(MeshComponent3D, OwnsMeshAsset) {
    FNode3D node(FStringView("Mesh"));
    auto& m = node.AddComponent<FMeshComponent3D>();
    EXPECT_TRUE(!m.HasMeshAsset());
    EXPECT_TRUE(m.Mesh() == nullptr);

    // メッシュを作って 2 頂点入れる
    TSharedPtr<FMeshAsset> mesh = MakeShared<FMeshAsset>();
    mesh->Vertices().PushBack(MeshVertex{ FVec3{0,0,0}, FVec3{0,1,0}, 0.0f, 0.0f });
    mesh->Vertices().PushBack(MeshVertex{ FVec3{1,0,0}, FVec3{0,1,0}, 1.0f, 0.0f });
    FMeshAsset* raw = mesh.Get();

    // Asset 基底へアップキャストして所有させる (種別が Mesh に切り替わる)
    m.SetMeshAsset(TSharedPtr<Asset>(mesh));
    EXPECT_TRUE(m.HasMeshAsset());
    EXPECT_TRUE(m.Primitive() == EMeshPrimitive3D::Mesh);
    EXPECT_TRUE(m.Mesh() == raw);
    EXPECT_EQ(m.Mesh()->Vertices().Size(), 2u);

    // 外部の共有参照を捨てる → コンポーネントが強参照を持つので生存
    mesh.Reset();
    EXPECT_TRUE(m.Mesh() == raw);
    EXPECT_EQ(m.Mesh()->Vertices().Size(), 2u);

    // null を渡すと外れる (種別はそのまま Mesh)
    m.SetMeshAsset(TSharedPtr<Asset>{});
    EXPECT_TRUE(!m.HasMeshAsset());
    EXPECT_TRUE(m.Mesh() == nullptr);
}
