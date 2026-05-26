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
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[ParticleEditorApp] FImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void ParticleEditorApp::OnRender() noexcept {
    // ImGui フレーム開始 -> FScene::OnRender で ImGui::* が呼ばれる ->
    // ImGui の描画コマンドをコマンドリストに発行、の順。FScene の Render
    // ロジックは FGame::OnRender が FSceneManager 経由で実行する。
    _imgui.NewFrame();
    FGame::OnRender();
    _imgui.Render();
}

void ParticleEditorApp::OnShutdown() noexcept {
    // FScene 側を先に止めてから ImGui を落とす (FScene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    _imgui.Shutdown();
}

void ParticleEditorApp::OnEvent(const FEvent& e) noexcept {
    _imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> ParticleEditorApp::InitialScene() noexcept {
    return MakeUnique<ParticleEditorScene>();
}

} // namespace helloparticleed
