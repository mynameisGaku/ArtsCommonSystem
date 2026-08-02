// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorScene lifecycle 実装。
//
// 構築 (panel メタミラー + 実 BT) は TreeBuilder に、Action Fn 群と step
// callback は TreeActions に分離。Scene 本体は OnEnter/OnExit/OnUpdate/OnRender
// の流れに集中する。
#include "BtEditorScene.h"
#include "TreeActions.h"
#include "TreeBuilder.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellobt {

void ABtEditorScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (background は ImGui がほぼ覆う)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    m_Panel.Init();

    // (1) 動的ブラックボードに変数を用意する。これらはエディタの Blackboard パネルから
    //     追加 / リネーム / 値編集でき、Compare デコレーターの変数候補にもなる。
    //     ・see_phase : CanSeePlayer が毎 tick 進める位相 (コード駆動)。
    //     ・health    : コードは触らない体力値。エディタで poke して条件を試す。
    m_Bb.Add("see_phase", EBtVarType::I32);
    m_Bb.Add("health",    EBtVarType::F32);
    m_Bb.SetF32("health", 100.0f);

    // (2) no-code 実行を有効化する。Task/Action 名 → 関数、Condition 名 → bool 関数を
    //     registry に登録し、動的ブラックボードと共に panel へ渡す。これで Step /
    //     Continuous は「エディタのグラフを直接インタプリト」して実行し、実行フローが
    //     ライブで光る。Compare デコレーターは動的変数を名前で読んで評価する。
    m_Reg.Register("Pickup", &ActionPickup);
    m_Reg.Register("Move",   &ActionMove);
    m_Reg.Register("Wait",   &ActionWait);
    m_Reg.Register("Attack", &ActionAttack);
    m_Cond.Register("CanSeePlayer", &CanSeePlayer);

    m_Panel.SetActionRegistry(&m_Reg);
    m_Panel.SetConditionRegistry(&m_Cond);
    m_Panel.SetDynamicBlackboard(&m_Bb);   // エディタで変数編集できる動的 BB
    m_Panel.SetGraphBlackboard(&m_Bb);     // Action/Condition 関数に渡す同一インスタンス

    // (3) エディタのグラフ (メタミラー) を構築し、root を選択しておく。
    const u32 root = BuildPanelMirror(m_Panel);
    m_Panel.SelectNode(root);

    // 起動時は Continuous (autorun) ON にして、すぐにアニメーションを見せる。
    m_Panel.SetAutorun(true);

    ACS_LOG_INFO("[BtEditor] entered (graph-run + dynamic blackboard)");
}

void ABtEditorScene::OnExit() noexcept {
    // registry / blackboard を解除してから Shutdown。
    m_Panel.SetActionRegistry(nullptr);
    m_Panel.SetConditionRegistry(nullptr);
    m_Panel.SetDynamicBlackboard(nullptr);
    m_Panel.SetGraphBlackboard(nullptr);
    m_Panel.Shutdown();

    ACS_LOG_INFO("[BtEditor] exited");
}

void ABtEditorScene::OnUpdate(f32 dt) noexcept {
    if (CInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt スパイク防御 (= 大 dt で 1 frame で Move/Wait が一気に終わるのを防ぐ)。
    if (dt > 0.1f) dt = 0.1f;

    // panel.OnFrameBegin を呼ぶ (= autorun ON なら 1 tick 進む)。
    // Workspace 統合していない構成では sample 側で明示的に呼ぶ責務がある。
    // ImGui::* を触らない hook なので OnUpdate 内で OK。
    m_Panel.OnFrameBegin(dt);
}

void ABtEditorScene::OnRender(FRenderContext& /*rc*/) noexcept {
    // MainMenuBar: File > Reset Tree / Quit
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Reset Tree", "R")) {
                // 動的変数を初期値へ、panel 側も Reset (= step counter / history /
                // 全 status を初期化)。メタミラーと autorun は維持。
                m_Bb.SetI32("see_phase", 0);
                m_Bb.SetF32("health", 100.0f);
                m_Panel.Reset();
                ACS_LOG_INFO("[BtEditor] Reset Tree");
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        // Window メニュー: パネルの × で閉じた後に再表示するための導線。
        // (panel.DrawUI は IsVisible() false で何も描かないので、ここで戻せないと
        //  一度閉じたパネルが二度と出せなくなる = 「× が効かない」体験になる。)
        if (ImGui::BeginMenu("Window")) {
            bool vis = m_Panel.IsVisible();
            if (ImGui::MenuItem("Behavior Tree Editor", nullptr, &vis)) {
                m_Panel.SetVisible(vis);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ABehaviorTreeEditorPanel 本体。
    // Workspace 未統合の最小構成なので、sample 側で直接 DrawUI を呼ぶ。
    m_Panel.DrawUI();
}

} // namespace hellobt
