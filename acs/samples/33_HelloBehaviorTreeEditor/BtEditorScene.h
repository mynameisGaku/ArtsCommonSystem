// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — FScene。BT 構築 + panel 構成 + render を担当。
//
// 構成:
//   - FBehaviorTree 本体を所有 (Selector{Seq{Pickup,Move},Seq{Wait,Attack}})。
//   - FBehaviorTreeEditorPanel を所有 + メタミラー node 構造を AddNode で組む。
//   - BtEditorBb (= panel + per-action 進捗カウンタ) を所有し、Action Fn から
//     `static_cast<BtEditorBb*>(blackboard)` で参照される。Action Fn と
//     構築 helper は TreeActions.{h,cpp} / TreeBuilder.{h,cpp} に分離。
#pragma once

#include "gameframework/GameFramework.h"
#include "gameframework/BehaviorTree.h"
#include "gameframework/tools/btedit/BehaviorTreeEditorPanel.h"

namespace hellobt {

// ============================================================================
// Blackboard — Action 関数から panel への双方向参照 + 進捗カウンタを持つ
// ============================================================================
// FBehaviorTree::Tick(&bb, dt) で渡され、各 Action Fn の中で
// `static_cast<BtEditorBb*>(blackboard)` で復元される。Fn 1 つにつき 1 個の
// 進捗カウンタを持ち、N frame Running した後に Success/Failure に遷移する
// 「タイマ式 stub action」として動作させる。
// ----------------------------------------------------------------------------
struct BtEditorBb {
    acs::game::btedit::FBehaviorTreeEditorPanel* panel = nullptr;

    // Action 関数が panel.SetNodeStatus に渡す node_id (sample が AddNode で
    // 払い出した値をそのまま保持)。
    acs::u32 id_pickup = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId;
    acs::u32 id_move   = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId;
    acs::u32 id_wait   = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId;
    acs::u32 id_attack = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId;

    // root / composite の id (= Selector / FSequence)。panel 側で root の status
    // を履歴に push するため、composite の status も Fn から push しないと
    // composite 自体は灰色 (Failure 初期値) のままになる。
    // 各 Action から composite の id を辿るのは FBehaviorTree の API では出来ない
    // ので、Bb に持たせる + Action Fn の最後に "親 composite の status も上書き"
    // する戦略にする (= sample 側のアドホック合成)。
    acs::u32 id_root     = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId; // Selector
    acs::u32 id_branch_a = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId; // FSequence "Pickup Branch"
    acs::u32 id_branch_b = acs::game::btedit::FBehaviorTreeEditorPanel::kInvalidId; // FSequence "Combat Branch"

    // 進捗カウンタ (Running 中の経過 frame 数)。Action 毎に独立。
    // Reset Tree メニュー or panel.Reset() に合わせて 0 に戻す。
    acs::u32 counter_move = 0;
    acs::u32 counter_wait = 0;

    // タイマ閾値 (frame 単位、kStepDt * 30 ≈ 0.48 sec)。
    static constexpr acs::u32 kMoveRunFrames = 30u;
    static constexpr acs::u32 kWaitRunFrames = 30u;
};

// ============================================================================
// BtEditorScene — BT 構築 + panel 構成 + render
// ============================================================================
class BtEditorScene : public acs::game::FScene {
public:
    void OnEnter() noexcept override;
    void OnExit() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

private:
    acs::game::FBehaviorTree                            _bt;
    BtEditorBb                                         _bb;
    acs::game::btedit::FBehaviorTreeEditorPanel         _panel;
};

} // namespace hellobt
