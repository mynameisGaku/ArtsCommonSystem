// SPDX-License-Identifier: Apache-2.0
// gameframework/BehaviorTree.h の検証:
//   ・ランタイム合成意味論 (Selector=OR / Sequence=AND / Running 伝播)
//   ・ABtDecorator + ApplyDecorator (Inverter / ForceSuccess / ForceFailure / Repeat)
//   ・BtCompareVar / BtCompareF32 (no-code 比較条件)
//   ・Cast<T> / IsA / CastChecked による ABtNode 階層の RTTI 不使用ダウンキャスト
#include "test/Test.h"
#include "test/Expect.h"
#include "gameframework/BehaviorTree.h"
#if WITH_RENDER_DX12_RAW
#    include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h" // bake (BuildRuntimeTree)
#endif
#include "foundation/Cast.h"
#include "memory/UniquePtr.h"

#include <cstddef> // offsetof

using namespace acs;
using namespace acs::game;

namespace {
EBtStatus RetSuccess(void*, f32) noexcept
{
    return EBtStatus::Success;
}
EBtStatus RetFailure(void*, f32) noexcept
{
    return EBtStatus::Failure;
}
EBtStatus RetRunning(void*, f32) noexcept
{
    return EBtStatus::Running;
}
} // namespace

ACS_TEST(BehaviorTree, SelectorOrSemantics)
{
    // 最初に non-Failure を返した子で停止 (= OR)。
    ABtSelector sel;
    sel.AddChild(MakeUnique<ABtAction>(&RetFailure));
    sel.AddChild(MakeUnique<ABtAction>(&RetSuccess));
    sel.AddChild(MakeUnique<ABtAction>(&RetRunning)); // 到達しない
    EXPECT_TRUE(sel.Tick(nullptr, 0.0f) == EBtStatus::Success);

    ABtSelector all_fail;
    all_fail.AddChild(MakeUnique<ABtAction>(&RetFailure));
    all_fail.AddChild(MakeUnique<ABtAction>(&RetFailure));
    EXPECT_TRUE(all_fail.Tick(nullptr, 0.0f) == EBtStatus::Failure);
}

ACS_TEST(BehaviorTree, SequenceAndSemantics)
{
    // 最初に non-Success を返した子で停止 (= AND)。
    ABtSequence seq;
    seq.AddChild(MakeUnique<ABtAction>(&RetSuccess));
    seq.AddChild(MakeUnique<ABtAction>(&RetFailure)); // ここで停止
    seq.AddChild(MakeUnique<ABtAction>(&RetSuccess)); // 到達しない
    EXPECT_TRUE(seq.Tick(nullptr, 0.0f) == EBtStatus::Failure);

    ABtSequence all_ok;
    all_ok.AddChild(MakeUnique<ABtAction>(&RetSuccess));
    all_ok.AddChild(MakeUnique<ABtAction>(&RetSuccess));
    EXPECT_TRUE(all_ok.Tick(nullptr, 0.0f) == EBtStatus::Success);
}

ACS_TEST(BehaviorTree, RunningPropagates)
{
    ABtSequence seq;
    seq.AddChild(MakeUnique<ABtAction>(&RetSuccess));
    seq.AddChild(MakeUnique<ABtAction>(&RetRunning)); // Running を伝播
    EXPECT_TRUE(seq.Tick(nullptr, 0.0f) == EBtStatus::Running);

    ABtSelector sel;
    sel.AddChild(MakeUnique<ABtAction>(&RetRunning)); // Selector も Running を伝播
    EXPECT_TRUE(sel.Tick(nullptr, 0.0f) == EBtStatus::Running);

    // nullptr fn = ソフトフェイル。空 Selector=Failure / 空 Sequence=Success。
    ABtSelector empty_sel;
    ABtSequence empty_seq;
    EXPECT_TRUE(empty_sel.Tick(nullptr, 0.0f) == EBtStatus::Failure);
    EXPECT_TRUE(empty_seq.Tick(nullptr, 0.0f) == EBtStatus::Success);
}

ACS_TEST(BehaviorTree, ApplyDecoratorPureFn)
{
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Inverter, EBtStatus::Success) == EBtStatus::Failure);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Inverter, EBtStatus::Failure) == EBtStatus::Success);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Inverter, EBtStatus::Running) == EBtStatus::Running);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::ForceSuccess, EBtStatus::Failure) == EBtStatus::Success);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::ForceSuccess, EBtStatus::Running) == EBtStatus::Running);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::ForceFailure, EBtStatus::Success) == EBtStatus::Failure);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Repeat, EBtStatus::Success) == EBtStatus::Running);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Repeat, EBtStatus::Failure) == EBtStatus::Failure);
    EXPECT_TRUE(ApplyDecorator(EBtDecoratorOp::Repeat, EBtStatus::Running) == EBtStatus::Running);
}

