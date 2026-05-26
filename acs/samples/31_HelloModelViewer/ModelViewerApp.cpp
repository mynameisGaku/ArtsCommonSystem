// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — ModelViewerApp 実装。
#include "ModelViewerApp.h"
#include "ModelViewerScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellomv {

void ModelViewerApp::OnStart() noexcept {
    // ImGui を FWindow + FRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewerApp] FImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void ModelViewerApp::OnRender() noexcept {
    // ImGui フレーム開始 → FScene::OnRender で 3D + ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    _imgui.NewFrame();
    FGame::OnRender();
    _imgui.Render();
}

void ModelViewerApp::OnShutdown() noexcept {
    // FScene 側を先に止めてから ImGui を落とす (FScene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    _imgui.Shutdown();
}

void ModelViewerApp::OnEvent(const FEvent& e) noexcept {
    _imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> ModelViewerApp::InitialScene() noexcept {
    return MakeUnique<ModelViewerScene>();
}

} // namespace hellomv
