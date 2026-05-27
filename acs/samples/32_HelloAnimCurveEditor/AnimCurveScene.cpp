// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — AnimCurveScene 実装。
#include "AnimCurveScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace helloac {

void AnimCurveScene::OnEnter() noexcept {
    // editor らしい暗グレー背景 (= ImGui に覆われない viewport 余白のクリア色を
    // 編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // Theme は ImGui context が存在してから Init する (= AnimCurveApp::OnStart で
    // ImGuiCtx::Init が走った後にこの OnEnter が呼ばれる前提)。
    m_Theme.Init();
    m_Theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    m_Workspace.Init();
    m_CurvePanel.Init();

    // 山なりの初期 curve (3 個の Hermite key):
    //   t=0   v=0   in:0   out:+2  上り始め
    //   t=0.5 v=1   in:0   out:0   山の頂点
    //   t=1   v=0   in:-2  out:0   下り終わり
    m_ExampleCurve.AddKeyHermite(0.0f, 0.0f, 0.0f,  2.0f);
    m_ExampleCurve.AddKeyHermite(0.5f, 1.0f, 0.0f,  0.0f);
    m_ExampleCurve.AddKeyHermite(1.0f, 0.0f, -2.0f, 0.0f);

    // panel は curve の raw 参照のみ保持する (所有権は Scene 側)。
    m_CurvePanel.SetCurve(&m_ExampleCurve);

    // RegisterPanel は内部で panel->OnInit(*this) を呼ぶので、ここで明示的に
    // OnInit を呼ぶと二重初期化になる。
    m_Workspace.RegisterPanel(&m_CurvePanel);

    ACS_LOG_INFO("[AnimCurveEditor] entered (workspace + panel + 3 Hermite keys)");
}

void AnimCurveScene::OnExit() noexcept {
    // Workspace::Shutdown が登録済み全 panel の OnShutdown を呼ぶので、個別の
    // UnregisterPanel は呼ばない (= 二重 OnShutdown を避ける)。
    m_Workspace.Shutdown();
    m_CurvePanel.Shutdown();

    // m_ExampleCurve は Scene のメンバなので自動破棄される。
    // FEditorTheme::Shutdown は存在しない (Dtor で十分)。
    ACS_LOG_INFO("[AnimCurveEditor] exited");
}

void AnimCurveScene::OnUpdate(f32 dt) noexcept {
    (void)dt;
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
}

void AnimCurveScene::OnRender(RenderContext& /*rc*/) noexcept {
    m_DrawFileMenu();

    // TickAllPanels は OnFrameBegin → DockSpace → MenuBar → 各 panel の DrawUI
    // を順に発火する。ImGui::* を含むので NewFrame() と Render() の間 (=
    // OnRender) で呼ぶ必要がある。dt は RenderContext から取れないため FGame の
    // DeltaTime() を使う。
    m_Workspace.TickAllPanels(GetGame().DeltaTime());
}

void AnimCurveScene::m_DrawFileMenu() noexcept {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("Save Curve")) {
            // シリアライザは未実装。ClearDirty で「保存済み」状態にだけ遷移する。
            m_CurvePanel.ClearDirty();
            ACS_LOG_INFO("[AnimCurveEditor] Save Curve -> '%s' (stub, no-op)", kCurvePath);
        }
        if (ImGui::MenuItem("Load Curve")) {
            ACS_LOG_INFO("[AnimCurveEditor] Load Curve <- '%s' (stub, no-op)", kCurvePath);
        }
        ImGui::Separator();
        // 未保存変更の有無を可視化する disabled MenuItem。
        if (m_CurvePanel.IsDirty()) {
            ImGui::MenuItem("[modified]", nullptr, false, false);
        } else {
            ImGui::MenuItem("[saved]", nullptr, false, false);
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Quit", "Esc")) {
            GetGame().Quit();
        }
        ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
}

} // namespace helloac
