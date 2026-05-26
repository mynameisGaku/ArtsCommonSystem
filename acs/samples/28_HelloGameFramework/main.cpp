// SPDX-License-Identifier: Apache-2.0
// HelloGameFramework — エントリポイント。
//
// 動作 (3 シーン):
//   Title (青、FSM で 2s 毎に明滅) ─ Space ─→ Gameplay (緑、Tween で呼吸)
//                                      ←──── Backspace
//   Gameplay ─ P ─→ Pause (薄灰、Sequence でログを定期出力) ─ P ─→ Gameplay
//   Esc でいつでも終了
//
// 構成:
//   GameTypes.h         - PlayerProfile (AppState) + RotateComponent + RotatingNode
//   TitleScene.{h,cpp}  - Title 画面 (StateMachine Idle/Blink で 2s 毎に明滅)
//   GameplayScene.{h,cpp} - 本編 (Node2D ツリー + Tween + Camera + Physics)
//   PauseScene.{h,cpp}  - 一時停止 overlay (Sequence Loop でログを定期出力)
//   HelloGfApp.{h,cpp}  - Game 派生クラス (AppState 構築、InitialScene 提供)
//
// Phase 2-11 で示す機能 (詳細は HelloGfApp / Scene 群のコメント参照):
//   Phase 2 : AppState (PlayerProfile) + 固定 step + ChangeScene/PushScene/PopScene
//   Phase 3 : SceneClock + TweenManager + Easing
//   Phase 4 : StateMachine<Owner> + Sequence + SequenceRunner
//   Phase 5 : Transform2D + Node2D ツリー (root → wheel → spoke)
//   Phase 6 : InputMap (キー binding + 1D axis)
//   Phase 7 : Component2D (RotateComponent attach、継承パターンとの対比)
//   Phase 8 : SceneServices ハブ (Default2D を WantedServices で宣言)
//   Phase 9 : Camera2D follow + screen shake (trauma)
//   Phase 10: CollisionWorld2D + SpatialGrid (AddCircle / OverlapAabb / Raycast)
//   Phase 11: PhysicsBody2D (重力で落下する ball + 静的 ground)
#include "HelloGfApp.h"

ACS_GAME_MAIN(hellogf::HelloGfApp)
