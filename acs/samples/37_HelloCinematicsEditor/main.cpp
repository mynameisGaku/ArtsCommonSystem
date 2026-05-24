// SPDX-License-Identifier: Apache-2.0
// HelloCinematicsEditor — GameFramework Phase 23 editor: Cinematics Timeline Editor
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace + EditorTheme と、Phase 23 で
//     実装した cinetimeline/CinematicsTimelineEditorPanel を 1 個の Workspace
//     に集約。
//   ・サンプル初期化時に CinematicsDirector を作成して panel に bind、
//     初期 keyframe を 3 個追加:
//       1) CameraCut       @ 0s
//       2) FadeColor       @ 2s
//       3) TriggerCallback @ 5s
//   ・ユーザは panel 上で Play / Pause / Stop / Step + time scrub +
//     marker drag + Add Keyframe + Inspector 編集を対話的に行える。
//   ・Tick (= Scene::OnUpdate) で panel.Step(dt) を呼び、Play 中なら director
//     が時間進行 + keyframe 発火する (= callback が main 側に紐付いていれば
//     ACS_LOG_INFO で発火を可視化)。
//   ・MainMenuBar:
//       File > Save / Load / Quit (Save/Load は stub)
//     Window / Layout は EditorWorkspace が自前で MenuBar に push する。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31/32 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。CMakeLists.txt 側
//                  および root acs/CMakeLists.txt の `acs_add_sample` 呼出を
//                  ACS_RENDER_DX12_RAW で guard する。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。`OnStart/OnShutdown/OnEvent` で
//     lifecycle を回し、`OnRender` を override して `NewFrame() → Scene OnRender
//     → Render()` の順で挟む (= sample 29/30/31/32 と同形)。
//   ・Scene は WantedServices() を None のままにする (= scene clock 等は不要、
//     director は Scene 自身がメンバとして持ち、panel に raw 参照を渡す)。
//   ・director は Scene のメンバとして所有 (= panel が non-owning raw pointer で
//     参照する規約に合わせる)。panel.SetCinematicsDirector(&_director) で bind。
//   ・初期 keyframe を 3 個追加することで、ユーザがサンプル起動直後から panel
//     上に marker を確認でき、編集 / 再生 / 削除を即試せる UX にする。
//   ・CinematicsDirector の callback (Camera / Event 等) は本 sample では
//     可視化目的で ACS_LOG_INFO に紐付ける (= Play 押下 + Step 進行で「発火が
//     確かに起きている」ことをログで観察できる)。

#include "gameframework/GameFramework.h"

// ----- CinematicsDirector (編集対象) -----
#include "gameframework/CinematicsDirector.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// ----- cinetimeline (Phase 23) -----
#include "gameframework/tools/cinetimeline/CinematicsTimelineEditorPanel.h"

// ----- ImGui / Input / Log -----
#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "memory/UniquePtr.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// CineEditorScene — Workspace + 1 panel + 1 CinematicsDirector
// ============================================================================
class CineEditorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // ---- File menu stub (実 dialog / serializer は将来 Phase) ----
    static constexpr const char* kCinePath = "preset.acscinetimeline";

    // ---- editor_core (Phase 21a) ----
    editor_core::EditorWorkspace                  _workspace;
    editor_core::EditorTheme                      _theme;

    // ---- cinetimeline (Phase 23) ----
    cinetimeline::CinematicsTimelineEditorPanel   _cine_panel;

    // ---- 編集対象の CinematicsDirector (= Scene が所有、panel は raw 参照) ----
    CinematicsDirector                            _director;

    // ---- runtime callback (= keyframe 発火可視化用、ACS_LOG_INFO に出力) ----
    static void OnCamera(void* /*user*/, Vec2 target, f32 zoom, f32 dur) noexcept {
        ACS_LOG_INFO("[CineEditor] Camera fire -> target=(%.2f, %.2f) zoom=%.2f dur=%.2f",
                     static_cast<double>(target.x), static_cast<double>(target.y),
                     static_cast<double>(zoom), static_cast<double>(dur));
    }
    static void OnEvent(void* /*user*/, u32 event_id) noexcept {
        ACS_LOG_INFO("[CineEditor] Event fire -> id=%u", event_id);
    }
};

// ----------------------------------------------------------------------------
// OnEnter — theme/workspace/panel/director を初期化 + 初期 keyframe 3 個
// ----------------------------------------------------------------------------
void CineEditorScene::OnEnter() noexcept {
    // editor らしい暗グレー背景 (= 残余領域のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    // ImGuiCtx::Init は CineEditorGame::OnStart で済んでいる前提。
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
                // 実シリアライザは未実装 (Phase 23 では UI のみ)。
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

// ============================================================================
// CineEditorGame — ImGui lifecycle を Game に持たせる薄いラッパ (sample 29/30/31/32 と同形)
// ============================================================================
class CineEditorGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[CineEditorGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        // 基底の OnStart は InitialScene() を push する。
        Game::OnStart();
    }

    void OnRender() noexcept override {
        // ImGui フレーム開始 → Scene::OnRender で ImGui::* が呼ばれる →
        // ImGui の描画コマンドをコマンドリストに発行、の順。
        _imgui.NewFrame();
        Game::OnRender();
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        // Scene 側を先に止めてから ImGui を落とす (Scene が ImGui::* を握って
        // ないことを保証)。
        Game::OnShutdown();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
        Game::OnEvent(e);
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<CineEditorScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(CineEditorGame)
