// SPDX-License-Identifier: Apache-2.0
// HelloBehaviorTreeEditor — Tree 構築 helper の宣言。
//
// 「panel に教えるメタミラー node 構造」と「実 BT (ABtSelector / ABtSequence /
// ABtAction) の構築」を 1:1 対応で組む責務を Scene 本体から分離する。Scene は
// lifecycle に集中し、構築のディテールはここに閉じ込める。
#pragma once

#include "BtEditorScene.h"

namespace hellobt {

// panel にメタミラー (Selector / Condition / Compare / Task …) を AddNode で組み、
// root ノードの id を返す。Compare デコレーターは動的ブラックボードの変数名を参照する。
acs::u32 BuildPanelMirror(acs::game::btedit::ABehaviorTreeEditorPanel& panel) noexcept;

// コードで BT を組む参考実装 (graph-run に置き換えたため現サンプルでは未使用)。
void BuildBehaviorTree(acs::game::CBehaviorTree& bt) noexcept;

} // namespace hellobt
