// SPDX-License-Identifier: Apache-2.0
// ANode (統一シーンノード) の動作確認テスト
//
// AObject 基底の所有/弱参照、2D ヘルパ (FTransform3D 一本化)、そして
// DrawTreeSorted のグローバル描画順 (DrawLayer/DrawPriority/YSort/原子 subtree)
// をヘッドレス (スプライトバッチ未配線の FRenderContext) で検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/ANode.h"
#include "gameframework/SceneCommandQueue.h"
#include "gameframework/RenderContext.h"

using namespace acs;
using namespace acs::game;

namespace {

/** 描画順の記録先 (テストフィクスチャ)。 */
struct FRecFixture {
    TArray<i32> order;
};

/** OnDraw で自分のタグを記録するコンポーネント。 */
class ARecordComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(ARecordComponent)
    ARecordComponent(FRecFixture* fx, i32 tag) noexcept : m_Fx(fx), m_Tag(tag) {}
    void OnDraw(FRenderContext&) noexcept override { m_Fx->order.PushBack(m_Tag); }
private:
    FRecFixture* m_Fx;
    i32          m_Tag;
};

/** subtree を原子描画単位にするマーカ (ステンシルクリップ相当)。 */
class AAtomicMarkComponent final : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AAtomicMarkComponent)
    bool WantsAtomicSubtree() const noexcept override { return true; }
};

/** タグ付きノードを作って親に付ける補助。 */
ANode& AddTagged(ANode& parent, FRecFixture& fx, i32 tag) noexcept
{
    ANode& n = parent.AddChild(NewObject<ANode>());
    n.AddComponent<ARecordComponent>(&fx, tag);
    return n;
}

void CountSceneCommand(void* user) noexcept
{
    ++*static_cast<u32*>(user);
}

} // namespace

ACS_TEST(SceneCommandQueue, InlineCapacityAndSpillPreserveExecution)
{
    FSceneCommandQueue queue;
    u32 calls = 0u;
    for (u32 i = 0u; i < 16u; ++i)
        queue.Enqueue("inline", &CountSceneCommand, &calls);
    EXPECT_TRUE(queue.UsesInlineStorage());
    queue.Enqueue("spill", &CountSceneCommand, &calls);
    EXPECT_FALSE(queue.UsesInlineStorage());
    queue.Flush();
    EXPECT_EQ(calls, 17u);
    EXPECT_EQ(queue.PendingCount(), 0u);
}

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

ACS_TEST(ANode, TryAddChildRejectsNullAndPreservesOwnershipOnFailure) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    TObjectPtr<ANode> empty;
    EXPECT_EQ(static_cast<u32>(root->TryAddChild(empty)),
              static_cast<u32>(EAddChildResult::NullChild));
    EXPECT_EQ(root->ChildCount(), 0u);

    TObjectPtr<ANode> child = NewObject<ANode>();
    child->Destroy();
    ANode* child_address = child.Get();
    EXPECT_EQ(static_cast<u32>(root->TryAddChild(child)),
              static_cast<u32>(EAddChildResult::ChildPendingDestroy));
    EXPECT_TRUE(child.IsValid());
    EXPECT_TRUE(child.Get() == child_address);
    EXPECT_TRUE(child->Parent() == nullptr);
    EXPECT_EQ(root->ChildCount(), 0u);

    root->Destroy();
    TObjectPtr<ANode> other = NewObject<ANode>();
    EXPECT_EQ(static_cast<u32>(root->TryAddChild(other)),
              static_cast<u32>(EAddChildResult::ParentPendingDestroy));
    EXPECT_TRUE(other.IsValid());
    EXPECT_TRUE(other->Parent() == nullptr);
}

ACS_TEST(ANode, TryAddChildRejectsSelfCycleAndMultipleParents) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    TObjectPtr<ANode> second_root = NewObject<ANode>();
    TObjectPtr<ANode> child = NewObject<ANode>();
    TObjectPtr<ANode> retained_child = child;

    EXPECT_EQ(static_cast<u32>(root->TryAddChild(child)),
              static_cast<u32>(EAddChildResult::Added));
    EXPECT_FALSE(child.IsValid()); // 成功時だけ所有権が移る
    EXPECT_TRUE(retained_child->Parent() == root.Get());

    TObjectPtr<ANode> already_parented = retained_child;
    EXPECT_EQ(static_cast<u32>(second_root->TryAddChild(already_parented)),
              static_cast<u32>(EAddChildResult::AlreadyParented));
    EXPECT_TRUE(already_parented.IsValid());
    EXPECT_TRUE(already_parented->Parent() == root.Get());
    EXPECT_EQ(root->ChildCount(), 1u);
    EXPECT_EQ(second_root->ChildCount(), 0u);

    TObjectPtr<ANode> self = root;
    EXPECT_EQ(static_cast<u32>(root->TryAddChild(self)),
              static_cast<u32>(EAddChildResult::SelfChild));
    EXPECT_TRUE(self.IsValid());
    EXPECT_EQ(root->ChildCount(), 1u);

    // root は child の祖先。child の下へ root を付けると循環するため拒否される。
    TObjectPtr<ANode> ancestor = root;
    EXPECT_EQ(static_cast<u32>(retained_child->TryAddChild(ancestor)),
              static_cast<u32>(EAddChildResult::WouldCreateCycle));
    EXPECT_TRUE(ancestor.IsValid());
    EXPECT_TRUE(root->Parent() == nullptr);
    EXPECT_EQ(retained_child->ChildCount(), 0u);

    // 互換 API も不正構造を作らず、従来契約どおり失敗時は呼び出し先自身を返す。
    ANode& fallback = retained_child->AddChild(root);
    EXPECT_TRUE(&fallback == retained_child.Get());
    EXPECT_TRUE(root->Parent() == nullptr);
}

