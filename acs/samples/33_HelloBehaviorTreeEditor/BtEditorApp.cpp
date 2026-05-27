// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorApp 実装。
#include "BtEditorApp.h"
#include "BtEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void BtEditorApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[BtEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    FGame::OnStart();
}

void BtEditorApp::OnRender() noexcept {
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void BtEditorApp::OnShutdown() noexcept {
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void BtEditorApp::OnEvent(const Event& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> BtEditorApp::InitialScene() noexcept {
    return MakeUnique<BtEditorScene>();
}

} // namespace hellobt
