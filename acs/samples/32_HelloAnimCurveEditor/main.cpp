// SPDX-License-Identifier: Apache-2.0
// HelloAnimCurveEditor — エントリポイント。
//
// 構成:
//   AnimCurveApp.{h,cpp}   - FGame 派生クラス (ImGui lifecycle ラッパ)
//   AnimCurveScene.{h,cpp} - Workspace + FAnimCurveEditorPanel +
//                            FAnimationCurve (Hermite 3 key) を持つ Scene
//
// 動作:
//   ・editor_core の FEditorWorkspace + FEditorTheme と animcurve の
//     FAnimCurveEditorPanel を 1 個の Workspace に集約。
//   ・初期化時に 3 個の Hermite key を持つ FAnimationCurve を生成し panel に
//     bind。ImGui canvas 上で key の drag / tangent 編集 / 右クリック追加 /
//     削除を対話的に行える。
//   ・MainMenuBar:
//       File > Save Curve / Load Curve / Quit
//     Save / Load は stub (ACS_LOG_INFO のみ)。実 I/O は FAnimationCurve の
//     シリアライザが揃ったタイミングで配線する。
//   ・Esc で終了。
//
// 必須バックエンド: ACS_RENDER_DX12_RAW (ImGuiCtx が DX12 raw backend 経由
// のため)。
#include "AnimCurveApp.h"

ACS_GAME_MAIN(helloac::AnimCurveApp)
