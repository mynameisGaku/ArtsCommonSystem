// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — SceneInspectorApp 実装。
#include "SceneInspectorApp.h"
#include "SceneInspectorScene.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace helloscene {

void SceneInspectorApp::OnStart() noexcept {
    if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
        ACS_LOG_ERROR("[SceneInspectorApp] ImGuiCtx.Init failed -> Quit");
        Quit();
        return;
    }
    // 基底の OnStart は InitialScene() を push する。
    FGame::OnStart();
}

void SceneInspectorApp::OnRender() noexcept {
    // ImGui フレーム開始 → Scene::OnRender 内で ImGui::* が呼ばれる →
    // ImGui の描画コマンドをコマンドリストに発行、の順。この順番でないと
    // Scene 側が ImGui::* を呼べない。
    _imgui.NewFrame();
    FGame::OnRender();
    _imgui.Render();
}

void SceneInspectorApp::OnShutdown() noexcept {
    // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
    // ないことを保証)。
    FGame::OnShutdown();
    _imgui.Shutdown();
}

void SceneInspectorApp::OnEvent(const Event& e) noexcept {
    _imgui.OnEvent(e);
    FGame::OnEvent(e);
}

TUniquePtr<Scene> SceneInspectorApp::InitialScene() noexcept {
    return MakeUnique<SceneInspectorScene>();
}

} // namespace helloscene