ACS_TEST(ANode, TreeDepthLimitAndIterativeWorldComposition) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    root->Local().position.x = 1.0f;
    ANode* tail = root.Get();

    for (u32 depth = 1u; depth <= kNodeMaxTreeDepth; ++depth) {
        TObjectPtr<ANode> next = NewObject<ANode>();
        next->Local().position.x = 1.0f;
        ANode* next_address = next.Get();
        EXPECT_EQ(static_cast<u32>(tail->TryAddChild(next)),
                  static_cast<u32>(EAddChildResult::Added));
        EXPECT_FALSE(next.IsValid());
        tail = next_address;
    }

    EXPECT_TRUE(tail != nullptr);
    EXPECT_NEAR(tail->World().position.x,
                static_cast<f32>(kNodeMaxTreeDepth + 1u), 1e-3f);

    TObjectPtr<ANode> overflow = NewObject<ANode>();
    EXPECT_EQ(static_cast<u32>(tail->TryAddChild(overflow)),
              static_cast<u32>(EAddChildResult::TreeDepthLimitExceeded));
    EXPECT_TRUE(overflow.IsValid());
    EXPECT_TRUE(overflow->Parent() == nullptr);
    EXPECT_EQ(tail->ChildCount(), 0u);

    TObjectPtr<ANode> source_root = NewObject<ANode>();
    ANode& moving = source_root->AddChild(NewObject<ANode>());
    moving.Reparent(*tail);
    EXPECT_FALSE(moving.IsPendingReparent());
    EXPECT_TRUE(moving.Parent() == source_root.Get());
}

ACS_TEST(ANode, ReparentSupportsValueOwnedTargetAndTracksItsLifetime) {
    ANode source_root;
    ANode value_target;
    ANode& moving = source_root.AddChild(NewObject<ANode>());

    moving.Reparent(value_target);
    EXPECT_TRUE(moving.IsPendingReparent());
    source_root.ResolveStructuralChanges();
    EXPECT_EQ(source_root.ChildCount(), 0u);
    EXPECT_EQ(value_target.ChildCount(), 1u);
    EXPECT_TRUE(moving.Parent() == &value_target);

    ANode second_root;
    ANode& cancelled = second_root.AddChild(NewObject<ANode>());
    {
        ANode short_lived_target;
        cancelled.Reparent(short_lived_target);
        EXPECT_TRUE(cancelled.IsPendingReparent());
    }
    // 値所有 target のデストラクタが observer を無効化する。
    EXPECT_FALSE(cancelled.IsPendingReparent());
    second_root.ResolveStructuralChanges();
    EXPECT_EQ(second_root.ChildCount(), 1u);
    EXPECT_TRUE(cancelled.Parent() == &second_root);
}

ACS_TEST(ANode, ReparentManagedTargetDestroyedBeforeSourceResolveIsCancelled) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& target_branch = root->AddChild(NewObject<ANode>());
    ANode& source_branch = root->AddChild(NewObject<ANode>());
    ANode& target = target_branch.AddChild(NewObject<ANode>());
    ANode& moving = source_branch.AddChild(NewObject<ANode>());
    TWeakObjectPtr<ANode> target_weak(&target);
    TWeakObjectPtr<ANode> moving_weak(&moving);

    moving.Reparent(target);
    target.Destroy();
    root->ResolveStructuralChanges();

    EXPECT_TRUE(target_weak.IsStale());
    EXPECT_TRUE(moving_weak.IsValid());
    EXPECT_FALSE(moving.IsPendingReparent());
    EXPECT_TRUE(moving.Parent() == &source_branch);
    EXPECT_EQ(source_branch.ChildCount(), 1u);
}

