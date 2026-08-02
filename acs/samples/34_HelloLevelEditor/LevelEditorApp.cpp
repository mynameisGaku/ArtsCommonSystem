// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — LevelEditorApp 実装。
#include "LevelEditorApp.h"
#include "LevelEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellole {

void CLevelEditorApp::OnStart() noexcept {
    // ImGui を FWindow + CRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[LevelEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    CGame::OnStart();
}

void CLevelEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    m_Imgui.NewFrame();
    CGame::OnRender();
    m_Imgui.Render();
}

void CLevelEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    CGame::OnShutdown();
    m_Imgui.Shutdown();
}

void CLevelEditorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    CGame::OnEvent(e);
}

TUniquePtr<AScene> CLevelEditorApp::InitialScene() noexcept {
    return MakeUnique<ALevelEditorScene>();
}

} // namespace hellole
