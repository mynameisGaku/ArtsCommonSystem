// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — ParticleEditorApp 実装。
#include "ParticleEditorApp.h"
#include "ParticleEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloparticleed {

void CParticleEditorApp::OnStart() noexcept {
    // ImGui を FWindow + CRenderer に紐付け。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    CGame::OnStart();
}

void CParticleEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 -> Scene::OnRender で ImGui::* が呼ばれる ->
    // ImGui の描画コマンドをコマンドリストに発行、の順。Scene の Render
    // ロジックは CGame::OnRender が CSceneManager 経由で実行する。
    m_Imgui.NewFrame();
    CGame::OnRender();
    m_Imgui.Render();
}

void CParticleEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    CGame::OnShutdown();
    m_Imgui.Shutdown();
}

void CParticleEditorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    CGame::OnEvent(e);
}

TUniquePtr<AScene> CParticleEditorApp::InitialScene() noexcept {
    return MakeUnique<AParticleEditorScene>();
}

} // namespace helloparticleed
