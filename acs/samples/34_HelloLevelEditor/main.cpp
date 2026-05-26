// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — エントリポイント。
//
// 構成:
//   main.cpp                - ACS_GAME_MAIN(LevelEditorApp) のみ
//   LevelEditorApp.{h,cpp}  - Game 派生クラス (ImGui lifecycle ラッパ)
//   LevelEditorScene.{h,cpp}- Workspace + LevelEditorPanel + Tilemap を持つ Scene
//
// 動作:
//   ・GameFramework Pillar Q の `acs::game::Tilemap` (32x32, 2 layer,
//     tile_size=16) を 1 個立て、「壁・床」初期パターンを焼く。
//   ・`editor_core::EditorWorkspace` + `editor_core::EditorTheme` と、
//     `leveledit::LevelEditorPanel` を統合した 1 ウィンドウのエディタ UI。
//   ・LevelEditorPanel が tilemap を ImDrawList で矩形描画し、Paint / Erase /
//     Fill / Pick の 4 ブラシでマウス編集できる。
//   ・main menu bar "File > Save / Load Tilemap" は stub callback
//     (実シリアライザ未配線、ACS_LOG_INFO だけ出す)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (ImGuiCtx が DX12 raw backend 経由のため)。
#include "LevelEditorApp.h"

ACS_GAME_MAIN(hellole::LevelEditorApp)
