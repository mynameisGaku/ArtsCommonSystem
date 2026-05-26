// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Action Fn 群 + step callback の宣言。
//
// Action Fn は `acs::game::BtAction::Fn`
// (= EBtStatus(*)(void*, f32) noexcept) のシグネチャに合わせる。実 BT の
// `BtAction` から呼ばれ、blackboard を `BtEditorBb*` に restore して
// panel.SetNodeStatus に status を push する。
//
// Tree 構築側 (TreeBuilder) と Scene 側 (BtEditorScene) の両方から関数
// ポインタとして参照されるので、宣言を共有ヘッダに分離する。
#pragma once

#include "BtEditorScene.h"

namespace hellobt {

// Action Fn 群 — 全て BtAction::Fn シグネチャ。
// blackboard は BtEditorBb* にキャストして使う。
acs::game::EBtStatus ActionPickup(void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionMove  (void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionWait  (void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionAttack(void* blackboard, acs::f32 dt) noexcept;

// panel.SetOnStepCallback に渡す callback。blackboard を渡して 1 tick 実行。
void StepCallbackFn(void* user, acs::game::BehaviorTree* tree, acs::f32 dt) noexcept;

} // namespace hellobt
