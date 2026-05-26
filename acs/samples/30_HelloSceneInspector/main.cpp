// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — エントリポイント。
//
// 動作:
//   ・GameFramework Pillar A の Scene 上に複数階層の Node2D ツリーを構築:
//        root
//        ├── wheel        (回転、SpokeBase 親)
//        │   ├── spoke[0] (オフセット)
//        │   └── spoke[1] (オフセット)
//        └── player       (IInspectableProvider 実装、Inspector で field 編集)
//   ・`inspector::HierarchyPanel _hierarchy_panel` が "Scene Hierarchy" window で
//     ツリーを描画。クリックで `inspector::SelectionService _selection` に選択を
//     伝播。
//   ・`inspector::InspectorPanel _inspector_panel` が "Inspector" window で
//     `_seam` 経由で選択 Node の Provider field を編集。
//   ・`inspector::EditorToolbar _toolbar` が "Editor Toolbar" window で
//     Play/Pause/Step 等のコマンドを発行。
//   ・main menu bar "File > Save Scene / Load Scene" で永続化フックを呼び出す
//     (Phase 20 では callback だけ走らせる stub)。
//   ・Esc で終了。
//
// 構成:
//   main.cpp                      - ACS_GAME_MAIN(SceneInspectorApp) のみ
//   SceneNodes.h                  - PlayerNode (Provider 実装) + WheelNode
//   SceneInspectorScene.{h,cpp}   - Node2D ツリー + 4 panel + Inspector seam を持つ Scene
//   SceneInspectorApp.{h,cpp}     - Game 派生クラス (ImGui lifecycle ラッパ)
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui / 29_HelloParticleEditor
// と同じ理由で、ImGuiCtx が DX12 raw backend 経由のため)。
#include "SceneInspectorApp.h"

ACS_GAME_MAIN(helloscene::SceneInspectorApp)
