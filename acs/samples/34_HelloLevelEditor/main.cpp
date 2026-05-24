// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — GameFramework Phase 22 editor: Tilemap (Level) Editor
//
// 動作:
//   ・GameFramework Pillar Q の `acs::game::Tilemap` (32x32, 2 layer, tile_size=16)
//     を 1 個立て、適当な「壁・床」初期パターンを焼く。
//   ・Phase 21a の `editor_core::EditorWorkspace` + `editor_core::EditorTheme` と、
//     Phase 22 で実装した `leveledit::LevelEditorPanel` を統合。
//   ・LevelEditorPanel が tilemap を ImDrawList で矩形描画し、Paint / Erase /
//     Fill / Pick の 4 ブラシでマウス編集できる。
//   ・main menu bar "File > Save Tilemap / Load Tilemap" は stub callback
//     (Phase 22 では実シリアライザ未配線、ACS_LOG_INFO だけ出す)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31/32 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// 設計メモ:
//   ・ImGuiCtx は **Game クラスが所有**。`OnStart/OnShutdown/OnEvent` で
//     lifecycle を回し、`OnRender` を override して `NewFrame() → Scene OnRender
//     → Render()` の順で挟む (= sample 29/30/31 と同形)。
//   ・Scene は WantedServices() を None のままにする (= scene clock 等は不要)。
//   ・初期 tilemap パターン:
//       - layer 0 (床) : 全面 tile id=1 で埋める (= 緑系の床色)
//       - layer 1 (壁) : 外周 (x=0 / x=31 / y=0 / y=31) を tile id=10 で囲む、
//                        中央 (8..23, 8..23) の枠を tile id=20 で囲む
//     これで「外周の壁 + 内側の部屋の壁」が見える編集スタート状態になる。
//   ・Workspace 経由で Panel を登録するだけで自動描画される (Workspace::
//     TickAllPanels が DockSpace + MenuBar + 各 panel.DrawUI を呼ぶ)。

#include "gameframework/GameFramework.h"

// ----- editor_core (Phase 21a) -----
#include "gameframework/tools/editor_core/EditorWorkspace.h"
#include "gameframework/tools/editor_core/EditorTheme.h"

// ----- leveledit (Phase 22) -----
#include "gameframework/tools/leveledit/LevelEditorPanel.h"

// ----- Tilemap (Pillar Q) -----
#include "gameframework/Tilemap.h"

// ----- ImGui / Input / Log -----
#include "imgui/ImGuiContext.h"
#include "platform/Input.h"
#include "foundation/Log.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

// ============================================================================
// LevelEditorScene — Workspace + LevelEditorPanel + Tilemap (32x32, 2 layer)
// ============================================================================
class LevelEditorScene : public Scene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(f32 dt) noexcept override;
    void OnRender(RenderContext& rc) noexcept override;

private:
    // File menu stub (実 serializer は Phase 23+ で配線)
    static constexpr const char* kSavePath = "tilemap.acstilemap";

    // ---- editor_core (Phase 21a) ----
    editor_core::EditorWorkspace _workspace;
    editor_core::EditorTheme     _theme;

    // ---- leveledit (Phase 22) ----
    leveledit::LevelEditorPanel  _level_panel;

    // ---- 編集対象 Tilemap ----
    Tilemap                      _tilemap;
};

