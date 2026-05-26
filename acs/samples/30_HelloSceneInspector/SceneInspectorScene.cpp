// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — SceneInspectorScene 実装。
#include "SceneInspectorScene.h"
#include "SceneNodes.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace helloscene {

void SceneInspectorScene::OnEnter() noexcept {
    // 背景色は editor らしいニュートラルグレー。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    _build_node_tree();

    _panels.Init();
    // 本サンプルでは Player のみ Provider を実装。他ノードは Inspector で
    // "(No provider)" 表示になる契約。
    _panels.RegisterProvider(_player);
    // 起動時から Inspector に値が見えるよう、最初の selection を Player に。
    _panels.SelectInitial(_player->Id());

    ACS_LOG_INFO("[SceneInspector] entered (root + wheel + 2 spokes + player, selection=Player)");
}

void SceneInspectorScene::_build_node_tree() noexcept {
    // root → wheel (30 deg/s で自転) → spoke[0/1]
    //      → player (Inspector 編集対象、wheel と兄弟)
    auto wheel_up = MakeUnique<WheelNode>(0.5f /*rad/s*/);
    wheel_up->Local().position = FVec2{ 4.0f, 0.0f };
    wheel_up->_SetId(NodeId{ 2u, 1u });
    Node2D& wheel_ref = _root_node.AddChild(Move(wheel_up));
    _wheel = static_cast<WheelNode*>(&wheel_ref);

    auto sp0_up = MakeUnique<Node2D>();
    sp0_up->Local().position = FVec2{ 2.0f, 0.0f };
    sp0_up->_SetId(NodeId{ 3u, 1u });
    _spoke[0] = &wheel_ref.AddChild(Move(sp0_up));

    auto sp1_up = MakeUnique<Node2D>();
    sp1_up->Local().position = FVec2{ 0.0f, 2.0f };
    sp1_up->_SetId(NodeId{ 4u, 1u });
    _spoke[1] = &wheel_ref.AddChild(Move(sp1_up));

    auto player_up = MakeUnique<PlayerNode>();
    player_up->Local().position = FVec2{ -4.0f, 0.0f };
    // NodeId{1, 1}: index=1 (root が将来 0 に振られる想定)、generation=1。
    player_up->_SetId(NodeId{ 1u, 1u });
    Node2D& player_ref = _root_node.AddChild(Move(player_up));
    _player = static_cast<PlayerNode*>(&player_ref);

    // root 自体にも id を振る (Hierarchy が根クリックに反応できるように)。
    _root_node._SetId(NodeId{ 0u, 1u });
}

void SceneInspectorScene::OnExit() noexcept {
    _panels.Shutdown();

    // ツリーを破棄。
    for (u32 i = 0; i < _root_node.ChildCount(); ++i) {
        if (auto* c = _root_node.Child(i)) c->Destroy();
    }
    _root_node.ResolveStructuralChanges();

    ACS_LOG_INFO("[SceneInspector] exited");
}

void SceneInspectorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // 大 dt スパイクで wheel 回転が暴れないよう safety clamp。
    if (dt > 0.1f) dt = 0.1f;

    _root_node.UpdateTree(dt);
    _root_node.ResolveStructuralChanges();
}

void SceneInspectorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ImGui の draw コマンドは Game::OnRender の NewFrame() と Render() の
    // 間で発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を
    // 呼んで OK。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            // Save/Load は callback hook だけ走らせる stub。将来は
            // `SceneSerializer::Save(kScenePath, _root_node)` 等になる想定。
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

    _panels.DrawUI(GetGame(), _root_node);
}

} // namespace helloscene
