// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — CGame 派生のアプリケーションクラス。
//
// ImGui lifecycle を CGame に持たせる薄いラッパ。OnRender 内で
// NewFrame と Render を Scene::OnRender の両側に挟むのが key
// (= Scene 側が ImGui::* をそのまま呼べるようにする)。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace helloscene {

class CSceneInspectorApp : public acs::game::CGame {
public:
    void OnStart()    noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::AScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
};

} // namespace helloscene
