// SPDX-License-Identifier: Apache-2.0
// HelloSpriteAtlasEditor — SpriteAtlasApp 実装。
// ImGui lifecycle を FGame に持たせる薄いラッパ。
#include "SpriteAtlasApp.h"
#include "SpriteAtlasScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellosa {

void FSpriteAtlasApp::OnStart() noexcept {
    // ImGui を FWindow + FRenderer に紐付け。失敗時は早期 Quit。
    if (auto r = m_Imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[SpriteAtlasApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void FSpriteAtlasApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。
    m_Imgui.NewFrame();
    FGame::OnRender();
    m_Imgui.Render();
}

void FSpriteAtlasApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    m_Imgui.Shutdown();
}

void FSpriteAtlasApp::OnEvent(const FEvent& e) noexcept {
    m_Imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<FScene> FSpriteAtlasApp::InitialScene() noexcept {
    return MakeUnique<FSpriteAtlasScene>();
}

} // namespace hellosa
