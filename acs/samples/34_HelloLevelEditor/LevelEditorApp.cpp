// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — LevelEditorApp 実装。
#include "LevelEditorApp.h"
#include "LevelEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellole {

void LevelEditorApp::OnStart() noexcept {
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[LevelEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    Game::OnStart();
}

void LevelEditorApp::OnRender() noexcept {
    _imgui.NewFrame();
    Game::OnRender();
    _imgui.Render();
}

void LevelEditorApp::OnShutdown() noexcept {
    Game::OnShutdown();
    _imgui.Shutdown();
}

void LevelEditorApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    Game::OnEvent(e);
}

UniquePtr<Scene> LevelEditorApp::InitialScene() noexcept {
    return MakeUnique<LevelEditorScene>();
}

} // namespace hellole
