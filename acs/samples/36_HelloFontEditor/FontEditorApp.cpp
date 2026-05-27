// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — FontEditorApp 実装。
// ImGui lifecycle を FGame に持たせる薄いラッパ。
#include "FontEditorApp.h"
#include "FontEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellofont {

void FontEditorApp::OnStart() noexcept {
    // ImGui を FWindow + FRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[FontEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void FontEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void FontEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void FontEditorApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> FontEditorApp::InitialScene() noexcept {
    return MakeUnique<FontEditorScene>();
}

} // namespace hellofont
