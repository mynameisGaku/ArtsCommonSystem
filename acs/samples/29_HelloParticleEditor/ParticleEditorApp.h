// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — FGame 派生のアプリケーションクラス。
//
// ImGui lifecycle を FGame に持たせる薄いラッパ。OnStart / OnShutdown / OnEvent
// で ImGuiCtx を回し、OnRender を override して NewFrame() → Scene::OnRender →
// Render() の順で挟む。これで Scene 側はそのまま ImGui::* を呼べる。
#pragma once

#include "gameframework/GameFramework.h"
#include "imgui/ImGuiContext.h"

namespace helloparticleed {

class FParticleEditorApp : public acs::game::FGame {
public:
    void OnStart()    noexcept override;
    void OnRender()   noexcept override;
    void OnShutdown() noexcept override;
    void OnEvent(const acs::FEvent& e) noexcept override;

protected:
    acs::TUniquePtr<acs::game::FScene> InitialScene() noexcept override;

private:
    acs::FImGuiCtx m_Imgui;
};

} // namespace helloparticleed
