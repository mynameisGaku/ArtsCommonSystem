// SPDX-License-Identifier: Apache-2.0
// HelloCinematicsEditor — エントリポイント。
//
// 構成:
//   CineEditorApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
//   CineEditorScene.{h,cpp} - Workspace + CinematicsTimelineEditorPanel +
//                             CinematicsDirector + 初期 keyframe 3 個
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace + EditorTheme と、Phase 23 で
//     実装した cinetimeline/CinematicsTimelineEditorPanel を 1 個の Workspace
//     に集約。
//   ・サンプル初期化時に CinematicsDirector を作成して panel に bind、
//     初期 keyframe を 3 個追加:
//       1) CameraCut       @ 0s
//       2) FadeColor       @ 2s
//       3) TriggerCallback @ 5s
//   ・ユーザは panel 上で Play / Pause / Stop / Step + time scrub +
//     marker drag + Add Keyframe + Inspector 編集を対話的に行える。
//   ・Tick (= Scene::OnUpdate) で panel.Step(dt) を呼び、Play 中なら director
//     が時間進行 + keyframe 発火する (= callback が main 側に紐付いていれば
//     ACS_LOG_INFO で発火を可視化)。
//   ・MainMenuBar:
//       File > Save / Load / Quit (Save/Load は stub)
//     Window / Layout は EditorWorkspace が自前で MenuBar に push する。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31/32 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。CMakeLists.txt 側
//                  および root acs/CMakeLists.txt の `acs_add_sample` 呼出を
//                  ACS_RENDER_DX12_RAW で guard する。
//
// ACS_GAME_MAIN は CineEditorApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "CineEditorApp.h"

ACS_GAME_MAIN(hellocine::CineEditorApp)
