// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorApp 実装。
#include "BtEditorApp.h"
#include "BtEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void BtEditorApp::OnStart() noexcept {
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[BtEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    Game::OnStart();
}

void BtEditorApp::OnRender() noexcept {
    _imgui.NewFrame();
    Game::OnRender();
    _imgui.Render();
}

void BtEditorApp::OnShutdown() noexcept {
    Game::OnShutdown();
    _imgui.Shutdown();
}

void BtEditorApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    Game::OnEvent(e);
}

TUniquePtr<Scene> BtEditorApp::InitialScene() noexcept {
    return MakeUnique<BtEditorScene>();
}

} // namespace hellobt
