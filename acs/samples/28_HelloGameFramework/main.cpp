// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — エントリポイント。
//
// 動作 (3 シーン):
//   Title (青、FSM で 2s 毎に明滅) ─ Space ─→ Gameplay (緑、FTween で呼吸)
//                                      ←──── Backspace
//   Gameplay ─ P ─→ Pause (薄灰、FSequence でログを定期出力) ─ P ─→ Gameplay
//   Esc でいつでも終了
//
// 構成:
//   PlayerProfile.h         - AppState (シーン跨ぎ永続状態) のサンプル struct
//   ARotateComponent.{h,cpp} - composition 版: プレーン ANode に attach する回転
//   ARotatingNode.{h,cpp}    - 継承版: ANode サブクラスで毎フレーム回転
//   TitleScene.{h,cpp}       - Title 画面 (TStateMachine Idle/Blink で 2s 毎に明滅)
//   GameplayScene.{h,cpp}    - 本編 (ANode ツリー + FTween + CCamera + Physics)
//   PauseScene.{h,cpp}       - 一時停止 overlay (FSequence Loop でログを定期出力)
//   HelloGfApp.{h,cpp}       - CGame 派生クラス (AppState 構築、InitialScene 提供)
//
// このサンプルが網羅する GameFramework の主要機能:
//   AppState + 固定 step + ChangeScene/PushScene/PopScene
//   CSceneClock + CTweenManager + Easing
//   TStateMachine<Owner> + FSequence + CSequenceRunner
//   FTransform2D + ANode ツリー (root → wheel → spoke)
//   FInputMap (キー binding + 1D axis)
//   AComponent (ARotateComponent attach、継承パターンとの対比)
//   CSceneServices ハブ (Default2D を WantedServices で宣言)
//   CCamera2D follow + screen shake (trauma)
//   CCollisionWorld2D + SpatialGrid (AddCircle / OverlapAabb / Raycast)
//   APhysicsBody2D (重力で落下する ball + 静的 ground)
#include "HelloGfApp.h"

ACS_GAME_MAIN(hellogf::CHelloGfApp)
