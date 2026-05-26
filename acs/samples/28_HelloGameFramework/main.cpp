// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — エントリポイント。
//
// 動作 (3 シーン):
//   Title (青、FSM で 2s 毎に明滅) ─ Space ─→ Gameplay (緑、Tween で呼吸)
//                                      ←──── Backspace
//   Gameplay ─ P ─→ Pause (薄灰、FSequence でログを定期出力) ─ P ─→ Gameplay
//   Esc でいつでも終了
//
// 構成:
//   PlayerProfile.h         - AppState (シーン跨ぎ永続状態) のサンプル struct
//   RotateComponent.{h,cpp} - composition 版: プレーン FNode2D に attach する回転
//   RotatingNode.{h,cpp}    - 継承版: FNode2D サブクラスで毎フレーム回転
//   TitleScene.{h,cpp}      - Title 画面 (FStateMachine Idle/Blink で 2s 毎に明滅)
//   GameplayScene.{h,cpp}   - 本編 (FNode2D ツリー + Tween + FCamera + Physics)
//   PauseScene.{h,cpp}      - 一時停止 overlay (FSequence Loop でログを定期出力)
//   HelloGfApp.{h,cpp}      - FGame 派生クラス (AppState 構築、InitialScene 提供)
//
// このサンプルが網羅する GameFramework の主要機能:
//   AppState + 固定 step + ChangeScene/PushScene/PopScene
//   FSceneClock + FTweenManager + Easing
//   FStateMachine<Owner> + FSequence + FSequenceRunner
//   FTransform2D + FNode2D ツリー (root → wheel → spoke)
//   FInputMap (キー binding + 1D axis)
//   FComponent2D (RotateComponent attach、継承パターンとの対比)
//   FSceneServices ハブ (Default2D を WantedServices で宣言)
//   FCamera2D follow + screen shake (trauma)
//   FCollisionWorld2D + SpatialGrid (AddCircle / OverlapAabb / Raycast)
//   FPhysicsBody2D (重力で落下する ball + 静的 ground)
#include "HelloGfApp.h"

ACS_GAME_MAIN(hellogf::HelloGfApp)