ACS_TEST(BehaviorTree, DecoratorNode)
{
    ABtDecorator inv(EBtDecoratorOp::Inverter);
    inv.SetChild(MakeUnique<ABtAction>(&RetFailure));
    EXPECT_TRUE(inv.HasChild());
    EXPECT_TRUE(inv.Tick(nullptr, 0.0f) == EBtStatus::Success); // Failure を反転

    ABtDecorator no_child(EBtDecoratorOp::Inverter);
    EXPECT_FALSE(no_child.HasChild());
    EXPECT_TRUE(no_child.Tick(nullptr, 0.0f) == EBtStatus::Failure); // 子なしはソフトフェイル
}

ACS_TEST(BehaviorTree, CompareVar)
{
    struct FBb {
        f32 hp;
        i32 ammo;
        bool alive;
    };
    FBb bb{25.0f, 3, true};
    EXPECT_TRUE(BtCompareVar(&bb, static_cast<u32>(offsetof(FBb, hp)), EBtVarType::F32, EBtCompareOp::Less, 30.0f));
    EXPECT_FALSE(BtCompareVar(&bb, static_cast<u32>(offsetof(FBb, hp)), EBtVarType::F32, EBtCompareOp::Greater, 30.0f));
    EXPECT_TRUE(BtCompareVar(&bb, static_cast<u32>(offsetof(FBb, ammo)), EBtVarType::I32, EBtCompareOp::Equal, 3.0f));
    EXPECT_TRUE(
        BtCompareVar(&bb, static_cast<u32>(offsetof(FBb, alive)), EBtVarType::Bool, EBtCompareOp::NotEqual, 0.0f));
    EXPECT_FALSE(BtCompareVar(nullptr, 0u, EBtVarType::F32, EBtCompareOp::Less, 30.0f)); // null は false
    EXPECT_TRUE(BtCompareF32(25.0f, EBtCompareOp::LessEq, 25.0f));
    EXPECT_TRUE(BtCompareF32(25.0f, EBtCompareOp::GreaterEq, 25.0f));
}

ACS_TEST(BehaviorTree, CastNodeHierarchy)
{
    auto sel = MakeUnique<ABtSelector>();
    ABtNode* node = sel.Get();
    EXPECT_TRUE(Cast<ABtSelector>(node) != nullptr);
    EXPECT_TRUE(Cast<ABtSelector>(node) == node);    // 同一ポインタ
    EXPECT_TRUE(Cast<ABtSequence>(node) == nullptr); // 兄弟型ではない
    EXPECT_TRUE(IsA<ABtNode>(node));
    EXPECT_TRUE(IsA<ABtSelector>(node));
    EXPECT_FALSE(IsA<ABtAction>(node));

    auto act = MakeUnique<ABtAction>(&RetSuccess);
    ABtNode* an = act.Get();
    EXPECT_TRUE(Cast<ABtAction>(an) != nullptr);
    EXPECT_TRUE(Cast<ABtSelector>(an) == nullptr);

    auto deco = MakeUnique<ABtDecorator>(EBtDecoratorOp::Inverter);
    ABtNode* dn = deco.Get();
    EXPECT_TRUE(Cast<ABtDecorator>(dn) != nullptr);
    EXPECT_TRUE(CastChecked<ABtDecorator>(dn) == dn); // 成功パス
    EXPECT_TRUE(dn->GetClassId() == ABtDecorator::StaticClassId());
}

#if WITH_RENDER_DX12_RAW
ACS_TEST(BtBake, GraphToRuntimeTree)
{
    using namespace acs::game::btedit;
    // エディタのグラフを bake → 実行可能 CBehaviorTree として走らせ、意味論一致を確認。
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    CBtActionRegistry reg;
    reg.Register("Win", &RetSuccess);
    FBtBlackboard bb;
    bb.Add("hp", EBtVarType::F32);
    bb.SetF32("hp", 10.0f);
    panel.SetActionRegistry(&reg);
    panel.SetDynamicBlackboard(&bb);

    // Sequence{ Compare[hp < 30] -> Action "Win" }
    const u32 root = panel.AddNode(EBtKind::Sequence, "root", ABehaviorTreeEditorPanel::kInvalidId);
    const u32 cmp = panel.AddNode(EBtKind::Decorator, "guard", root);
    panel.SetNodeCompare(cmp, "hp", EBtCompareOp::Less, 30.0f);
    panel.AddNode(EBtKind::Action, "Win", cmp);

    TUniquePtr<ABtNode> tree = panel.BuildRuntimeTree();
    EXPECT_TRUE(static_cast<bool>(tree));

    CBehaviorTree bt;
    bt.SetRoot(Move(tree));
    EXPECT_TRUE(bt.Tick(&bb, 0.016f) == EBtStatus::Success); // hp=10<30 → guard 通過 → Win
    bb.SetF32("hp", 50.0f);
    EXPECT_TRUE(bt.Tick(&bb, 0.016f) == EBtStatus::Failure); // hp=50 → guard 失敗 → Failure
}

