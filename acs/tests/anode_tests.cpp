// SPDX-License-Identifier: Apache-2.0
// ANode (統一シーンノード) の動作確認テスト
//
// FObject 基底の所有/弱参照、2D ヘルパ (FTransform3D 一本化)、そして
// DrawTreeSorted のグローバル描画順 (DrawLayer/DrawPriority/YSort/原子 subtree)
// をヘッドレス (スプライトバッチ未配線の RenderContext) で検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ANode.h"
#include "gameframework/RenderContext.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 描画順の記録先 (テストフィクスチャ)。 */
struct FRecFixture {
    TArray<i32> order;
};

/** OnDraw で自分のタグを記録するコンポーネント。 */
class FRecComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(FRecComponent)
    FRecComponent(FRecFixture* fx, i32 tag) noexcept : m_Fx(fx), m_Tag(tag) {}
    void OnDraw(RenderContext&) noexcept override { m_Fx->order.PushBack(m_Tag); }
private:
    FRecFixture* m_Fx;
    i32          m_Tag;
};

/** subtree を原子描画単位にするマーカ (ステンシルクリップ相当)。 */
class FAtomicMarkComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(FAtomicMarkComponent)
    bool WantsAtomicSubtree() const noexcept override { return true; }
};

/** タグ付きノードを作って親に付ける補助。 */
ANode& AddTagged(ANode& parent, FRecFixture& fx, i32 tag) noexcept
{
    ANode& n = parent.AddChild(NewObject<ANode>());
    n.AddComponent<FRecComponent>(&fx, tag);
    return n;
}

} // namespace

ACS_TEST(ANode, ObjectLifecycleAndWeakRef) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    EXPECT_TRUE(root.IsValid());

    ANode& child = root->AddChild(NewObject<ANode>());
    TWeakObjectPtr<ANode> weak(&child);
    EXPECT_TRUE(weak.IsValid());
    EXPECT_EQ(root->ChildCount(), 1u);

    child.Destroy();
    EXPECT_TRUE(weak.IsValid());          // 破棄はフレーム境界まで遅延
    root->ResolveStructuralChanges();
    EXPECT_EQ(root->ChildCount(), 0u);
    EXPECT_TRUE(weak.IsStale());          // 最後の強参照が切れて破棄済み
}

ACS_TEST(ANode, Transform2DHelpers) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->SetPosition2D({10, 20});
    root->SetRotation2D(0.5f);
    root->SetScale2D({2, 3});
    EXPECT_NEAR(root->Rotation2D(), 0.5f, 1e-4f);
    EXPECT_NEAR(root->Position2D().x, 10.0f, 1e-5f);
    EXPECT_NEAR(root->Position2D().y, 20.0f, 1e-5f);

    // 親 (回転なし・scale (2,3)) と合成した World2D。合成規約により子のローカル
    // オフセットは親 scale で拡大される: (10,20) + (2*1, 3*2) = (12, 26)。
    root->SetRotation2D(0.0f);
    ANode& child = root->AddChild(NewObject<ANode>());
    child.SetPosition2D({1, 2});
    const FTransform2D w = child.World2D();
    EXPECT_NEAR(w.position.x, 12.0f, 1e-4f);
    EXPECT_NEAR(w.position.y, 26.0f, 1e-4f);
    EXPECT_NEAR(w.scale.x, 2.0f, 1e-4f);
    EXPECT_NEAR(w.scale.y, 3.0f, 1e-4f);
}

ACS_TEST(ANode, SortedDrawDefaultsToTreeOrder) {
    FRecFixture fx;
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& a = AddTagged(*root, fx, 1);
    ANode& b = AddTagged(*root, fx, 2);
    AddTagged(b, fx, 3);
    (void)a;

    RenderContext rc;
    root->DrawTreeSorted(rc);
    EXPECT_EQ(fx.order.Size(), 3u);
    if (fx.order.Size() == 3) {
        EXPECT_EQ(fx.order[0], 1);
        EXPECT_EQ(fx.order[1], 2);
        EXPECT_EQ(fx.order[2], 3);
    }
}

