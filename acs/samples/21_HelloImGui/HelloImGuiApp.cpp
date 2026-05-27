// SPDX-License-Identifier: Apache-2.0
// HelloImGui — FApplication 派生クラス実装。
#include "HelloImGuiApp.h"

#include "platform/Input.h"

#include <imgui.h>

using namespace acs;

namespace helloimgui {

void HelloImGuiApp::OnStart() noexcept {
    auto r = _imgui.Init(GetWindow(), GetRenderer());
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
    _imgui.NewFrame();

    // ShowDemoWindow は ImGui 機能網羅のリファレンス。
    if (_show_demo) ImGui::ShowDemoWindow(&_show_demo);

    ImGui::Begin("ACS FSample");
    ImGui::Text("FPS: %.1f", FPS());
    ImGui::Text("Frames: %llu", static_cast<unsigned long long>(FrameCount()));
    ImGui::Checkbox("Show ImGui demo", &_show_demo);
    ImGui::SliderFloat("R", &_r, 0.0f, 1.0f);
    ImGui::SliderFloat("G", &_g, 0.0f, 1.0f);
    ImGui::SliderFloat("B", &_b, 0.0f, 1.0f);
    if (ImGui::Button("Quit")) Quit();
    ImGui::End();

    // Render は ImGui の draw list をコマンドリストに発行する (BeginFrame との対)。
    _imgui.Render();
}

void HelloImGuiApp::OnShutdown() noexcept {
    _imgui.Shutdown();
}

void HelloImGuiApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
}

} // namespace helloimgui
