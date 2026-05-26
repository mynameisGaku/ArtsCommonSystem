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
    // 背景色 (editor らしいニュートラルグレー)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Node2D ツリー組立: root → wheel → spoke[0/1] + root → player ----
    // wheel: 30 deg/s で回転
    auto wheel_up = MakeUnique<WheelNode>(0.5f /*rad/s*/);
    wheel_up->Local().position = Vec2{ 4.0f, 0.0f };
    wheel_up->_SetId(NodeId{ 2u, 1u });
    Node2D& wheel_ref = _root_node.AddChild(Move(wheel_up));
    _wheel = static_cast<WheelNode*>(&wheel_ref);

    // spoke[0]
    auto sp0_up = MakeUnique<Node2D>();
    sp0_up->Local().position = Vec2{ 2.0f, 0.0f };
    sp0_up->_SetId(NodeId{ 3u, 1u });
    _spoke[0] = &wheel_ref.AddChild(Move(sp0_up));

    // spoke[1]
    auto sp1_up = MakeUnique<Node2D>();
    sp1_up->Local().position = Vec2{ 0.0f, 2.0f };
    sp1_up->_SetId(NodeId{ 4u, 1u });
    _spoke[1] = &wheel_ref.AddChild(Move(sp1_up));

    // player (Inspector 編集対象、root の直下、wheel と兄弟)
    auto player_up = MakeUnique<PlayerNode>();
    player_up->Local().position = Vec2{ -4.0f, 0.0f };
    // NodeId{1, 1}: index=1 (root が将来 0 に振られる想定)、generation=1。
    player_up->_SetId(NodeId{ 1u, 1u });
    Node2D& player_ref = _root_node.AddChild(Move(player_up));
    _player = static_cast<PlayerNode*>(&player_ref);
    // root 自体にも id を割り振っておく (Hierarchy の根クリック対応)。
    _root_node._SetId(NodeId{ 0u, 1u });

    // ---- InspectorSeam に Player Provider を登録 ----
    // 本サンプルでは Player のみ Provider を実装。他ノードは Inspector で
    // "(No provider)" 表示になる契約。
    _seam.Init();
    _seam.RegisterProvider(_player);

    // ---- SelectionService 初期化 + 4 panel への注入 ----
    _selection.Init();
    _hierarchy_panel.Init();
    _hierarchy_panel.SetSelectionService(&_selection);
    _inspector_panel.Init();
    _inspector_panel.SetSelectionService(&_selection);

    // ---- Toolbar 初期化 ----
    // EditorToolbar の最小契約: `Init()` で内部状態を default に戻す。
    // Play/Pause/Step トグルは内部で持つ想定。
    _toolbar.Init();

    // 起動時の selection を Player に向けておく (UX: 最初から Inspector に
    // 値が見えるようにする)。
    _selection.SelectNode(_player->Id());

    ACS_LOG_INFO("[SceneInspector] entered (root + wheel + 2 spokes + player, selection=Player)");
}

void SceneInspectorScene::OnExit() noexcept {
    // 4 panel + service + seam を逆順に shutdown / clear。Provider 自体は
    // PlayerNode が握っているので、seam 側は登録解除だけ行う (= ClearAll)。
    _toolbar.Shutdown();
    _inspector_panel.Shutdown();
    _hierarchy_panel.Shutdown();
    _selection.ClearAll();
    _seam.ClearAll();

    // ツリーを破棄 (Phase 5 標準パターン)。
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

    // dt の安全 clamp (大 dt スパイクで wheel 回転が暴れない)。
    if (dt > 0.1f) dt = 0.1f;

    // ツリー全体に dt を伝播 (wheel が回り、spoke の World() が変化する)。
    _root_node.UpdateTree(dt);
    _root_node.ResolveStructuralChanges();

    // 現選択ノードを掴む (= sample 仕様 §5)。SelectionService が source of truth。
    const NodeId selected_id = _selection.CurrentSelection();
    // (現状 selected_id は Hierarchy → SelectionService → Inspector で使う。
    // OnUpdate 内では log 出力など以外には未使用なので参照だけ。)
    (void)selected_id;
}

void SceneInspectorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- main menu bar: File > Save / Load (stub) ----
    // ImGui の draw コマンドは Game::OnRender の NewFrame() と Render() の間で
    // 発行される。Scene::OnRender はその内側なのでそのまま ImGui::* を呼べる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Scene")) {
                // Phase 20: serializer は未実装。callback hook だけ走らせる stub。
                // 将来は `SceneSerializer::Save(kScenePath, _root_node)` を呼ぶ。
                ACS_LOG_INFO("[SceneInspector] Save Scene -> '%s' (stub, no-op)", kScenePath);
            }
            if (ImGui::MenuItem("Load Scene")) {
                // Phase 20: serializer は未実装。callback hook だけ走らせる stub。
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

    // ---- Editor Toolbar: Play / Pause / Step / 等の global コマンド ----
    // Phase 24: EditorPanel 基底に乗ったため SetGame + no-arg DrawUI に変更。
    _toolbar.SetGame(&GetGame());
    _toolbar.DrawUI();

    // ---- Hierarchy: Node2D ツリー描画 + 選択操作 ----
    // Phase 24: SetRootNode + no-arg DrawUI。
    _hierarchy_panel.SetRootNode(&_root_node);
    _hierarchy_panel.DrawUI();

    // ---- Inspector: 選択 Node の field を Provider 経由で編集 ----
    // Phase 24: SetInspectorSeam + no-arg DrawUI、selection は
    // SelectionService 経由 (SetSelectionService 済) で取得される。
    _inspector_panel.SetInspectorSeam(&_seam);
    _inspector_panel.DrawUI();
}

} // namespace helloscene
