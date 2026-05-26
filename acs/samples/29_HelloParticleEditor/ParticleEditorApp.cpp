// SPDX-License-Identifier: Apache-2.0
// HelloParticleEditor — ParticleEditorApp 実装。
#include "ParticleEditorApp.h"
#include "ParticleEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloparticleed {

void ParticleEditorApp::OnStart() noexcept {
    // ImGui を Window + Renderer に紐付け。
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    Game::OnStart();
}

void ParticleEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 -> Scene::OnRender で ImGui::* が呼ばれる ->
    // ImGui の描画コマンドをコマンドリストに発行、の順。Scene の Render
    // ロジックは Game::OnRender が SceneManager 経由で実行する。
    _imgui.NewFrame();
    Game::OnRender();
    _imgui.Render();
}

void ParticleEditorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    Game::OnShutdown();
    _imgui.Shutdown();
}

void ParticleEditorApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    Game::OnEvent(e);
}

UniquePtr<Scene> ParticleEditorApp::InitialScene() noexcept {
    return MakeUnique<ParticleEditorScene>();
}

} // namespace helloparticleed
