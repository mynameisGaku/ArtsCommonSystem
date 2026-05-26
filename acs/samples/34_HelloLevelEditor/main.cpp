// SPDX-License-Identifier: Apache-2.0
// HelloLevelEditor — エントリポイント。
//
// 構成:
//   main.cpp                - ACS_GAME_MAIN(LevelEditorApp) のみ
//   LevelEditorApp.{h,cpp}  - FGame 派生クラス (ImGui lifecycle ラッパ)
//   LevelEditorScene.{h,cpp}- Workspace + FLevelEditorPanel + FTilemap を持つ FScene
//
// 動作:
//   ・GameFramework Pillar Q の `acs::game::FTilemap` (32x32, 2 layer,
//     tile_size=16) を 1 個立て、「壁・床」初期パターンを焼く。
//   ・`editor_core::FEditorWorkspace` + `editor_core::FEditorTheme` と、
//     `leveledit::FLevelEditorPanel` を統合した 1 ウィンドウのエディタ UI。
//   ・FLevelEditorPanel が tilemap を ImDrawList で矩形描画し、Paint / Erase /
//     Fill / Pick の 4 ブラシでマウス編集できる。
//   ・main menu bar "File > Save / Load FTilemap" は stub callback
//     (実シリアライザ未配線、ACS_LOG_INFO だけ出す)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (FImGuiCtx が DX12 raw backend 経由のため)。
#include "LevelEditorApp.h"

ACS_GAME_MAIN(hellole::LevelEditorApp)
