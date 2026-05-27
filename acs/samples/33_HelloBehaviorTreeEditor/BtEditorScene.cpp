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

void BtEditorScene::OnEnter() noexcept {
    // editor らしいニュートラルグレー (background は ImGui がほぼ覆う)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // panel 初期化 (順序: Init → AddNode 構造登録 → SetTree → SetCallback)。
    m_Panel.Init();

    // panel に「メタミラー node 構造」を登録、続けて同形の実 BT を組む。
    // この 2 つは 1:1 対応で組むことで、Action Fn からの panel.SetNodeStatus
    // 呼び出しが正しい node に着地する。
    BuildPanelMirror(m_Panel, m_Bb);
    BuildBehaviorTree(m_Bt);

    // panel に観察対象 BT + callback を登録。
    m_Bb.panel = &m_Panel;
    m_Panel.SetTree(&m_Bt);
    m_Panel.SetOnStepCallback(&StepCallbackFn, &m_Bb);

    // 起動時 selection を root に向けておく
    // (UX: 最初から Inspector に情報が見えるように)。
    m_Panel.SelectNode(m_Bb.id_root);

    // 起動時は Continuous (autorun) ON にして、すぐにアニメーションを見せる。
    m_Panel.SetAutorun(true);

    ACS_LOG_INFO("[BtEditor] entered (BT: Selector{Seq{Pickup,Move},Seq{Wait,Attack}})");
}

void BtEditorScene::OnExit() noexcept {
    // 順序: callback 解除 → SetTree(nullptr) → Shutdown。
    m_Panel.SetOnStepCallback(nullptr, nullptr);
    m_Panel.SetTree(nullptr);
    m_Panel.Shutdown();

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
    m_Panel.OnFrameBegin(dt);
}

void BtEditorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // MainMenuBar: File > Reset Tree / Quit
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Reset Tree", "R")) {
                // bb 進捗カウンタを 0 に戻し、panel 側も Reset (= step counter /
                // history / 全 status を初期化)。メタミラーと autorun は維持。
                m_Bb.counter_move = 0;
                m_Bb.counter_wait = 0;
                m_Panel.Reset();
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

    // FBehaviorTreeEditorPanel 本体。
    // Workspace 未統合の最小構成なので、sample 側で直接 DrawUI を呼ぶ。
    m_Panel.DrawUI();
}

} // namespace hellobt
