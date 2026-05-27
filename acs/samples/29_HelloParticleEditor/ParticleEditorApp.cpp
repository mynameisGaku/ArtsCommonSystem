// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — ParticleEditorApp 実装。
#include "ParticleEditorApp.h"
#include "ParticleEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloparticleed {

void ParticleEditorApp::OnStart() noexcept {
    // ImGui を FWindow + FRenderer に紐付け。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void ParticleEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 -> Scene::OnRender で ImGui::* が呼ばれる ->
    // ImGui の描画コマンドをコマンドリストに発行、の順。Scene の Render
    // ロジックは FGame::OnRender が FSceneManager 経由で実行する。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void ParticleEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void ParticleEditorApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> ParticleEditorApp::InitialScene() noexcept {
    return MakeUnique<ParticleEditorScene>();
}

} // namespace helloparticleed
