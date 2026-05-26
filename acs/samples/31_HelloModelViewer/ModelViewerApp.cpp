// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — ModelViewerApp 実装。
#include "ModelViewerApp.h"
#include "ModelViewerScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellomv {

void ModelViewerApp::OnStart() noexcept {
    // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewerApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    Game::OnStart();
}

void ModelViewerApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で 3D + ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    _imgui.NewFrame();
    Game::OnRender();
    _imgui.Render();
}

void ModelViewerApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    Game::OnShutdown();
    _imgui.Shutdown();
}

void ModelViewerApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    Game::OnEvent(e);
}

TUniquePtr<Scene> ModelViewerApp::InitialScene() noexcept {
    return MakeUnique<ModelViewerScene>();
}

} // namespace hellomv
