// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — Scene 上に Node2D ツリー + 4 panel + InspectorSeam を持つ
// シーン。HierarchyPanel / InspectorPanel / EditorToolbar / SelectionService を
// 連結し、ImGui で edit する。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/InspectorSeam.h"
#include "gameframework/tools/inspector/HierarchyPanel.h"
#include "gameframework/tools/inspector/InspectorPanel.h"
#include "gameframework/tools/inspector/EditorToolbar.h"
#include "gameframework/tools/inspector/SelectionService.h"

namespace helloscene {

class PlayerNode;
class WheelNode;

class SceneInspectorScene : public acs::game::Scene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::RenderContext& rc) noexcept override;

private:
    // File メニュー stub の保存先 (現状 callback だけ走らせるため未使用)。
    static constexpr const char* kScenePath = "scene.acscene";

    // Node2D ツリーのルート。ChildCount() == 2 (wheel, player) になる。
    acs::game::Node2D            _root_node;
    // 弱参照: ResolveStructuralChanges 後に dangling になり得るが、本サンプルでは
    // OnExit 以外で Destroy を呼ばないので OnUpdate 中は安全。
    PlayerNode*                  _player      = nullptr;
    WheelNode*                   _wheel       = nullptr;
    acs::game::Node2D*           _spoke[2]    = { nullptr, nullptr };

    // Editor panels (4 つ) + Inspector seam + Selection service。
    acs::game::inspector::HierarchyPanel    _hierarchy_panel;
    acs::game::inspector::InspectorPanel    _inspector_panel;
    acs::game::inspector::EditorToolbar     _toolbar;
    acs::game::inspector::SelectionService  _selection;
    acs::game::InspectorSeam                _seam;
};

} // namespace helloscene
