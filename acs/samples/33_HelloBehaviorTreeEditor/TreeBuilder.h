// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Tree 構築 helper の宣言。
//
// 「panel に教えるメタミラー node 構造」と「実 BT (FBtSelector / FBtSequence /
// FBtAction) の構築」を 1:1 対応で組む責務を Scene 本体から分離する。Scene は
// lifecycle に集中し、構築のディテールはここに閉じ込める。
#pragma once

#include "BtEditorScene.h"

namespace hellobt {

// panel に対し AddNode で node id を払い出し、bb 側に保存する。
// AddNode の払い出した id を Action Fn からの panel.SetNodeStatus に使うため、
// 構築フェーズで一括して id を確定させる。
void BuildPanelMirror(acs::game::btedit::FBehaviorTreeEditorPanel& panel,
                      BtEditorBb&                                 bb) noexcept;

// メタミラーと同形の実 BT (Selector{Seq{Pickup,Move},Seq{Wait,Attack}}) を組み、
// bt にセットする。Action Fn の関数ポインタはここで実 BT に bake される。
void BuildBehaviorTree(acs::game::FBehaviorTree& bt) noexcept;

} // namespace hellobt
