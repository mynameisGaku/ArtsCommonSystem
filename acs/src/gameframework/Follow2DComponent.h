// SPDX-License-Identifier: Apache-2.0
// GameFramework — FFollow2DComponent (オブジェクト参照プロパティのデモ + 実用コンポーネント)
//
// 「別のノードを参照して、そこへ向かって追従する」コンポーネント。インスペクタの
// «オブジェクト参照» プロパティ(他オブジェクトへの参照を渡す UE 風 UPROPERTY)の実例。
// target は参照先ノードの «安定 ID»(SerialId = エディタ id)を保持する。実行時に
// id → 生きた FNode2D* へ解決して追従する(解決は FindBySerialId、結果はキャッシュ)。
#pragma once

#include "foundation/Types.h"
#include "gameframework/Component2D.h"
#include "gameframework/Node2D.h"

#include <cmath>

namespace acs::game {

/**
 * 参照したノードへ追従する 2D コンポーネント(オブジェクト参照プロパティのデモ)。
 *
 * @details
 * public メンバ + 単一継承なので ACS_RFIELD_REF / ACS_RFIELD_D で offset 反射でき、
 * authored 値(target ID / speed)が実体へ apply される(Play/standalone でも一致)。
 * OnUpdate で target ID を FNode2D* へ解決し、speed[units/sec] で近づく。
 */
class FFollow2DComponent : public FComponent2D {
public:
    ACS_GAME_COMPONENT_KIND(FFollow2DComponent)

    /** 追従先ノードの安定 ID(-1 = «なし»)。エディタの «オブジェクト参照» ピッカーで設定。 */
    i32 target = -1;

    /** 追従速度(units/sec)。 */
    f32 speed  = 3.0f;

    /** target に到達したか(実行時の «読み取り専用» ステータス = VisibleAnywhere のデモ)。 */
    bool arrived = false;

    void OnUpdate(f32 dt) noexcept override {
        if (target < 0 || speed <= 0.0f || dt <= 0.0f) return;
        if (!HasOwner()) return;
        FNode2D* owner = &Owner();

        FNode2D* tgt = ResolveTarget(owner);
        if (tgt == nullptr || tgt == owner) return;

        // 同階層前提の簡易追従(owner/target が同じ親 = local が比較可能)。
        const FVec2 cur = owner->Local().position;
        const FVec2 dst = tgt->Local().position;
        const FVec2 d{ dst.x - cur.x, dst.y - cur.y };
        const f32 len  = std::sqrt(d.x * d.x + d.y * d.y);
        const f32 step = speed * dt;
        if (len <= step || len < 1e-5f) { owner->Local().position = dst; arrived = true; }   // 到達
        else { owner->Local().position = FVec2{ cur.x + d.x / len * step,
                                                cur.y + d.y / len * step }; arrived = false; }
    }

private:
    /** target ID を root から解決して返す(結果は target 変化までキャッシュ)。 */
    FNode2D* ResolveTarget(FNode2D* owner) noexcept {
        if (m_Cached != nullptr && m_CachedFor == target) return m_Cached;
        FNode2D* root = owner;
        while (root->Parent() != nullptr) root = root->Parent();
        m_Cached    = root->FindBySerialId(target);
        m_CachedFor = target;
        return m_Cached;
    }

    FNode2D* m_Cached    = nullptr;   ///< 解決済み参照先(キャッシュ)。
    i32      m_CachedFor = -2;        ///< キャッシュ時の target(変化検出用、初期値は target と不一致）。
};

} // namespace acs::game
