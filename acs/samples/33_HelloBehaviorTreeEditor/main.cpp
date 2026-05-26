// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — エントリポイント。
//
// 構成:
//   main.cpp              - ACS_GAME_MAIN(BtEditorApp) のみ
//   BtEditorApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
//   BtEditorScene.{h,cpp} - FScene lifecycle (OnEnter/OnExit/OnUpdate/OnRender)
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
//                  FImGuiCtx が DX12 raw backend 経由のため)。
#include "BtEditorApp.h"

#if defined(ACS_RENDER_DX12_RAW)
ACS_GAME_MAIN(hellobt::BtEditorApp)
#else
// ACS_RENDER_DX12_RAW 未定義時はビルドガード (= FImGuiCtx が dx12 raw 専用)。
// sample 21/29/30/31 と同じ理由で本 sample も DX12 raw 限定。
// CMakeLists.txt でも `if(ACS_RENDER_DX12_RAW)` ガードを掛けている。
int main() { return 0; }
#endif
