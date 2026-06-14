// SPDX-License-Identifier: Apache-2.0
// GameFramework — FFollow2DComponent (オブジェクト参照プロパティのデモ + 実用コンポーネント)
//
// 「別のノードを参照して、そこへ向かって追従する」コンポーネント。インスペクタの
// «オブジェクト参照» プロパティ(他オブジェクトへの参照を渡す UE 風 UPROPERTY)の実例。
// target は参照先ノードの «安定 ID»(エディタ id)を保持する。実行時に id → 生きた
// FNode2D* へ解決して追従する(解決は Phase 2b でローダ側に実装)。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Component2D.h"

namespace acs::game {

/**
 * 参照したノードへ追従する 2D コンポーネント(オブジェクト参照プロパティのデモ)。
 *
 * @details
 * public メンバ + 単一継承なので ACS_RFIELD_REF / ACS_RFIELD_D で offset 反射でき、
 * authored 値(target ID / speed)が実体へ apply される(Play/standalone でも一致)。
 */
class FFollow2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FFollow2DComponent)

    /** 追従先ノードの安定 ID(-1 = «なし»)。エディタの «オブジェクト参照» ピッカーで設定。 */
    i32 target = -1;

    /** 追従速度(units/sec)。 */
    f32 speed  = 3.0f;

    // NOTE: 実際の追従(target ID → 生きた FNode2D* の解決 + 移動)は Phase 2b。
};

} // namespace acs::game
