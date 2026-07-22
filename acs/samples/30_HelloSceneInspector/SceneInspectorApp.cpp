// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — FSceneInspectorApp 実装。
#include "SceneInspectorApp.h"
#include "SceneInspectorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloscene {

void FSceneInspectorApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[FSceneInspectorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void FSceneInspectorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender 内で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。この順番でないと
    // Scene 側が ImGui::* を呼べない。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void FSceneInspectorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void FSceneInspectorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> FSceneInspectorApp::InitialScene() noexcept {
    return MakeUnique<FSceneInspectorScene>();
}

} // namespace helloscene
