// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — FGame 派生のアプリケーションクラス。
//
// ImGui lifecycle を FGame に持たせる薄いラッパ。OnRender 内で
// NewFrame と Render を Scene::OnRender の両側に挟むのが key
// (= Scene 側が ImGui::* をそのまま呼べるようにする)。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace helloscene {

class SceneInspectorApp : public acs::game::FGame {
public:
    void OnStart()    noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::Event& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::Scene> InitialScene() noexcept override;

private:
    acs::ImGuiCtx m_Imgui;
};

} // namespace helloscene
