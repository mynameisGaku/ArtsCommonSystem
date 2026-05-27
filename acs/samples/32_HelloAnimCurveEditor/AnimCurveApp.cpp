// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — AnimCurveApp 実装。
#include "AnimCurveApp.h"
#include "AnimCurveScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloac {

void AnimCurveApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        // ImGui 初期化失敗時は Scene を回しても何も描けないので早期 Quit。
        ACS_LOG_ERROR("[AnimCurveApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart が InitialScene() を push する。
    FGame::OnStart();
}

void AnimCurveApp::OnRender() noexcept {
    // Scene::OnRender 内の ImGui::* を NewFrame と Render の間に挟む。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void AnimCurveApp::OnShutdown() noexcept {
    // Scene が ImGui::* を握っていない状態にしてから ImGui を落とす。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void AnimCurveApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> AnimCurveApp::InitialScene() noexcept {
    return MakeUnique<AnimCurveScene>();
}

} // namespace helloac
