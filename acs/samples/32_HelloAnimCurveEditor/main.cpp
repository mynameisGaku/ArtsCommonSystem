// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — GameFramework Phase 22 editor 第四弾: AnimationCurve Editor
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace + EditorTheme と、
//     Phase 22 で実装した animcurve/AnimCurveEditorPanel を 1 個の Workspace
//     に集約。
//   ・サンプル初期化時に 3 個の Hermite key を持つ AnimationCurve を生成し、
//     panel に bind。ユーザは ImGui canvas 上で key の drag / tangent 編集 /
//     右クリック追加 / 削除を対話的に行える。
//   ・MainMenuBar:
//       File > Save curve / Load curve / Quit
//     Save / Load は stub (ACS_LOG_INFO のみ、ファイル I/O は将来 Phase で
//     AnimationCurve のシリアライザを追加した時点で配線)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。`OnStart/OnShutdown/OnEvent` で
//     lifecycle を回し、`OnRender` を override して `NewFrame() → Scene OnRender
//     → Render()` の順で挟む (= sample 29/30/31 と同形)。
//   ・Scene は WantedServices() を None のままにする (= scene clock 等は不要、
//     curve は Scene 自身がメンバとして持ち、panel に raw 参照を渡す)。
//   ・curve は Scene のメンバとして所有 (= panel が non-owning raw pointer で
//     参照する規約に合わせる)。panel.SetCurve(&_example_curve) で bind。
//   ・初期 curve は「3 点 Hermite (= 山なり、開始 0 / 中央 1 / 終了 0)」。
//     ユーザがすぐに「曲線らしい形」を見て編集を始められるよう、最初から
//     視覚的にわかりやすい形にしておく。

#include "gameframework/GameFramework.h"

// ----- AnimationCurve (編集対象) -----
#include "gameframework/AnimationCurve.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// ----- animcurve (Phase 22) -----
#include "gameframework/tools/animcurve/AnimCurveEditorPanel.h"

// ----- ImGui / Input / Log -----
#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"
#include "memory/UniquePtr.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// AnimCurveScene — Workspace + 1 panel + 1 AnimationCurve (Hermite 3 key)
// ============================================================================
class AnimCurveScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // ---- File menu stub (実 dialog / serializer は将来 Phase) ----
    static constexpr const char* kCurvePath = "preset.acscurve";

    // ---- editor_core (Phase 21a) ----
    editor_core::EditorWorkspace       _workspace;
    editor_core::EditorTheme           _theme;

    // ---- animcurve (Phase 22) ----
    animcurve::AnimCurveEditorPanel    _curve_panel;

    // ---- 編集対象の AnimationCurve (= Scene が所有、panel は raw 参照) ----
    AnimationCurve                     _example_curve;
};

// ----------------------------------------------------------------------------
// OnEnter — theme/workspace/panel/curve を初期化、panel に curve を bind
// ----------------------------------------------------------------------------
void AnimCurveScene::OnEnter() noexcept {
    // editor らしい暗グレー背景 (= 残余領域のクリア色を編集向けに揃える)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    // ImGuiCtx::Init は AnimCurveGame::OnStart で済んでいる前提。
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

// ============================================================================
// AnimCurveGame — ImGui lifecycle を Game に持たせる薄いラッパ (sample 29/30/31 と同形)
// ============================================================================
class AnimCurveGame : public Game {
public:
    void OnStart() noexcept override {
        // ImGui を Window + Renderer に紐付け。失敗時は早期 Quit。
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[AnimCurveGame] ImGuiCtx.Init failed -> Quit");
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
        return MakeUnique<AnimCurveScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(AnimCurveGame)
