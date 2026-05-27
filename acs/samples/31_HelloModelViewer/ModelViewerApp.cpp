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
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ModelViewerApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void ModelViewerApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で 3D + ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void ModelViewerApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void ModelViewerApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> ModelViewerApp::InitialScene() noexcept {
    return MakeUnique<ModelViewerScene>();
}

} // namespace hellomv
