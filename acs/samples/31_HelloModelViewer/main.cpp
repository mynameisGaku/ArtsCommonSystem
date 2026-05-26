// SPDX-License-Identifier: Apache-2.0
// HelloModelViewer — エントリポイント。3D Model Viewer エディタ。
//
// 構成 (機能別分割):
//   main.cpp                      - ACS_GAME_MAIN(ModelViewerApp) のみ
//   ModelViewerApp.{h,cpp}        - Game 派生クラス (ImGui lifecycle ラッパ)
//   ModelViewerScene.{h,cpp}      - 薄いオーケストレータ (3 サブモジュールを束ねる)
//   ViewerScenePipeline.{h,cpp}   - 3D RHI リソース所有 / MVP 更新 / draw call 発行
//   ViewerPanels.{h,cpp}          - workspace + asset browser + 4 panel + theme
//   ViewerMenuBar.{h,cpp}         - File メニュー + Theme クイックアクション窓
//   ViewerCubeAssets.h            - cube 頂点 / インデックス / HLSL (constexpr 定数)
//
// 動作:
//   ・editor_core の EditorWorkspace / AssetBrowser / EditorTheme と、modelview/
//     配下 4 panel (ModelViewerPanel / ModelInspectorPanel / ModelMaterialPanel /
//     ModelAnimationPanel) を 1 個の Workspace に集約。
//   ・サンプル 17_HelloMesh と同等の "頂点+色 cube" を回転表示する 3D viewport を
//     ImGui の外側 (= フレームバッファ直書き) に同時描画。viewport カメラは
//     `ModelViewerPanel::FCamera()` (= editor_core::EditorCamera) の view/proj を
//     使用するため、エディタ上でマウスドラッグ → orbit / dolly がそのまま 3D 像
//     に反映される (panel 側の HandleMouseInput が ImGui の IO を吸い上げる前提)。
//   ・MainMenuBar:
//       File > Open Model.../Save Layout/Load Layout/Theme.../Quit
//     File 系は stub callback (実 dialog/serializer は未配線)。
//     Layout は editor_core::EditorWorkspace::SaveLayout/LoadLayout を呼ぶ。
//     Theme は EditorTheme::DrawThemeSettingsUI() を toggle で開閉。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (ImGuiCtx が DX12 raw backend 経由のため)。
//
// 範囲外 (本 sample では持たない):
//   ・実 model loader (glTF / FBX) — File > Open Model... は callback だけ stub。
//   ・animation frame 適用 — ModelAnimationPanel::Tick は時間だけ進める。
//   ・gizmo 操作 (translate/rotate/scale)。
//   ・複数モデルの同時表示 (現状 1 cube 固定)。
#include "ModelViewerApp.h"

ACS_GAME_MAIN(hellomv::ModelViewerApp)
