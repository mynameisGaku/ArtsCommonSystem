// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Action Fn 群 + step callback の実装。
//
// 各 Fn は「N frame Running した後 Success/Failure に切替」のタイマ式 stub。
// blackboard を BtEditorBb* に restore し、panel.SetNodeStatus で status を
// push することで、エディタ側のメタミラーが BT 実行の進行に合わせて着色される。
#include "TreeActions.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

// "Pickup" — 1 frame で Success (= 拾うアクションは即時完了)。
EBtStatus ActionPickup(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    const EBtStatus s = EBtStatus::Success;
    if (bb && bb->panel) bb->panel->SetNodeStatus(bb->id_pickup, s);
    return s;
}

// "Move" — kMoveRunFrames frame Running し続けてから Success に遷移。
//   pickup の後に「30 frame かけて拾った先まで歩く」イメージ。
EBtStatus ActionMove(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    if (!bb) return EBtStatus::Failure;

    EBtStatus s = EBtStatus::Running;
    if (bb->counter_move >= BtEditorBb::kMoveRunFrames) {
        s = EBtStatus::Success;
        bb->counter_move = 0; // 次サイクルのために 0 に戻す (= sequence 再評価で再生)
    } else {
        ++bb->counter_move;
    }

    if (bb->panel) {
        bb->panel->SetNodeStatus(bb->id_move, s);
        // sequence "Pickup Branch" の status も同期: 最後の子の status を流す。
        // ※ stateless BtSequence は最後に Success が出れば自分も Success、
        //   途中 Running なら自分も Running なので、最後の子の status を流すと
        //   ほぼ等価に見える (Failure になる Action は本 sample に居ない)。
        bb->panel->SetNodeStatus(bb->id_branch_a, s);
        // root Selector も "成功した最初の枝の status" を返すので、Pickup Branch
        // が Success/Running の間は root も同じ status (Combat Branch には進まない)。
        bb->panel->SetNodeStatus(bb->id_root, s);
    }
    return s;
}

// "Wait" — kWaitRunFrames frame Running した後 Failure に遷移。
//   Failure を返すことで Sequence "Combat Branch" 全体が Failure になり、
//   結果として root Selector も最後に Failure を伝播する (= 試したが全部
//   駄目だった状態)。ただし本 sample は Pickup Branch が常に Success/Running
//   なので、Wait に到達するのは Pickup Branch が Failure を返した場合のみ。
//   現実には到達しないが「Failure が出る経路」の demonstration として残す。
EBtStatus ActionWait(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    if (!bb) return EBtStatus::Failure;

    EBtStatus s = EBtStatus::Running;
    if (bb->counter_wait >= BtEditorBb::kWaitRunFrames) {
        s = EBtStatus::Failure;
        bb->counter_wait = 0;
    } else {
        ++bb->counter_wait;
    }

    if (bb->panel) {
        bb->panel->SetNodeStatus(bb->id_wait, s);
        bb->panel->SetNodeStatus(bb->id_branch_b, s); // Sequence の途中 status
    }
    return s;
}

// "Attack" — 1 frame で Success (本 sample では到達しない予定 = Wait が走るため)。
EBtStatus ActionAttack(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    const EBtStatus s = EBtStatus::Success;
    if (bb && bb->panel) bb->panel->SetNodeStatus(bb->id_attack, s);
    return s;
}

// step callback — panel に登録され、autorun 中の各 tick で呼ばれる。
// blackboard を渡して BT.Tick を 1 回回す責務だけを持つ薄いラッパ。
void StepCallbackFn(void* user, BehaviorTree* tree, f32 dt) noexcept {
    auto* bb = static_cast<BtEditorBb*>(user);
    if (!bb || !tree) return;
    tree->Tick(bb, dt);
}

} // namespace hellobt
