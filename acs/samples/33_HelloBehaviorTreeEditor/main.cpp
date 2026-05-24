// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — GameFramework Phase 22: BehaviorTree Editor デモ
//
// 動作:
//   ・小さな Behavior Tree を構築:
//        Selector (root)
//        ├── Sequence "Pickup Branch"
//        │    ├── Action "Pickup"
//        │    └── Action "Move"
//        └── Sequence "Combat Branch"
//             ├── Action "Wait"
//             └── Action "Attack"
//   ・BT そのものは `acs::game::BehaviorTree` で実行。各 BtAction の Fn は
//     blackboard 経由で `BehaviorTreeEditorPanel` の SetNodeStatus を呼び、
//     panel に「この frame でこの node が何を返したか」を push する。
//     これで panel 側のメタミラーがリアルタイムに着色される。
//   ・panel API (AddNode で構造を panel に教える、SetTree で観察対象を渡す、
//     SetOnStepCallback で blackboard 渡し付き Tick を委譲) の典型用途を一通り
//     見せる sample。
//   ・MainMenuBar: File > Reset Tree / Quit。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (= samples/21/29/30/31 と同じ理由で
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・blackboard は本 sample 専用の struct (= panel ポインタ + per-action 進捗
//     カウンタ) を作って `BehaviorTree::Tick(&bb, dt)` に渡す。Action Fn の中で
//     `auto* bb = static_cast<MyBb*>(blackboard);` で取り戻し、bb->panel->
//     SetNodeStatus(bb->action_id, ret) を呼ぶ。
//     "panel API" だけで完結する構成 (= BT 本体側に panel 連携の knowledge を
//     入れない) を見せる sample なので、Fn を 4 個書く形にする。
//   ・Action 関数は各々 N 回 Running を返したら Success / Failure に遷移する。
//     "Pickup" : 1 frame で Success、"Move" : 30 frame Running → Success、
//     "Wait"   : 30 frame Running → Failure、"Attack": 1 frame で Success。
//     こうすると Selector の挙動 (sequence 切替) が history graph 上で見える。
//   ・Reset Tree メニューで bb 進捗カウンタと panel.Reset() を同期させる。
//     panel.ClearNodes() は呼ばない (= 同じ構造を再観察したいため)。
//   ・ImGuiCtx は **Game クラスが所有**。OnStart/OnShutdown/OnEvent で
//     lifecycle を回し、OnRender を override して NewFrame() → Scene OnRender
//     → Render() の順で挟む (= sample 29/30/31 と同形)。
//   ・Workspace 統合はせず、panel.DrawUI() を Scene::OnRender から直接呼ぶ
//     最小構成 (= sample 22 = Phase 22 デモはエディタ単体に集中)。
//     Workspace 登録パターンは sample 31 で示済。

#include "gameframework/GameFramework.h"
#include "gameframework/BehaviorTree.h"
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"

#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "memory/UniquePtr.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// Blackboard — Action 関数から panel への双方向参照 + 進捗カウンタを持つ
// ============================================================================
// BehaviorTree::Tick(&bb, dt) で渡され、各 Action Fn の中で
// `static_cast<BtEditorBb*>(blackboard)` で復元される。Fn 1 つにつき 1 個の
// 進捗カウンタを持ち、N frame Running した後に Success/Failure に遷移する
// 「タイマ式 stub action」として動作させる。
// ----------------------------------------------------------------------------
struct BtEditorBb {
    btedit::BehaviorTreeEditorPanel* panel = nullptr;

    // Action 関数が panel.SetNodeStatus に渡す node_id (sample が AddNode で
    // 払い出した値をそのまま保持)。
    u32 id_pickup = btedit::BehaviorTreeEditorPanel::kInvalidId;
    u32 id_move   = btedit::BehaviorTreeEditorPanel::kInvalidId;
    u32 id_wait   = btedit::BehaviorTreeEditorPanel::kInvalidId;
    u32 id_attack = btedit::BehaviorTreeEditorPanel::kInvalidId;

    // root / composite の id (= Selector / Sequence)。panel 側で root の status
    // を履歴に push するため、composite の status も Fn から push しないと
    // composite 自体は灰色 (Failure 初期値) のままになる。
    // 各 Action から composite の id を辿るのは BehaviorTree の API では出来ない
    // ので、Bb に持たせる + Action Fn の最後に "親 composite の status も上書き"
    // する戦略にする (= sample 側のアドホック合成)。
    u32 id_root      = btedit::BehaviorTreeEditorPanel::kInvalidId; // Selector
    u32 id_branch_a  = btedit::BehaviorTreeEditorPanel::kInvalidId; // Sequence "Pickup Branch"
    u32 id_branch_b  = btedit::BehaviorTreeEditorPanel::kInvalidId; // Sequence "Combat Branch"

    // 進捗カウンタ (Running 中の経過 frame 数)。Action 毎に独立。
    // Reset Tree メニュー or panel.Reset() に合わせて 0 に戻す。
    u32 counter_move   = 0;
    u32 counter_wait   = 0;

    // タイマ閾値 (frame 単位、kStepDt * 30 ≈ 0.48 sec)。
    static constexpr u32 kMoveRunFrames = 30u;
    static constexpr u32 kWaitRunFrames = 30u;
};

// ----------------------------------------------------------------------------
// Action Fn 群 — 全て BtAction::Fn (= EBtStatus(*)(void*, f32) noexcept) シグネチャ
// ----------------------------------------------------------------------------
// blackboard を BtEditorBb* にキャストし、panel.SetNodeStatus で status を push。
// 各 Fn は「N frame Running した後 Success/Failure に切替」のタイマ式。

// "Pickup" — 1 frame で Success (= 拾うアクションは即時完了)。
static EBtStatus ActionPickup(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    const EBtStatus s = EBtStatus::Success;
    if (bb && bb->panel) bb->panel->SetNodeStatus(bb->id_pickup, s);
    return s;
}

// "Move" — kMoveRunFrames frame Running し続けてから Success に遷移。
//   pickup の後に「30 frame かけて拾った先まで歩く」イメージ。
static EBtStatus ActionMove(void* blackboard, f32 /*dt*/) noexcept {
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
static EBtStatus ActionWait(void* blackboard, f32 /*dt*/) noexcept {
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
static EBtStatus ActionAttack(void* blackboard, f32 /*dt*/) noexcept {
    auto* bb = static_cast<BtEditorBb*>(blackboard);
    const EBtStatus s = EBtStatus::Success;
    if (bb && bb->panel) bb->panel->SetNodeStatus(bb->id_attack, s);
    return s;
}

// ----------------------------------------------------------------------------
// step callback — panel.SetOnStepCallback で登録、blackboard を渡して Tick
// ----------------------------------------------------------------------------
static void StepCallbackFn(void* user, BehaviorTree* tree, f32 dt) noexcept {
    auto* bb = static_cast<BtEditorBb*>(user);
    if (!bb || !tree) return;
    tree->Tick(bb, dt);
}

// ============================================================================
// BtEditorScene — BT 構築 + panel 構成 + render
// ============================================================================
class BtEditorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    BehaviorTree                       _bt;
    BtEditorBb                         _bb;
    btedit::BehaviorTreeEditorPanel    _panel;
};

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

// ============================================================================
// BtEditorGame — ImGui lifecycle を Game に持たせる薄いラッパ (sample 29/30/31 と同形)
// ============================================================================
class BtEditorGame : public Game {
public:
    void OnStart() noexcept override {
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[BtEditorGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        Game::OnStart();
    }

    void OnRender() noexcept override {
        _imgui.NewFrame();
        Game::OnRender();
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        Game::OnShutdown();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
        Game::OnEvent(e);
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<BtEditorScene>();
    }

private:
    ImGuiCtx _imgui;
};

#if defined(ACS_RENDER_DX12_RAW)
ACS_GAME_MAIN(BtEditorGame)
#else
// ACS_RENDER_DX12_RAW 未定義時はビルドガード (= ImGuiCtx が dx12 raw 専用)。
// sample 21/29/30/31 と同じ理由で本 sample も DX12 raw 限定。
// CMakeLists.txt でも `if(ACS_RENDER_DX12_RAW)` ガードを掛けている。
int main() { return 0; }
#endif
