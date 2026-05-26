// SPDX-License-Identifier: Apache-2.0
// HelloSceneInspector — エントリポイント。
//
// 動作:
//   ・GameFramework Pillar A の Scene 上に複数階層の Node2D ツリーを構築:
//        root
//        ├── wheel        (自転、spoke の親)
//        │   ├── spoke[0] (オフセット)
//        │   └── spoke[1] (オフセット)
//        └── player       (IInspectableProvider 実装、Inspector で field 編集)
//   ・`HierarchyPanel` が "Scene Hierarchy" window でツリーを描画。クリックで
//     `SelectionService` に選択を伝播。
//   ・`InspectorPanel` が "Inspector" window で `InspectorSeam` 経由で
//     選択 Node の Provider field を編集。
//   ・`EditorToolbar` が "Editor Toolbar" window で Play/Pause/Step 等の
//     コマンドを発行。
//   ・main menu bar "File > Save Scene / Load Scene" は永続化フックを呼び出す
//     stub (本サンプルでは callback 走らせるのみ)。
//   ・Esc で終了。
//
// 構成:
//   main.cpp                      - ACS_GAME_MAIN(SceneInspectorApp) のみ
//   SceneNodes.{h,cpp}            - PlayerNode (Provider 実装) + WheelNode
//   PanelLayout.{h,cpp}           - 4 panel + Selection + Seam の束ね
//   SceneInspectorScene.{h,cpp}   - Node2D ツリー + PanelLayout を持つ Scene
//   SceneInspectorApp.{h,cpp}     - Game 派生クラス (ImGui lifecycle ラッパ)
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21_HelloImGui /
// 29_HelloParticleEditor と同じ理由で、ImGuiCtx が DX12 raw backend 経由のため)。
#include "SceneInspectorApp.h"

ACS_GAME_MAIN(helloscene::SceneInspectorApp)
