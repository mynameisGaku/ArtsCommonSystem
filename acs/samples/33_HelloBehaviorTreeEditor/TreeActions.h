// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Task/Action Fn 群 + Condition Fn の宣言。
//
// Action Fn は `acs::game::FBtAction::Fn` (= EBtStatus(*)(void*, f32) noexcept)、
// Condition Fn は `FBtConditionRegistry::Fn` (= bool(*)(void*) noexcept) のシグネチャ。
// blackboard は `hellobt::Blackboard*` (= FBtBlackboard*) に restore し、変数は名前で
// Get/Set してアクセスする。Tree 構築側 (TreeBuilder) と Scene 側から関数ポインタとして
// 参照されるので宣言を共有ヘッダに分離する。
#pragma once

#include "BtEditorScene.h"

namespace hellobt {

// Task/Action Fn 群 — registry に名前登録される。
acs::game::EBtStatus ActionAttack(void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionWait  (void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionMove  (void* blackboard, acs::f32 dt) noexcept;
acs::game::EBtStatus ActionPickup(void* blackboard, acs::f32 dt) noexcept;

// Condition デコレーター用の条件関数 (bool(*)(void*) noexcept)。
// blackboard 変数 "see_phase" を毎 tick 進めつつ、前半 (<60) で true を返す。
bool CanSeePlayer(void* blackboard) noexcept;

} // namespace hellobt
