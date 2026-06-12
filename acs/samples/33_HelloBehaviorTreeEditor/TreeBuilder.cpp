// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Tree 構築 helper の実装。
#include "TreeBuilder.h"
#include "TreeActions.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

acs::u32 BuildPanelMirror(btedit::FBehaviorTreeEditorPanel& panel) noexcept {
    // エディタのグラフ (メタミラー) を構築する。no-code 実行では panel がこの構造を
    // 直接インタプリトして走らせ、各ノードの実行状況をライブ表示する。
    //
    // 「ソースの関数/変数とリンクした Condition / Compare デコレーター」が流れを切り替える:
    //
    //   Root Selector                          ← 子を順に試し最初の Success/Running を採用
    //   ├─ CanSeePlayer  (Decorator[Condition]) → Attack (Task)   見えてる間だけ攻撃
    //   ├─ see_phase>90  (Decorator[Compare])   → Wait   (Task)   位相後半だけ待機
    //   ├─ health<30     (Decorator[Compare])   → Pickup (Task)   体力が低い間だけ回収
    //   └─ Move          (Task)                                   どれも外れたら巡回
    //
    //   CanSeePlayer は動的変数 see_phase を毎 tick 進めつつ前半 (<60) で true を返す。
    //   ・see_phase 0..59   : CanSeePlayer=true  → Attack 枝
    //   ・see_phase 60..90  : 条件オフ (health=100) → Move へフォールバック
    //   ・see_phase 91..119 : see_phase>90=true → Wait 枝
    //   health はコードが更新しない動的変数。エディタの Blackboard パネルで health を
    //   30 未満に poke すると Pickup 枝がライブで点く = 「エディタで変数を編集 → 流れが変わる」。
    using K  = btedit::EBtKind;
    using DM = btedit::EBtDecoMode;
    const u32 INV = btedit::FBehaviorTreeEditorPanel::kInvalidId;

    const u32 root = panel.AddNode(K::Selector, "Root Selector", INV);

    // (1) Condition デコレーター: bool 関数 CanSeePlayer を名前で解決し、true の間だけ子を実行。
    const u32 see = panel.AddNode(K::Decorator, "CanSeePlayer", root);
    panel.SetNodeDecoratorMode(see, DM::Condition);
    panel.AddNode(K::Task, "Attack", see);

    // (2) Compare デコレーター: 動的変数 see_phase を定数 90 と比較し、超えた間だけ子を実行。
    const u32 cmp = panel.AddNode(K::Decorator, "phase gate", root);
    panel.SetNodeCompare(cmp, "see_phase", EBtCompareOp::Greater, 90.0f);
    panel.AddNode(K::Task, "Wait", cmp);

    // (3) Compare デコレーター: 動的変数 health < 30 のときだけ Pickup (poke デモ)。
    const u32 low = panel.AddNode(K::Decorator, "low health", root);
    panel.SetNodeCompare(low, "health", EBtCompareOp::Less, 30.0f);
    panel.AddNode(K::Task, "Pickup", low);

    // (4) どの条件も外れたときの巡回。
    panel.AddNode(K::Task, "Move", root);

    return root;
}

void BuildBehaviorTree(FBehaviorTree& bt) noexcept {
    // コードで BT を組む参考実装 (graph-run に置き換えたため現サンプルでは未使用)。
    // ランタイム FBtSelector / FBtSequence / FBtAction / FBtDecorator の組み立て例。
    auto root  = MakeUnique<FBtSelector>();

    auto seq_a = MakeUnique<FBtSequence>();
    seq_a->AddChild(MakeUnique<FBtAction>(&ActionPickup));

    // Decorator(Inverter) で Wait の結果を反転して Sequence を前進させる。
    auto guard = MakeUnique<FBtDecorator>(EBtDecoratorOp::Inverter);
    guard->SetChild(MakeUnique<FBtAction>(&ActionWait));
    seq_a->AddChild(Move(guard));

    seq_a->AddChild(MakeUnique<FBtAction>(&ActionMove));
    root->AddChild(Move(seq_a));

    auto seq_b = MakeUnique<FBtSequence>();
    seq_b->AddChild(MakeUnique<FBtAction>(&ActionAttack));
    root->AddChild(Move(seq_b));

    bt.SetRoot(Move(root));
}

} // namespace hellobt
