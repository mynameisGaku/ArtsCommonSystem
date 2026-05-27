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
// OnEnter — theme / workspace / tilemap / panel を順に初期化
// ----------------------------------------------------------------------------
void LevelEditorScene::OnEnter() noexcept {
    // editor 風の暗グレー背景 (ImGui に隠れるが viewport の外側余白に出る)。
    GetGame().SetClearColor(0.15f, 0.15f, 0.18f);

    // ---- Theme: ImGui context 取得後に Init (= Dark preset) ----
    m_Theme.Init();
    m_Theme.ApplyPreset(editor_core::EEditorThemePreset::Dark);

    // ---- Workspace 本体 ----
    m_Workspace.Init();

    // ---- FTilemap: 32x32 / 2 layer / tile_size=16 ----
    m_Tilemap.Init(/*width=*/32u, /*height=*/32u,
                  /*layer_count=*/2u, /*tile_size=*/16.0f);
    m_SeedInitialTilemapPattern();

    // ---- FLevelEditorPanel に tilemap を渡して Workspace に登録 ----
    m_LevelPanel.Init();
    m_LevelPanel.SetTilemap(&m_Tilemap);
    // editor 開始時は壁 (tile id=10) をスポイトしやすいよう Paint + active layer=1
    // の状態にしておく (= 触り始めに「壁を増やす」操作が直感的)。
    m_LevelPanel.SetActiveLayer(1u);
    m_LevelPanel.SetCurrentBrush(leveledit::EBrushKind::Paint);
    m_LevelPanel.SetCurrentTileId(10u);
    // FEditorWorkspace::RegisterPanel は内部で panel->OnInit(*this) を呼ぶ。
    // よって panel.OnInit を別途呼ぶ必要は無い。
    m_Workspace.RegisterPanel(&m_LevelPanel);

    // 初期視点を tilemap 全体が画面に収まる位置へ。
    m_LevelPanel.Camera().FrameToBoundingBox2D(
        FVec2{0.0f, 0.0f},
        FVec2{static_cast<f32>(m_Tilemap.Width())  * m_Tilemap.TileSize(),
             static_cast<f32>(m_Tilemap.Height()) * m_Tilemap.TileSize()});

    ACS_LOG_INFO("[LevelEditor] entered (tilemap 32x32 / 2 layer / tile_size=16, 4 ブラシ + workspace + theme)");
}

// ----------------------------------------------------------------------------
// 初期 tilemap パターンを焼く (床 + 外周壁 + 中央部屋の枠)
// ----------------------------------------------------------------------------
void LevelEditorScene::m_SeedInitialTilemapPattern() noexcept {
    // layer 0 = 床 (全面 tile id=1)
    m_Tilemap.Fill(FTileId{1u}, /*layer=*/0u);

    // layer 1 = 壁。FillRect は (x0,y0,x1,y1) の矩形を tile で埋める
    // (x0/y0 == x1/y1 で線分扱い)。

    // 外周 (4 辺) を tile id=10 で囲む
    m_Tilemap.FillRect(0u,  0u,  31u, 0u,  FTileId{10u}, /*layer=*/1u);    // 下
    m_Tilemap.FillRect(0u,  31u, 31u, 31u, FTileId{10u}, /*layer=*/1u);    // 上
    m_Tilemap.FillRect(0u,  0u,  0u,  31u, FTileId{10u}, /*layer=*/1u);    // 左
    m_Tilemap.FillRect(31u, 0u,  31u, 31u, FTileId{10u}, /*layer=*/1u);    // 右

    // 中央部屋 (8..23, 8..23) の枠を tile id=20 で囲む
    m_Tilemap.FillRect(8u,  8u,  23u, 8u,  FTileId{20u}, /*layer=*/1u);    // 下辺
    m_Tilemap.FillRect(8u,  23u, 23u, 23u, FTileId{20u}, /*layer=*/1u);    // 上辺
    m_Tilemap.FillRect(8u,  8u,  8u,  23u, FTileId{20u}, /*layer=*/1u);    // 左辺
    m_Tilemap.FillRect(23u, 8u,  23u, 23u, FTileId{20u}, /*layer=*/1u);    // 右辺
}

