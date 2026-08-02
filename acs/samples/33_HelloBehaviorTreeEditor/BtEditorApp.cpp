// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — BtEditorApp 実装。
#include "BtEditorApp.h"
#include "BtEditorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellobt {

void CBtEditorApp::OnStart() noexcept {
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[BtEditorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    CGame::OnStart();
}

void CBtEditorApp::OnRender() noexcept {
    m_Imgui.NewFrame();
    CGame::OnRender();
    m_Imgui.Render();
}

void CBtEditorApp::OnShutdown() noexcept {
    CGame::OnShutdown();
    m_Imgui.Shutdown();
}

void CBtEditorApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    CGame::OnEvent(e);
}

TUniquePtr<AScene> CBtEditorApp::InitialScene() noexcept {
    return MakeUnique<ABtEditorScene>();
}

} // namespace hellobt
