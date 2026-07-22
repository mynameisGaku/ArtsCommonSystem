// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — エントリポイント。
//
// 構成:
//   main.cpp              - ACS_GAME_MAIN(BtEditorApp) のみ
//   BtEditorApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
//   BtEditorScene.{h,cpp} - Scene lifecycle (OnEnter/OnExit/OnUpdate/OnRender)
//   TreeActions.{h,cpp}   - Action Fn 群 + step callback
//   TreeBuilder.{h,cpp}   - panel メタミラー組立 + 実 BT 構築 helper
//
// 動作:
//   ・小さな Behavior Tree を構築:
//        Selector (root)
//        ├── FSequence "Pickup Branch"
//        │    ├── Action "Pickup"
//        │    └── Action "Move"
//        └── FSequence "Combat Branch"
//             ├── Action "Wait"
//             └── Action "Attack"
//   ・BT そのものは `acs::game::FBehaviorTree` で実行。各 FBtAction の Fn は
//     blackboard 経由で `FBehaviorTreeEditorPanel` の SetNodeStatus を呼び、
//     panel に「この frame でこの node が何を返したか」を push する。
//     これで panel 側のメタミラーがリアルタイムに着色される。
//   ・MainMenuBar: File > Reset Tree / Quit。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (= samples/21/29/30/31 と同じ理由で
//                  ImGuiCtx が DX12 raw backend 経由のため)。
#include "BtEditorApp.h"

// 本 sample は DX12 raw 限定だが、その制約は CMakeLists.txt の
// `if(ACS_RENDER_DX12_RAW)` ガードで担保している (ON のときだけ target を追加)。
// ACS_RENDER_DX12_RAW はコンパイル定義として渡らないため、ソース側の #if ガードは
// 機能せず WIN32 entry (WinMain) が欠落する不具合があった → sample 30 と同じく
// ACS_GAME_MAIN を無条件で使う。
ACS_GAME_MAIN(hellobt::FBtEditorApp)
