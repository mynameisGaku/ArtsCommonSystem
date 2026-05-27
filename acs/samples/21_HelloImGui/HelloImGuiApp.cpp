// SPDX-License-Identifier: Apache-2.0
// HelloImGui — FApplication 派生クラス実装。
#include "HelloImGuiApp.h"

#include "platform/Input.h"

#include <imgui.h>

using namespace acs;

namespace helloimgui {

void HelloImGuiApp::OnStart() noexcept {
    auto r = m_Imgui.Init(GetWindow(), GetRenderer());
    if (r.IsErr()) {
        Quit();
        return;
    }
}

void HelloImGuiApp::OnUpdate(f32 /*dt*/) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
}

void HelloImGuiApp::OnRender() noexcept {
    // NewFrame は OnRender 内 (BeginFrame の後) で呼ぶのが ACS の作法。
    m_Imgui.NewFrame();

    // ShowDemoWindow は ImGui 機能網羅のリファレンス。
    if (m_ShowDemo) ImGui::ShowDemoWindow(&m_ShowDemo);

    ImGui::Begin("ACS FSample");
    ImGui::Text("FPS: %.1f", FPS());
    ImGui::Text("Frames: %llu", static_cast<unsigned long long>(FrameCount()));
    ImGui::Checkbox("Show ImGui demo", &m_ShowDemo);
    ImGui::SliderFloat("R", &m_R, 0.0f, 1.0f);
    ImGui::SliderFloat("G", &m_G, 0.0f, 1.0f);
    ImGui::SliderFloat("B", &m_B, 0.0f, 1.0f);
    if (ImGui::Button("Quit")) Quit();
    ImGui::End();

    // Render は ImGui の draw list をコマンドリストに発行する (BeginFrame との対)。
    m_Imgui.Render();
}

void HelloImGuiApp::OnShutdown() noexcept {
    m_Imgui.Shutdown();
}

void HelloImGuiApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
}

} // namespace helloimgui
