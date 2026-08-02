// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — CSceneInspectorApp 実装。
#include "SceneInspectorApp.h"
#include "SceneInspectorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloscene {

void CSceneInspectorApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[CSceneInspectorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    CGame::OnStart();
}

void CSceneInspectorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender 内で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。この順番でないと
    // Scene 側が ImGui::* を呼べない。
    m_Imgui.NewFrame();
    CGame::OnRender();
    m_Imgui.Render();
}

void CSceneInspectorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    CGame::OnShutdown();
    m_Imgui.Shutdown();
}

void CSceneInspectorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    CGame::OnEvent(e);
}

TUniquePtr<AScene> CSceneInspectorApp::InitialScene() noexcept {
    return MakeUnique<ASceneInspectorScene>();
}

} // namespace helloscene