// ----------------------------------------------------------------------------
// OnExit — 逆順 shutdown
// ----------------------------------------------------------------------------
void LevelEditorScene::OnExit() noexcept {
    // FEditorWorkspace::Shutdown は登録済み全 panel に OnShutdown を 1 度ずつ
    // 呼んでから list を Clear する。よって個別 UnregisterPanel を呼ぶ必要は無い。
    m_Workspace.Shutdown();
    m_LevelPanel.Shutdown();
    // FEditorTheme::Shutdown は存在しない API なので明示解放は不要 (Dtor で十分)。
    ACS_LOG_INFO("[LevelEditor] exited");
}

// ----------------------------------------------------------------------------
// OnUpdate — 非 ImGui 系のロジック (Esc 終了のみ)
// ----------------------------------------------------------------------------
// ImGui 関連 (Workspace::TickAllPanels が呼ぶ DrawDockSpace / MenuBar / panel
// DrawUI) はすべて OnRender 側へ。`ImGui::Begin` 等は NewFrame() と Render()
// の間でしか呼べないため、ここでは Workspace::TickAllPanels は呼ばない。
void LevelEditorScene::OnUpdate(f32 dt) noexcept {
    (void)dt;
    if (Input::IsKeyPressed(EKey::Escape)) {
        GetGame().Quit();
        return;
    }
}

// ----------------------------------------------------------------------------
// OnRender — File/Brush メニュー → Workspace 全描画
// ----------------------------------------------------------------------------
void LevelEditorScene::OnRender(RenderContext& rc) noexcept {
    (void)rc;

    // ---- (1) MainMenuBar に本 sample 専用メニューを push ----
    // ImGui は同一フレーム内で BeginMainMenuBar を複数回呼んでも 1 個の bar に
    // マージするので、ここで File / Brush を出しておけば Workspace 側が出す
    // FWindow / Layout メニューと並ぶ。
    if (ImGui::BeginMainMenuBar()) {
        m_DrawFileMenu();
        m_DrawBrushMenu();
        ImGui::EndMainMenuBar();
    }

    // ---- (2) Workspace 全描画 (1 行で OnFrameBegin → DockSpace → MenuBar →
    //         各 panel DrawUI を順に発火) ----
    m_Workspace.TickAllPanels(GetGame().DeltaTime());
}

// ----------------------------------------------------------------------------
// File メニュー (Save / Load / Quit)。Save/Load は stub。
// ----------------------------------------------------------------------------
void LevelEditorScene::m_DrawFileMenu() noexcept {
    if (!ImGui::BeginMenu("File")) {
        return;
    }
    if (ImGui::MenuItem("Save FTilemap")) {
        // 実 serializer は未配線。callback hook だけ走らせる stub。
        ACS_LOG_INFO("[LevelEditor] Save FTilemap -> '%s' (stub, no-op: %ux%u, %u layer)",
                     kSavePath, m_Tilemap.Width(), m_Tilemap.Height(),
                     m_Tilemap.LayerCount());
    }
    if (ImGui::MenuItem("Load FTilemap")) {
        ACS_LOG_INFO("[LevelEditor] Load FTilemap <- '%s' (stub, no-op)", kSavePath);
    }
    ImGui::Separator();
    if (ImGui::MenuItem("Quit", "Esc")) {
        GetGame().Quit();
    }
    ImGui::EndMenu();
}

// ----------------------------------------------------------------------------
// Brush メニュー (Paint / Erase / Fill / Pick の選択 UI)。
// ----------------------------------------------------------------------------
void LevelEditorScene::m_DrawBrushMenu() noexcept {
    if (!ImGui::BeginMenu("Brush")) {
        return;
    }
    const leveledit::EBrushKind cur = m_LevelPanel.CurrentBrush();
    if (ImGui::MenuItem("Paint", "1", cur == leveledit::EBrushKind::Paint)) {
        m_LevelPanel.SetCurrentBrush(leveledit::EBrushKind::Paint);
    }
    if (ImGui::MenuItem("Erase", "2", cur == leveledit::EBrushKind::Erase)) {
        m_LevelPanel.SetCurrentBrush(leveledit::EBrushKind::Erase);
    }
    if (ImGui::MenuItem("Fill", "3", cur == leveledit::EBrushKind::Fill)) {
        m_LevelPanel.SetCurrentBrush(leveledit::EBrushKind::Fill);
    }
    if (ImGui::MenuItem("Pick", "4", cur == leveledit::EBrushKind::Pick)) {
        m_LevelPanel.SetCurrentBrush(leveledit::EBrushKind::Pick);
    }
    ImGui::EndMenu();
}

} // namespace hellole