ACS_TEST(BtBake, CompareSchemaMode)
{
    using namespace acs::game::btedit;
    // offset スキーマ (raw 構造体) モデルの Compare も bake → 正しく走る (UB なし)。
    struct FGameBb {
        f32 hp;
    };
    FGameBb game{10.0f};
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    CBtActionRegistry reg;
    reg.Register("Win", &RetSuccess);
    FBtBlackboardSchema sch;
    sch.Add("hp", EBtVarType::F32, static_cast<u32>(offsetof(FGameBb, hp)));
    panel.SetActionRegistry(&reg);
    panel.SetBlackboardSchema(&sch); // 動的 BB は設定しない (schema のみ)
    panel.SetGraphBlackboard(&game); // raw 構造体を blackboard に

    const u32 root = panel.AddNode(EBtKind::Sequence, "root", ABehaviorTreeEditorPanel::kInvalidId);
    const u32 cmp = panel.AddNode(EBtKind::Decorator, "guard", root);
    panel.SetNodeCompare(cmp, "hp", EBtCompareOp::Less, 30.0f);
    panel.AddNode(EBtKind::Action, "Win", cmp);

    CBehaviorTree bt;
    bt.SetRoot(panel.BuildRuntimeTree());
    EXPECT_TRUE(bt.Tick(&game, 0.016f) == EBtStatus::Success); // schema offset で hp=10<30
    game.hp = 50.0f;
    EXPECT_TRUE(bt.Tick(&game, 0.016f) == EBtStatus::Failure);
}

ACS_TEST(BtSerialize, DynamicBlackboardRoundTrip)
{
    using namespace acs::game::btedit;
    const char* path = "bt_serialize_test.btg";

    // save: 3 種の変数 + Compare ノードを書き出す。
    {
        ABehaviorTreeEditorPanel panel;
        panel.Init();
        FBtBlackboard bb;
        bb.Add("hp", EBtVarType::F32);
        bb.SetF32("hp", 42.5f);
        bb.Add("ammo", EBtVarType::I32);
        bb.SetI32("ammo", 7);
        bb.Add("alive", EBtVarType::Bool);
        bb.SetBool("alive", true);
        panel.SetDynamicBlackboard(&bb);
        const u32 g = panel.AddNode(EBtKind::Decorator, "g", ABehaviorTreeEditorPanel::kInvalidId);
        panel.SetNodeCompare(g, "hp", EBtCompareOp::Less, 30.0f);
        EXPECT_TRUE(panel.SaveGraph(path));
    }

    // load: 別パネル + 空の bb に復元する。
    {
        ABehaviorTreeEditorPanel panel;
        panel.Init();
        FBtBlackboard bb;
        panel.SetDynamicBlackboard(&bb);
        EXPECT_TRUE(panel.LoadGraph(path));
        EXPECT_EQ(bb.Count(), 3u);
        EXPECT_NEAR(bb.GetF32("hp"), 42.5f, 0.001f);
        EXPECT_EQ(bb.GetI32("ammo"), 7);
        EXPECT_TRUE(bb.GetBool("alive"));
        EXPECT_TRUE(panel.NodeCount() >= 1u); // ノードも復元
    }
}

ACS_TEST(BtUndo, UndoRedoNodeCount)
{
    using namespace acs::game::btedit;
    ABehaviorTreeEditorPanel panel;
    panel.Init();
    EXPECT_FALSE(panel.CanUndo());

    panel.PushUndoCheckpoint(); // baseline = 空 (0 nodes)
    panel.AddNode(EBtKind::Selector, "a", ABehaviorTreeEditorPanel::kInvalidId);
    panel.PushUndoCheckpoint(); // undo=[0], baseline=[1]
    panel.AddNode(EBtKind::Sequence, "b", ABehaviorTreeEditorPanel::kInvalidId);
    panel.PushUndoCheckpoint(); // undo=[0],[1], baseline=[2]
    EXPECT_EQ(panel.NodeCount(), 2u);

    EXPECT_TRUE(panel.CanUndo());
    panel.Undo();
    EXPECT_EQ(panel.NodeCount(), 1u);
    panel.Undo();
    EXPECT_EQ(panel.NodeCount(), 0u);
    EXPECT_FALSE(panel.CanUndo());

    EXPECT_TRUE(panel.CanRedo());
    panel.Redo();
    EXPECT_EQ(panel.NodeCount(), 1u);
    panel.Redo();
    EXPECT_EQ(panel.NodeCount(), 2u);
    EXPECT_FALSE(panel.CanRedo());
}
#endif
