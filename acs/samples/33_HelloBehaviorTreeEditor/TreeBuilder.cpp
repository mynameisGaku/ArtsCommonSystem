// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Tree 構築 helper の実装。
#include "TreeBuilder.h"
#include "TreeActions.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void BuildPanelMirror(btedit::FBehaviorTreeEditorPanel& panel, BtEditorBb& bb) noexcept {
    // 注意: AddNode の払い出す id を bb に保存することで、Action Fn から
    // panel.SetNodeStatus を呼べるようにする。BT 本体の構造とこちらの構造を
    // 1:1 対応で組み立てる責務は sample 側 (= panel は実体 BT を覗けない)。
    bb.id_root     = panel.AddNode(btedit::EBtKind::Selector, "Root Selector", btedit::FBehaviorTreeEditorPanel::kInvalidId);
    bb.id_branch_a = panel.AddNode(btedit::EBtKind::FSequence, "Pickup Branch", bb.id_root);
    bb.id_pickup   = panel.AddNode(btedit::EBtKind::Action,   "Pickup",        bb.id_branch_a);
    bb.id_move     = panel.AddNode(btedit::EBtKind::Action,   "Move",          bb.id_branch_a);
    bb.id_branch_b = panel.AddNode(btedit::EBtKind::FSequence, "Combat Branch", bb.id_root);
    bb.id_wait     = panel.AddNode(btedit::EBtKind::Action,   "Wait",          bb.id_branch_b);
    bb.id_attack   = panel.AddNode(btedit::EBtKind::Action,   "Attack",        bb.id_branch_b);
}

void BuildBehaviorTree(FBehaviorTree& bt) noexcept {
    // 実 BT は BuildPanelMirror で組んだメタミラーと同形に組む
    // (= Action Fn から panel に push する node id と node の位置が一致する)。
    auto root  = MakeUnique<FBtSelector>();

    auto seq_a = MakeUnique<FBtSequence>();
    seq_a->AddChild(MakeUnique<FBtAction>(&ActionPickup));
    seq_a->AddChild(MakeUnique<FBtAction>(&ActionMove));
    root->AddChild(Move(seq_a));

    auto seq_b = MakeUnique<FBtSequence>();
    seq_b->AddChild(MakeUnique<FBtAction>(&ActionWait));
    seq_b->AddChild(MakeUnique<FBtAction>(&ActionAttack));
    root->AddChild(Move(seq_b));

    bt.SetRoot(Move(root));
}

} // namespace hellobt
