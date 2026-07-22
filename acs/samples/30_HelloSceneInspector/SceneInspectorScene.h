// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — ANode ツリー + Inspector 系 panel 群を持つ Scene。
//
// 構造: root → wheel (回転) → spoke[0/1]、と root → player (Provider) の
// 2 系統。FHierarchyPanel でツリーを編集、FInspectorPanel で player の field
// を編集する。panel/seam/selection の配線は FPanelLayout に委譲。
#pragma once

#include "gameframework/GameFramework.h"
#include "PanelLayout.h"

namespace helloscene {

class APlayerNode;
class AWheelNode;

class FSceneInspectorScene : public acs::game::FScene {
public:
    void OnEnter()             noexcept override;
    void OnExit()              noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    // ツリー組立を OnEnter から切り出した小ヘルパ。
    void m_BuildNodeTree() noexcept;

    // File メニュー stub の保存先 (現状 callback だけ走らせる)。
    static constexpr const char* kScenePath = "scene.acscene";

    // ANode ツリーのルート。ChildCount() == 2 (wheel, player) になる。
    acs::game::ANode m_RootNode;

    // 弱参照: OnExit 以外で Destroy を呼ばないので OnUpdate 中は安全。
    APlayerNode*        m_Player    = nullptr;
    AWheelNode*         m_Wheel     = nullptr;
    acs::game::ANode* m_Spoke[2]  = { nullptr, nullptr };

    // 4 panel + Selection + Seam の束ね。
    FPanelLayout m_Panels;
};

} // namespace helloscene
