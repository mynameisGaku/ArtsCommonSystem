// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor 窶・Tree 讒狗ｯ・helper 縺ｮ螳溯｣・・
#include "TreeBuilder.h"
#include "TreeActions.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void BuildPanelMirror(btedit::FBehaviorTreeEditorPanel& panel, BtEditorBb& bb) noexcept {
    // 豕ｨ諢・ AddNode 縺ｮ謇輔＞蜃ｺ縺・id 繧・bb 縺ｫ菫晏ｭ倥☆繧九％縺ｨ縺ｧ縲、ction Fn 縺九ｉ
    // panel.SetNodeStatus 繧貞他縺ｹ繧九ｈ縺・↓縺吶ｋ縲・T 譛ｬ菴薙・讒矩縺ｨ縺薙■繧峨・讒矩繧・
    // 1:1 蟇ｾ蠢懊〒邨・∩遶九※繧玖ｲｬ蜍吶・ sample 蛛ｴ (= panel 縺ｯ螳滉ｽ・BT 繧定ｦ励￠縺ｪ縺・縲・
    bb.id_root     = panel.AddNode(btedit::EBtKind::Selector, "Root Selector", btedit::FBehaviorTreeEditorPanel::kInvalidId);
    bb.id_branch_a = panel.AddNode(btedit::EBtKind::Sequence, "Pickup Branch", bb.id_root);
    bb.id_pickup   = panel.AddNode(btedit::EBtKind::Action,   "Pickup",        bb.id_branch_a);
    bb.id_move     = panel.AddNode(btedit::EBtKind::Action,   "Move",          bb.id_branch_a);
    bb.id_branch_b = panel.AddNode(btedit::EBtKind::Sequence, "Combat Branch", bb.id_root);
    bb.id_wait     = panel.AddNode(btedit::EBtKind::Action,   "Wait",          bb.id_branch_b);
    bb.id_attack   = panel.AddNode(btedit::EBtKind::Action,   "Attack",        bb.id_branch_b);
}

void BuildBehaviorTree(FBehaviorTree& bt) noexcept {
    // 螳・BT 縺ｯ BuildPanelMirror 縺ｧ邨・ｓ縺繝｡繧ｿ繝溘Λ繝ｼ縺ｨ蜷悟ｽ｢縺ｫ邨・・
    // (= Action Fn 縺九ｉ panel 縺ｫ push 縺吶ｋ node id 縺ｨ node 縺ｮ菴咲ｽｮ縺御ｸ閾ｴ縺吶ｋ)縲・
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