// ----------------------------------------------------------------------------
// OnEnter — theme / workspace / panel / tilemap を順に初期化
// ----------------------------------------------------------------------------
void LevelEditorScene::OnEnter() noexcept {
    // editor 風の暗グレー背景 (ImGui に隠れるが viewport の外側余白に出る)
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    _theme.Init();
    _theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // ---- Workspace 本体 ----
    _workspace.Init();

    // ---- Tilemap: 32x32 / 2 layer / tile_size=16 ----
    _tilemap.Init(/*width=*/32u, /*height=*/32u,
                  /*layer_count=*/2u, /*tile_size=*/16.0f);

    // 初期パターン: layer 0 = 床 (全面 tile id=1)
    _tilemap.Fill(TileId{1u}, /*layer=*/0u);

    // 初期パターン: layer 1 = 壁 (外周 + 中央部屋)
    // 外周 (4 辺) を tile id=10 で囲む
    _tilemap.FillRect(0u,  0u,  31u, 0u,  TileId{10u}, /*layer=*/1u);    // 下
    _tilemap.FillRect(0u,  31u, 31u, 31u, TileId{10u}, /*layer=*/1u);    // 上
    _tilemap.FillRect(0u,  0u,  0u,  31u, TileId{10u}, /*layer=*/1u);    // 左
    _tilemap.FillRect(31u, 0u,  31u, 31u, TileId{10u}, /*layer=*/1u);    // 右
    // 中央部屋 (8..23, 8..23) の枠を tile id=20 で囲む
    _tilemap.FillRect(8u,  8u,  23u, 8u,  TileId{20u}, /*layer=*/1u);    // 下辺
    _tilemap.FillRect(8u,  23u, 23u, 23u, TileId{20u}, /*layer=*/1u);    // 上辺
    _tilemap.FillRect(8u,  8u,  8u,  23u, TileId{20u}, /*layer=*/1u);    // 左辺
    _tilemap.FillRect(23u, 8u,  23u, 23u, TileId{20u}, /*layer=*/1u);    // 右辺

    // ---- LevelEditorPanel に tilemap を渡して Workspace に登録 ----
    _level_panel.Init();
    _level_panel.SetTilemap(&_tilemap);
    // editor 開始時は壁 (tile id=10) をスポイトしやすいよう Paint + active layer=1
    // の状態にしておく (= 触り始めに「壁を増やす」操作が直感的)。
    _level_panel.SetActiveLayer(1u);
    _level_panel.SetCurrentBrush(leveledit::EBrushKind::Paint);
    _level_panel.SetCurrentTileId(10u);
    _workspace.RegisterPanel(&_level_panel);

    // 初期視点を tilemap 全体が画面に収まる位置へ。
    _level_panel.Camera().FrameToBoundingBox2D(
        Vec2{0.0f, 0.0f},
        Vec2{static_cast<f32>(_tilemap.Width())  * _tilemap.TileSize(),
             static_cast<f32>(_tilemap.Height()) * _tilemap.TileSize()});

    ACS_LOG_INFO("[LevelEditor] entered (tilemap 32x32 / 2 layer / tile_size=16, 4 ブラシ + workspace + theme)");
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown
// ----------------------------------------------------------------------------
void LevelEditorScene::OnExit() noexcept {
    // EditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する。よって個別 UnregisterPanel を呼ぶ必要は無い。
    _workspace.Shutdown();
    _level_panel.Shutdown();
    // EditorTheme::Shutdown は存在しない API なので明示解放は不要 (Dtor で十分)。
    ACS_LOG_INFO("[LevelEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — 非 ImGui 系のロジック (Esc 終了のみ)
// ----------------------------------------------------------------------------
// ※ ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
//   DrawUI) はすべて OnRender 側へ。`ImGui::Begin` 等は NewFrame() と Render()
//   の間でしか呼べないため、ここでは Workspace::TickAllPanels は呼ばない。
void LevelEditorScene::OnUpdate(f32 dt) noexcept {
    (void)dt;
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
}

// ----------------------------------------------------------------------------
// OnRender — File menu → Workspace 全描画
// ----------------------------------------------------------------------------
void LevelEditorScene::OnRender(RenderContext& rc) noexcept {
    (void)rc;

    // ---- (1) File メニュー (Workspace::DrawMenuBar の前に push する) ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、本 sample 専用の File メニューを Workspace の Window/
    // Layout メニューと並べて表示できる。
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Save Tilemap")) {
                // Phase 22: 実 serializer は未配線。callback hook だけ走らせる。
                ACS_LOG_INFO("[LevelEditor] Save Tilemap -> '%s' (stub, no-op: %ux%u, %u layer)",
                             kSavePath, _tilemap.Width(), _tilemap.Height(),
                             _tilemap.LayerCount());
            }
            if (ImGui::MenuItem("Load Tilemap")) {
                ACS_LOG_INFO("[LevelEditor] Load Tilemap <- '%s' (stub, no-op)", kSavePath);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) {
                GetGame().Quit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Brush")) {
            const leveledit::EBrushKind cur = _level_panel.CurrentBrush();
            if (ImGui::MenuItem("Paint", "1", cur == leveledit::EBrushKind::Paint)) {
                _level_panel.SetCurrentBrush(leveledit::EBrushKind::Paint);
            }
            if (ImGui::MenuItem("Erase", "2", cur == leveledit::EBrushKind::Erase)) {
                _level_panel.SetCurrentBrush(leveledit::EBrushKind::Erase);
            }
            if (ImGui::MenuItem("Fill", "3", cur == leveledit::EBrushKind::Fill)) {
                _level_panel.SetCurrentBrush(leveledit::EBrushKind::Fill);
            }
            if (ImGui::MenuItem("Pick", "4", cur == leveledit::EBrushKind::Pick)) {
                _level_panel.SetCurrentBrush(leveledit::EBrushKind::Pick);
            }
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    // ---- (2) Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar →
    //         各 panel DrawUI を順に発火) ----
    _workspace.TickAllPanels(GetGame().DeltaTime());
}

// ============================================================================
// LevelEditorGame — ImGui lifecycle を Game に持たせる薄いラッパ (sample 29/30/31 と同形)
// ============================================================================
class LevelEditorGame : public Game {
public:
    void OnStart() noexcept override {
        if (auto r = _imgui.Init(GetWindow(), GetRenderer()); r.IsErr()) {
            ACS_LOG_ERROR("[LevelEditorGame] ImGuiCtx.Init failed -> Quit");
            Quit();
            return;
        }
        Game::OnStart();
    }

    void OnRender() noexcept override {
        _imgui.NewFrame();
        Game::OnRender();
        _imgui.Render();
    }

    void OnShutdown() noexcept override {
        Game::OnShutdown();
        _imgui.Shutdown();
    }

    void OnEvent(const Event& e) noexcept override {
        _imgui.OnEvent(e);
        Game::OnEvent(e);
    }

protected:
    UniquePtr<Scene> InitialScene() noexcept override {
        return MakeUnique<LevelEditorScene>();
    }

private:
    ImGuiCtx _imgui;
};

ACS_GAME_MAIN(LevelEditorGame)
