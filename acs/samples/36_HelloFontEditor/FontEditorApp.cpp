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
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[FontEditorApp] FImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void FontEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → FScene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    _imgui.NewFrame();
    FGame::OnRender();
    _imgui.Render();
}

void FontEditorApp::OnShutdown() noexcept {
    // FScene 側を先に止めてから ImGui を落とす (FScene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    _imgui.Shutdown();
}

void FontEditorApp::OnEvent(const FEvent& e) noexcept {
    _imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> FontEditorApp::InitialScene() noexcept {
    return MakeUnique<FontEditorScene>();
}

} // namespace hellofont
