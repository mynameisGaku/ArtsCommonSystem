// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — FSceneInspectorScene 実装。
#include "SceneInspectorScene.h"
#include "SceneNodes.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace helloscene {

void FSceneInspectorScene::OnEnter() noexcept {
    // 背景色は editor らしいニュートラルグレー。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    m_BuildNodeTree();

    m_Panels.Init();
    // 本サンプルでは Player のみ Provider を実装。他ノードは Inspector で
    // "(No provider)" 表示になる契約。Player の FNodeId をキーに登録し、
    // Inspector が選択 FNodeId から node-keyed で逆引きできるようにする。
    m_Panels.RegisterProvider(m_Player->Id(), m_Player);
    // 起動時から Inspector に値が見えるよう、最初の selection を Player に。
    m_Panels.SelectInitial(m_Player->Id());

    ACS_LOG_INFO("[SceneInspector] entered (root + wheel + 2 spokes + player, selection=Player)");
}

void FSceneInspectorScene::m_BuildNodeTree() noexcept {
    // root → wheel (30 deg/s で自転) → spoke[0/1]
    //      → player (Inspector 編集対象、wheel と兄弟)
    auto wheel_up = NewObject<AWheelNode>(0.5f /*rad/s*/);
    wheel_up->SetPosition2D(FVec2{ 4.0f, 0.0f });
    wheel_up->_SetId(FNodeId{ 2u, 1u });
    ANode& wheel_ref = m_RootNode.AddChild(Move(wheel_up));
    m_Wheel = static_cast<AWheelNode*>(&wheel_ref);

    auto sp0_up = NewObject<ANode>();
    sp0_up->SetPosition2D(FVec2{ 2.0f, 0.0f });
    sp0_up->_SetId(FNodeId{ 3u, 1u });
    m_Spoke[0] = &wheel_ref.AddChild(Move(sp0_up));

    auto sp1_up = NewObject<ANode>();
    sp1_up->SetPosition2D(FVec2{ 0.0f, 2.0f });
    sp1_up->_SetId(FNodeId{ 4u, 1u });
    m_Spoke[1] = &wheel_ref.AddChild(Move(sp1_up));

    auto player_up = NewObject<APlayerNode>();
    player_up->SetPosition2D(FVec2{ -4.0f, 0.0f });
    // FNodeId{1, 1}: index=1 (root が将来 0 に振られる想定)、generation=1。
    player_up->_SetId(FNodeId{ 1u, 1u });
    ANode& player_ref = m_RootNode.AddChild(Move(player_up));
    m_Player = static_cast<APlayerNode*>(&player_ref);

    // root 自体にも id を振る (Hierarchy が根クリックに反応できるように)。
    m_RootNode._SetId(FNodeId{ 0u, 1u });
}

void FSceneInspectorScene::OnExit() noexcept {
    m_Panels.Shutdown();

    // ツリーを破棄。
    for (u32 i = 0; i < m_RootNode.ChildCount(); ++i) {
        if (auto* c = m_RootNode.Child(i)) c->Destroy();
    }
    m_RootNode.ResolveStructuralChanges();

    ACS_LOG_INFO("[SceneInspector] exited");
}

void FSceneInspectorScene::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // 大 dt スパイクで wheel 回転が暴れないよう safety clamp。
    if (dt > 0.1f) dt = 0.1f;

    m_RootNode.UpdateTree(dt);
    m_RootNode.ResolveStructuralChanges();
}

void FSceneInspectorScene::OnRender(FRenderContext& /*rc*/) noexcept {
    // ImGui の draw コマンドは FGame::OnRender の NewFrame() と Render() の
    // 間で発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を
    // 呼んで OK。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Save/Load は callback hook だけ走らせる stub。将来は
            // `SceneSerializer::Save(kScenePath, m_RootNode)` 等になる想定。
            if (ImGui::MenuItem("Save Scene")) {
                ACS_LOG_INFO("[SceneInspector] Save Scene -> '%s' (stub, no-op)", kScenePath);
            }
            if (ImGui::MenuItem("Load Scene")) {
                ACS_LOG_INFO("[SceneInspector] Load Scene <- '%s' (stub, no-op)", kScenePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    m_Panels.DrawUI(GetGame(), m_RootNode);
}

} // namespace helloscene