ACS_TEST(ANode, ReparentPendingDestroyTargetIsRevalidatedAtCommit) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    // source を先に走査し、target がまだ生存している pending 状態で適用判定させる。
    ANode& source_branch = root->AddChild(NewObject<ANode>());
    ANode& target_branch = root->AddChild(NewObject<ANode>());
    ANode& moving = source_branch.AddChild(NewObject<ANode>());
    ANode& target = target_branch.AddChild(NewObject<ANode>());
    TWeakObjectPtr<ANode> target_weak(&target);

    moving.Reparent(target);
    target.Destroy();
    root->ResolveStructuralChanges();

    EXPECT_TRUE(target_weak.IsStale());
    EXPECT_FALSE(moving.IsPendingReparent());
    EXPECT_TRUE(moving.Parent() == &source_branch);
    EXPECT_EQ(source_branch.ChildCount(), 1u);
}

ACS_TEST(ANode, ReciprocalReparentRequestsRevalidateCycleAtCommit) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& a = root->AddChild(NewObject<ANode>());
    ANode& b = root->AddChild(NewObject<ANode>());

    a.Reparent(b);
    b.Reparent(a);
    EXPECT_TRUE(a.IsPendingReparent());
    EXPECT_TRUE(b.IsPendingReparent());
    root->ResolveStructuralChanges();

    // 先に a→b が適用され、後続 b→a は新しい構造では循環になるため拒否される。
    EXPECT_FALSE(a.IsPendingReparent());
    EXPECT_FALSE(b.IsPendingReparent());
    EXPECT_TRUE(b.Parent() == root.Get());
    EXPECT_TRUE(a.Parent() == &b);
    EXPECT_EQ(root->ChildCount(), 1u);
    EXPECT_EQ(b.ChildCount(), 1u);
}

ACS_TEST(ANode, ReparentDepthIsRevalidatedAfterEarlierMove) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& deepening = root->AddChild(NewObject<ANode>());
    ANode& moving = root->AddChild(NewObject<ANode>());

    ANode* tail = root.Get();
    for (u32 depth = 1u; depth < kNodeMaxTreeDepth; ++depth) {
        tail = &tail->AddChild(NewObject<ANode>());
    }
    EXPECT_EQ(tail->TreeDepth(), kNodeMaxTreeDepth - 1u);

    // 予約時はどちらも合法。適用順により deepening が深度上限へ先に移動する。
    deepening.Reparent(*tail);
    moving.Reparent(deepening);
    EXPECT_TRUE(deepening.IsPendingReparent());
    EXPECT_TRUE(moving.IsPendingReparent());
    root->ResolveStructuralChanges();

    EXPECT_TRUE(deepening.Parent() == tail);
    EXPECT_EQ(deepening.TreeDepth(), kNodeMaxTreeDepth);
    EXPECT_TRUE(moving.Parent() == root.Get());
    EXPECT_FALSE(moving.IsPendingReparent());
}

ACS_TEST(ANode, DestroyedReparentSourceUnlinksTargetObserver) {
    TObjectPtr<ANode> root = NewObject<ANode>();
    ANode& source_branch = root->AddChild(NewObject<ANode>());
    ANode& target_branch = root->AddChild(NewObject<ANode>());
    ANode& source = source_branch.AddChild(NewObject<ANode>());
    ANode& target = target_branch.AddChild(NewObject<ANode>());
    TWeakObjectPtr<ANode> source_weak(&source);
    TWeakObjectPtr<ANode> target_weak(&target);

    source.Reparent(target);
    source.Destroy();
    root->ResolveStructuralChanges();
    EXPECT_TRUE(source_weak.IsStale());
    EXPECT_TRUE(target_weak.IsValid());

    // source の observer が残っていれば、ここで破棄済み source を触ってしまう。
    target.Destroy();
    root->ResolveStructuralChanges();
    EXPECT_TRUE(target_weak.IsStale());
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

    FRenderContext rc;
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

    FRenderContext rc;
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

    FRenderContext rc;
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

    FRenderContext rc;
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
    m.AddComponent<AAtomicMarkComponent>();
    m.SetDrawLayer(1);
    ANode& m1 = AddTagged(m, fx, 5);
    m1.SetDrawLayer(99);                  // 原子内部ではツリー順なので無視される
    // 兄弟 D (layer 2): M subtree 全体の後に描かれる。
    ANode& d = AddTagged(*root, fx, 6);
    d.SetDrawLayer(2);

    FRenderContext rc;
    root->DrawTreeSorted(rc);
    EXPECT_EQ(fx.order.Size(), 3u);
    if (fx.order.Size() == 3) {
        EXPECT_EQ(fx.order[0], 4);   // M 自身
        EXPECT_EQ(fx.order[1], 5);   // M の子 (subtree 一塊)
        EXPECT_EQ(fx.order[2], 6);   // D は layer 2 で最後
    }
}
