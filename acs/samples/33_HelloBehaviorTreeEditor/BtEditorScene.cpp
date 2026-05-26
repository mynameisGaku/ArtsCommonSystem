// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorScene + Action Fn 群 + step callback 実装。
//
// Action Fn 群と step callback は .cpp 内 static として定義する (= sample 内
// のみで使う file-local 関数)。BtEditorBb は Scene が所有し、Tick から
// blackboard として渡される。
#include "BtEditorScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellobt {

// ----------------------------------------------------------------------------
// Action Fn 群 — 全て BtAction::Fn (= EBtStatus(*)(void*, f32) noexcept) シグネチャ
// ----------------------------------------------------------------------------
// blackboard を BtEditorBb* にキャストし、panel.SetNodeStatus で status を push。
// 各 Fn は「N frame Running した後 Success/Failure に切替」のタイマ式。

namespace {

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

// ----------------------------------------------------------------------------
// step callback — panel.SetOnStepCallback で登録、blackboard を渡して Tick
// ----------------------------------------------------------------------------
void StepCallbackFn(void* user, BehaviorTree* tree, f32 dt) noexcept {
    auto* bb = static_cast<BtEditorBb*>(user);
    if (!bb || !tree) return;
    tree->Tick(bb, dt);
}

} // namespace

// ============================================================================
// BtEditorScene 実装
// ============================================================================
void BtEditorScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (background は ImGui がほぼ覆う)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- panel 初期化 (順序: Init → AddNode 構造登録 → SetTree → SetCallback) ----
    _panel.Init();

    // ---- メタミラー組立: panel に BT 構造を教える -----
    // 注意: AddNode の払い出す id を _bb に保存することで、Action Fn から
    // panel.SetNodeStatus を呼べるようにする。BT 本体の構造とこちらの構造を
    // 1:1 対応で組み立てる責務は sample 側 (= panel は実体 BT を覗けない)。
    _bb.id_root     = _panel.AddNode(btedit::EBtKind::Selector, "Root Selector",      btedit::BehaviorTreeEditorPanel::kInvalidId);
    _bb.id_branch_a = _panel.AddNode(btedit::EBtKind::Sequence, "Pickup Branch",      _bb.id_root);
    _bb.id_pickup   = _panel.AddNode(btedit::EBtKind::Action,   "Pickup",             _bb.id_branch_a);
    _bb.id_move     = _panel.AddNode(btedit::EBtKind::Action,   "Move",               _bb.id_branch_a);
    _bb.id_branch_b = _panel.AddNode(btedit::EBtKind::Sequence, "Combat Branch",      _bb.id_root);
    _bb.id_wait     = _panel.AddNode(btedit::EBtKind::Action,   "Wait",               _bb.id_branch_b);
    _bb.id_attack   = _panel.AddNode(btedit::EBtKind::Action,   "Attack",             _bb.id_branch_b);

    // ---- 実 BT 構築 (= 上で panel に教えた構造と同形に組む) -----
    {
        auto root  = MakeUnique<BtSelector>();
        auto seq_a = MakeUnique<BtSequence>();
        seq_a->AddChild(MakeUnique<BtAction>(&ActionPickup));
        seq_a->AddChild(MakeUnique<BtAction>(&ActionMove));
        root->AddChild(Move(seq_a));

        auto seq_b = MakeUnique<BtSequence>();
        seq_b->AddChild(MakeUnique<BtAction>(&ActionWait));
        seq_b->AddChild(MakeUnique<BtAction>(&ActionAttack));
        root->AddChild(Move(seq_b));

        _bt.SetRoot(Move(root));
    }

    // ---- panel に観察対象 BT + callback を登録 -----
    _bb.panel = &_panel;
    _panel.SetTree(&_bt);
    _panel.SetOnStepCallback(&StepCallbackFn, &_bb);

    // ---- 起動時 selection を root に向けておく (UX: 最初から Inspector に
    //      情報が見えるように) -----
    _panel.SelectNode(_bb.id_root);

    // ---- 起動時は Continuous (autorun) ON にして、すぐにアニメーションを見せる ----
    _panel.SetAutorun(true);

    ACS_LOG_INFO("[BtEditor] entered (BT: Selector{Seq{Pickup,Move},Seq{Wait,Attack}})");
}

void BtEditorScene::OnExit() noexcept {
    // 順序: callback 解除 → SetTree(nullptr) → Shutdown。
    _panel.SetOnStepCallback(nullptr, nullptr);
    _panel.SetTree(nullptr);
    _panel.Shutdown();

    ACS_LOG_INFO("[BtEditor] exited");
}

void BtEditorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt スパイク防御 (= 大 dt で 1 frame で Move/Wait が一気に終わるのを防ぐ)。
    if (dt > 0.1f) dt = 0.1f;

    // panel.OnFrameBegin を呼ぶ (= autorun ON なら 1 tick 進む)。
    // Workspace 統合していない構成では sample 側で明示的に呼ぶ責務がある。
    // ImGui::* を触らない hook なので OnUpdate 内で OK。
    _panel.OnFrameBegin(dt);
}

void BtEditorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- (1) MainMenuBar: File > Reset Tree / Quit ----
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Reset Tree", "R")) {
                // bb 進捗カウンタを 0 に戻し、panel 側も Reset (= step counter /
                // history / 全 status を初期化)。メタミラーと autorun は維持。
                _bb.counter_move = 0;
                _bb.counter_wait = 0;
                _panel.Reset();
                ACS_LOG_INFO("[BtEditor] Reset Tree");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ---- (2) BehaviorTreeEditorPanel 本体 ----
    // Workspace 未統合の最小構成なので、sample 側で直接 DrawUI を呼ぶ。
    _panel.DrawUI();
}

} // namespace hellobt
