// SPDX-License-Identifier: Apache-2.0
// HelloFontEditor — FontEditorApp 実装。
// ImGui lifecycle を Game に持たせる薄いラッパ。
#include "FontEditorApp.h"
#include "FontEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellofont {

void FontEditorApp::OnStart() noexcept {
    // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[FontEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    Game::OnStart();
}

void FontEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    _imgui.NewFrame();
    Game::OnRender();
    _imgui.Render();
}

void FontEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    Game::OnShutdown();
    _imgui.Shutdown();
}

void FontEditorApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    Game::OnEvent(e);
}

TUniquePtr<Scene> FontEditorApp::InitialScene() noexcept {
    return MakeUnique<FontEditorScene>();
}

} // namespace hellofont
