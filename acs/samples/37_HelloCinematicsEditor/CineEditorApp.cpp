// SPDX-License-Identifier: Apache-2.0
// HelloCinematicsEditor — CineEditorApp 実装。
// ImGui lifecycle を CGame に持たせる薄いラッパ。
#include "CineEditorApp.h"
#include "CineEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellocine {

void CCineEditorApp::OnStart() noexcept {
    // ImGui を FWindow + CRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[CineEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    CGame::OnStart();
}

void CCineEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    m_Imgui.NewFrame();
    CGame::OnRender();
    m_Imgui.Render();
}

void CCineEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    CGame::OnShutdown();
    m_Imgui.Shutdown();
}

void CCineEditorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    CGame::OnEvent(e);
}

TUniquePtr<AScene> CCineEditorApp::InitialScene() noexcept {
    return MakeUnique<ACineEditorScene>();
}

} // namespace hellocine
