// SPDX-License-Identifier: Apache-2.0
// HelloImGui — HelloImGuiApp 実装。
#include "HelloImGuiApp.h"

#include "platform/Input.h"

#include <imgui.h>

using namespace acs;

namespace helloimgui {

void HelloImGuiApp::OnStart() noexcept {
    // ImGui を初期化 (Window と Renderer に紐付け)
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
    // ImGui の新フレーム開始 (OnRender は BeginFrame の後に呼ばれる)
    _imgui.NewFrame();

    // デモウィンドウ (ImGui の機能網羅サンプル)
    if (_show_demo) ImGui::ShowDemoWindow(&_show_demo);

    // 自前のコントロールウィンドウ
    ImGui::Begin("ACS Sample");
    ImGui::Text("FPS: %.1f", FPS());
    ImGui::Text("Frames: %llu", static_cast<unsigned long long>(FrameCount()));
    ImGui::Checkbox("Show ImGui demo", &_show_demo);
    ImGui::SliderFloat("R", &_r, 0.0f, 1.0f);
    ImGui::SliderFloat("G", &_g, 0.0f, 1.0f);
    ImGui::SliderFloat("B", &_b, 0.0f, 1.0f);
    if (ImGui::Button("Quit")) Quit();
    ImGui::End();

    // ImGui の描画コマンドをコマンドリストに発行
    _imgui.Render();
}

void HelloImGuiApp::OnShutdown() noexcept {
    _imgui.Shutdown();
}

void HelloImGuiApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
}

} // namespace helloimgui
