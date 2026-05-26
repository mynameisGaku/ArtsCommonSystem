// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — エントリポイント。
//
// 構成:
//   AnimCurveApp.{h,cpp}   - Game 派生クラス (ImGui lifecycle ラッパ)
//   AnimCurveScene.{h,cpp} - Workspace + AnimCurveEditorPanel +
//                            AnimationCurve (Hermite 3 key) を持つ Scene
//
// 動作:
//   ・editor_core (Phase 21a) の EditorWorkspace + EditorTheme と、
//     Phase 22 で実装した animcurve/AnimCurveEditorPanel を 1 個の Workspace
//     に集約。
//   ・サンプル初期化時に 3 個の Hermite key を持つ AnimationCurve を生成し、
//     panel に bind。ユーザは ImGui canvas 上で key の drag / tangent 編集 /
//     右クリック追加 / 削除を対話的に行える。
//   ・MainMenuBar:
//       File > Save curve / Load curve / Quit
//     Save / Load は stub (ACS_LOG_INFO のみ、ファイル I/O は将来 Phase で
//     AnimationCurve のシリアライザを追加した時点で配線)。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (samples/21/29/30/31 と同じ理由で、
//                  ImGuiCtx が DX12 raw backend 経由のため)。
//
// ACS_GAME_MAIN は AnimCurveApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "AnimCurveApp.h"

ACS_GAME_MAIN(helloac::AnimCurveApp)
