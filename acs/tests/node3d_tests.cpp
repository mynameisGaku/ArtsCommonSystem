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
#include "gameframework/MeshComponent3D.h"
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
