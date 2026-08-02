// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Scene。no-code BT エディタの構成 + render を担当。
//
// 構成:
//   - FBtBlackboard (エディタ所有の動的ブラックボード) を状態ストアとして所有。
//     変数 (see_phase / health …) を名前+型で持ち、Action/Condition 関数は
//     `static_cast<FBtBlackboard*>(blackboard)` から名前アクセスする。エディタの
//     Blackboard パネルで変数を追加 / リネーム / 値編集できる。
//   - ABehaviorTreeEditorPanel を所有 + メタミラー node 構造を AddNode で組む。
//   - CBtActionRegistry / CBtConditionRegistry にタスク関数 / 条件関数を名前登録し、
//     registry 実行 (graph-run) でエディタのグラフを直接インタプリトして走らせる。
//   Action/Condition Fn と構築 helper は TreeActions.{h,cpp} / TreeBuilder.{h,cpp} に分離。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/BehaviorTree.h"
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"

namespace hellobt {

// blackboard 型のエイリアス (Action/Condition Fn が void* から restore する型)。
using Blackboard = acs::game::btedit::FBtBlackboard;

// ============================================================================
// BtEditorScene — no-code BT エディタの構成 + render
// ============================================================================
class ABtEditorScene : public acs::game::AScene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    acs::game::CBehaviorTree                            m_Bt;   // 参考用 (graph-run 時は未使用)
    acs::game::btedit::ABehaviorTreeEditorPanel         m_Panel;

    // エディタ所有の動的ブラックボード。状態 (see_phase / health …) を名前+型で保持し、
    // graph blackboard としても渡す。エディタから変数を追加/リネーム/値編集できる。
    Blackboard                                          m_Bb;

    // no-code 実行: Task/Action 名 → 関数ポインタ。registry 実行でエディタのグラフを
    // 直接インタプリトして走らせ、実行フローがライブで光る。
    acs::game::btedit::CBtActionRegistry                m_Reg;

    // Condition デコレーター用: 条件名 → bool 関数 (CanSeePlayer)。
    acs::game::btedit::CBtConditionRegistry             m_Cond;
};

} // namespace hellobt