ACS_TEST(ANode, DrawLayerReordersAcrossHierarchy) {
    FRecFixture fx;
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& a = AddTagged(*root, fx, 1);          // layer 0
    ANode& b = AddTagged(*root, fx, 2);          // layer -1 (奥へ)
    AddTagged(b, fx, 3);                          // b の子 (layer 0 = a と同格)
    (void)a;
    b.SetDrawLayer(-1);

    RenderContext rc;
    root->DrawTreeSorted(rc);
    // b が階層に関係なく最奥へ。子 3 は独立アイテムとして layer 0 に残り、
    // 出現順で a(1) の後。
    EXPECT_EQ(fx.order.Size(), 3u);
    if (fx.order.Size() == 3) {
        EXPECT_EQ(fx.order[0], 2);
        EXPECT_EQ(fx.order[1], 1);
        EXPECT_EQ(fx.order[2], 3);
    }
}

ACS_TEST(ANode, DrawPriorityOrdersWithinLayer) {
    FRecFixture fx;
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& a = AddTagged(*root, fx, 1);
    ANode& b = AddTagged(*root, fx, 2);
    ANode& c = AddTagged(*root, fx, 3);
    a.SetDrawPriority(5);
    b.SetDrawPriority(-5);
    c.SetDrawPriority(0);

    RenderContext rc;
    root->DrawTreeSorted(rc);
    EXPECT_EQ(fx.order.Size(), 3u);
    if (fx.order.Size() == 3) {
        EXPECT_EQ(fx.order[0], 2);   // -5
        EXPECT_EQ(fx.order[1], 3);   //  0
        EXPECT_EQ(fx.order[2], 1);   // +5
    }
}

ACS_TEST(ANode, YSortOrdersEqualPriorityNodes) {
    FRecFixture fx;
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& a = AddTagged(*root, fx, 1);   // y=50 (画面下 = 手前 = 後)
    ANode& b = AddTagged(*root, fx, 2);   // y=10 (画面上 = 奥 = 先)
    a.SetPosition2D({0, 50});
    b.SetPosition2D({0, 10});
    a.SetYSortEnabled(true);
    b.SetYSortEnabled(true);

    RenderContext rc;
    root->DrawTreeSorted(rc);
    EXPECT_EQ(fx.order.Size(), 2u);
    if (fx.order.Size() == 2) {
        EXPECT_EQ(fx.order[0], 2);
        EXPECT_EQ(fx.order[1], 1);
    }

    // バイアスで逆転できる (a の足元を大きく上へ)。
    fx.order.Clear();
    a.SetYSortBias(-100.0f);   // 実効 y = -50 < 10
    root->DrawTreeSorted(rc);
    if (fx.order.Size() == 2) {
        EXPECT_EQ(fx.order[0], 1);
        EXPECT_EQ(fx.order[1], 2);
    }
}

ACS_TEST(ANode, AtomicSubtreeStaysContiguousAndSortsByRoot) {
    FRecFixture fx;
    TObjectPtr<ANode> root = NewObject<ANode>();
    // 原子 subtree M (layer 1): 内部の子が極端な layer を持っても外へ漏れない。
    ANode& m = AddTagged(*root, fx, 4);
    m.AddComponent<FAtomicMarkComponent>();
    m.SetDrawLayer(1);
    ANode& m1 = AddTagged(m, fx, 5);
    m1.SetDrawLayer(99);                  // 原子内部ではツリー順なので無視される
    // 兄弟 D (layer 2): M subtree 全体の後に描かれる。
    ANode& d = AddTagged(*root, fx, 6);
    d.SetDrawLayer(2);

    RenderContext rc;
    root->DrawTreeSorted(rc);
    EXPECT_EQ(fx.order.Size(), 3u);
    if (fx.order.Size() == 3) {
        EXPECT_EQ(fx.order[0], 4);   // M 自身
        EXPECT_EQ(fx.order[1], 5);   // M の子 (subtree 一塊)
        EXPECT_EQ(fx.order[2], 6);   // D は layer 2 で最後
    }
}
