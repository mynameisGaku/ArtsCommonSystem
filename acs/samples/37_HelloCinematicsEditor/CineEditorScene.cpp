// SPDX-License-Identifier: Apache-2.0
// HelloCinematicsEditor — CineEditorScene 実装。
#include "CineEditorScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellocine {

// ---- runtime callback (= keyframe 発火可視化用、ACS_LOG_INFO に出力) ----
void CineEditorScene::OnCamera(void* /*user*/, Vec2 target, f32 zoom, f32 dur) noexcept {
    ACS_LOG_INFO("[CineEditor] Camera fire -> target=(%.2f, %.2f) zoom=%.2f dur=%.2f",
                 static_cast<double>(target.x), static_cast<double>(target.y),
                 static_cast<double>(zoom), static_cast<double>(dur));
}

void CineEditorScene::OnEvent(void* /*user*/, u32 event_id) noexcept {
    ACS_LOG_INFO("[CineEditor] Event fire -> id=%u", event_id);
}

// ----------------------------------------------------------------------------
// OnEnter — theme/workspace/panel/director を初期化 + 初期 keyframe 3 個
// ----------------------------------------------------------------------------
void CineEditorScene::OnEnter() noexcept {
    // editor らしい暗グレー背景 (= 残余領域のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    // ImGuiCtx::Init は CineEditorApp::OnStart で済んでいる前提。
    _theme.Init();
    _theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // ---- Workspace 本体 ----
    _workspace.Init();

    // ---- CinematicsDirector に runtime callback を登録 (発火可視化用) ----
    _director.SetCameraCallback(&CineEditorScene::OnCamera, this);
    _director.SetEventCallback (&CineEditorScene::OnEvent,  this);

    // ---- CinematicsTimelineEditorPanel: Init + director bind + 初期 KF 3 個 ----
    _cine_panel.Init();
    _cine_panel.SetCinematicsDirector(&_director);

    // 初期 keyframe を 3 個追加 (= ユーザが起動直後から marker を見て編集を
    // 始められる UX)。
    //   1) CameraCut       @ 0s  — 開始フレームのカメラ切替
    //   2) FadeColor       @ 2s  — 黒からのフェードイン演出
    //   3) TriggerCallback @ 5s  — 任意ロジックの発火点 (event_id=0)
    _cine_panel.AddKeyframe(cinetimeline::ETimelineKeyKind::CameraCut,       0.0f);
    _cine_panel.AddKeyframe(cinetimeline::ETimelineKeyKind::FadeColor,       2.0f);
    _cine_panel.AddKeyframe(cinetimeline::ETimelineKeyKind::TriggerCallback, 5.0f);

    // EditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ
    // (EditorWorkspace.cpp §119)。したがって _cine_panel.OnInit を別途呼ぶ
    // 必要は無い (= 二重 OnInit を誘発する)。以降の MainLoop 内で
    // TickAllPanels が OnFrameBegin → DrawUI を順に回す。
    _workspace.RegisterPanel(&_cine_panel);

    ACS_LOG_INFO("[CineEditor] entered (workspace + panel + 3 initial keyframes)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown (workspace → panel → theme は no-op)
// ----------------------------------------------------------------------------
void CineEditorScene::OnExit() noexcept {
    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する (EditorWorkspace.cpp §76)。よって個別
    // UnregisterPanel は不要 (= 二重 OnShutdown を避けるため)。
    _workspace.Shutdown();

    // panel 内 state をリセット (= Init 後の解放手順、director ptr を解除)。
    _cine_panel.Shutdown();

    // _director は Scene のメンバなので自動破棄。
    // EditorTheme::Shutdown は存在しない API なので明示解放は不要 (Dtor で十分)。

    ACS_LOG_INFO("[CineEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — Esc 終了 + Play 中なら panel.Step で時間進行 + keyframe 発火
// ----------------------------------------------------------------------------
void CineEditorScene::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }

    // dt スパイク防御 (Application 側の大 dt で再生が暴れない)。
    if (dt > 0.1f) dt = 0.1f;

    // Play 中なら time を進める。panel.Step は内部で director.Tick を呼んで
    // keyframe を発火 (= 登録した callback が ACS_LOG_INFO で表示)。
    _cine_panel.Step(dt);
}

// ----------------------------------------------------------------------------
// OnRender — File menu → Workspace 全描画
// ----------------------------------------------------------------------------
void CineEditorScene::OnRender(RenderContext& /*rc*/) noexcept {
    // ---- File メニュー (Workspace::DrawMenuBar の前に push する) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Cinematics")) {
                // 実シリアライザは未実装 (本サンプルは UI のみ)。
                ACS_LOG_INFO("[CineEditor] Save Cinematics -> '%s' (stub, no-op)", kCinePath);
            }
            if (ImGui::MenuItem("Load Cinematics")) {
                ACS_LOG_INFO("[CineEditor] Load Cinematics <- '%s' (stub, no-op)", kCinePath);
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

} // namespace hellocine
