// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — AnimCurveScene 実装。
#include "AnimCurveScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace helloac {

// ----------------------------------------------------------------------------
// OnEnter — theme/workspace/panel/curve を初期化、panel に curve を bind
// ----------------------------------------------------------------------------
void AnimCurveScene::OnEnter() noexcept {
    // editor らしい暗グレー背景 (= 残余領域のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    // ImGuiCtx::Init は AnimCurveApp::OnStart で済んでいる前提。
    _theme.Init();
    _theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // ---- Workspace 本体 ----
    _workspace.Init();

    // ---- AnimCurveEditorPanel: Init + curve bind + workspace register ----
    _curve_panel.Init();

    // ---- 初期 curve: 3 個の Hermite key で山なりの形を作る ----
    // t=0   → v=0   (in:0, out:+2): 上り始め
    // t=0.5 → v=1   (in:0, out:0):  山の頂点
    // t=1   → v=0   (in:-2, out:0): 下り終わり
    _example_curve.AddKeyHermite(0.0f, 0.0f, 0.0f,  2.0f);
    _example_curve.AddKeyHermite(0.5f, 1.0f, 0.0f,  0.0f);
    _example_curve.AddKeyHermite(1.0f, 0.0f, -2.0f, 0.0f);

    // panel に curve を bind (= panel は raw 参照を保持、Scene が curve を所有)
    _curve_panel.SetCurve(&_example_curve);

    // EditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ
    // (EditorWorkspace.cpp §119)。したがって _curve_panel.OnInit を別途呼ぶ
    // 必要は無い (= 二重 OnInit を誘発する)。以降の MainLoop 内で
    // TickAllPanels が OnFrameBegin → DrawUI を順に回す。
    _workspace.RegisterPanel(&_curve_panel);

    ACS_LOG_INFO("[AnimCurveEditor] entered (workspace + panel + 3 Hermite keys)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown (workspace → panel → theme は no-op)
// ----------------------------------------------------------------------------
void AnimCurveScene::OnExit() noexcept {
    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する (EditorWorkspace.cpp §76)。よって個別
    // UnregisterPanel は不要 (= 二重 OnShutdown を避けるため)。
    _workspace.Shutdown();

    // panel 内 state をリセット (Init 後の解放手順)。
    _curve_panel.Shutdown();

    // _example_curve は Scene のメンバなので自動破棄 (~AnimationCurve)。
    // EditorTheme::Shutdown は存在しない API なので明示解放は不要 (Dtor で十分)。

    ACS_LOG_INFO("[AnimCurveEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — Esc 終了のみ (ImGui 系は OnRender 側)
// ----------------------------------------------------------------------------
void AnimCurveScene::OnUpdate(f32 dt) noexcept {
    (void)dt;
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
}

// ----------------------------------------------------------------------------
// OnRender — File menu → Workspace 全描画
// ----------------------------------------------------------------------------
void AnimCurveScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- File メニュー (Workspace::DrawMenuBar の前に push する) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Curve")) {
                // 実シリアライザは未実装 (Phase 22 では UI のみ)。
                // panel が dirty なら ClearDirty で「保存済み」状態にする。
                _curve_panel.ClearDirty();
                ACS_LOG_INFO("[AnimCurveEditor] Save Curve -> '%s' (stub, no-op)", kCurvePath);
            }
            if (ImGui::MenuItem("Load Curve")) {
                ACS_LOG_INFO("[AnimCurveEditor] Load Curve <- '%s' (stub, no-op)", kCurvePath);
            }
            ImGui::Separator();
            // dirty 状態表示 (= 未保存変更があるかの可視化)。
            if (_curve_panel.IsDirty()) {
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

    // ---- Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar →
    //      各 panel DrawUI を順に発火) ----
    // EditorWorkspace::TickAllPanels は ImGui 系の Draw 呼出を含むため OnRender
    // の中で呼ぶ。dt は RenderContext からは取れないので Game の DeltaTime() を使う。
    _workspace.TickAllPanels(GetGame().DeltaTime());
}

} // namespace helloac
