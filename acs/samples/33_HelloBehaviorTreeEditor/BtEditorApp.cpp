// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorApp 実装。
#include "BtEditorApp.h"
#include "BtEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void FBtEditorApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[BtEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    FGame::OnStart();
}

void FBtEditorApp::OnRender() noexcept {
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void FBtEditorApp::OnShutdown() noexcept {
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void FBtEditorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> FBtEditorApp::InitialScene() noexcept {
    return MakeUnique<FBtEditorScene>();
}

} // namespace hellobt
