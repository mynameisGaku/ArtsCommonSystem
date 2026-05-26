// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — LevelEditorScene 実装。
#include "LevelEditorScene.h"

#include "platform/Input.h"
#include "foundation/Log.h"
#include "math/Vec.h"

#include <imgui.h>

using namespace acs;
using namespace acs::game;

namespace hellole {

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

} // namespace hellole
