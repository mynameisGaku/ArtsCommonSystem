// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — LevelEditorApp 実装。
#include "LevelEditorApp.h"
#include "LevelEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellole {

void LevelEditorApp::OnStart() noexcept {
    // ImGui を FWindow + FRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[LevelEditorApp] FImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void LevelEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 → FScene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    _imgui.NewFrame();
    FGame::OnRender();
    _imgui.Render();
}

void LevelEditorApp::OnShutdown() noexcept {
    // FScene 側を先に止めてから ImGui を落とす (FScene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    _imgui.Shutdown();
}

void LevelEditorApp::OnEvent(const FEvent& e) noexcept {
    _imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> LevelEditorApp::InitialScene() noexcept {
    return MakeUnique<LevelEditorScene>();
}

} // namespace hellole
