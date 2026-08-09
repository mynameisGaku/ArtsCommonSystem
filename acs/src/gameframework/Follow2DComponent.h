// SPDX-License-Identifier: Apache-2.0
// GameFramework — AFollow2DComponent (オブジェクト参照プロパティのデモ + 実用コンポーネント)
//
// 「別のノードを参照して、そこへ向かって追従する」コンポーネント。インスペクタの
// 他オブジェクトを安定 ID で参照する ACS_PROPERTY の実例。
// target は参照先ノードの «安定 ID»(SerialId = エディタ id)を保持する。実行時に
// id → 生きた ANode* へ解決して追従する(解決は FindBySerialId、結果はキャッシュ)。
#pragma once

#include "foundation/Types.h"
#include "foundation/Log.h"
#include "gameframework/AcsClass.h"   // ACS_FUNCTION マーカー
#include "gameframework/AComponent.h"
#include "gameframework/ANode.h"

#include <cmath>

namespace acs::game {

/**
 * 参照したノードへ追従する 2D コンポーネント(オブジェクト参照プロパティのデモ)。
 *
 * @details
 * public メンバ + 単一継承なので ACS_RFIELD_REF / ACS_RFIELD_D で offset 反射でき、
 * authored 値(target ID / speed)が実体へ apply される(Play/standalone でも一致)。
 * OnUpdate で target ID を ANode* へ解決し、speed[units/sec] で近づく。
 */
class AFollow2DComponent : public AComponent {
public:
    ACS_GAME_COMPONENT_KIND(AFollow2DComponent)

    /** 追従先ノードの安定 ID(-1 = «なし»)。エディタの «オブジェクト参照» ピッカーで設定。 */
    i32 target = -1;

    /** 追従速度(units/sec)。 */
    f32 speed  = 3.0f;

    /** target に到達したか(実行時の «読み取り専用» ステータス = VisibleAnywhere のデモ)。 */
    bool arrived = false;

    void OnUpdate(f32 dt) noexcept override {
        if (target < 0 || speed <= 0.0f || dt <= 0.0f) return;
        if (!HasOwner()) return;
        ANode* owner = &Owner();

        ANode* tgt = ResolveTarget(owner);
        if (tgt == nullptr || tgt == owner) return;

        // 同階層前提の簡易追従(owner/target が同じ親 = local が比較可能)。
        const FVec2 cur = owner->Position2D();
        const FVec2 dst = tgt->Position2D();
        const FVec2 d{ dst.x - cur.x, dst.y - cur.y };
        const f32 len  = std::sqrt(d.x * d.x + d.y * d.y);
        const f32 step = speed * dt;
        if (len <= step || len < 1e-5f) { owner->SetPosition2D(dst); arrived = true; }   // 到達
        else { owner->SetPosition2D(FVec2{ cur.x + d.x / len * step,
                                           cur.y + d.y / len * step }); arrived = false; }
    }

    /** 現在の参照/速度をログ出力する(BlueprintCallable + CallInEditor のデモ関数)。 */
    ACS_FUNCTION(BlueprintCallable, CallInEditor)
    void Ping() noexcept {
        ACS_LOG_INFO("[Follow] Ping! target=%d speed=%.1f arrived=%d", target, static_cast<double>(speed), arrived ? 1 : 0);
    }

private:
    /** target ID を root から解決して返す(結果は target 変化までキャッシュ)。 */
    ANode* ResolveTarget(ANode* owner) noexcept {
        if (m_Cached != nullptr && m_CachedFor == target) return m_Cached;
        ANode* root = owner;
        while (root->Parent() != nullptr) root = root->Parent();
        m_Cached    = root->FindBySerialId(target);
        m_CachedFor = target;
        return m_Cached;
    }

    ANode* m_Cached    = nullptr;   ///< 解決済み参照先(キャッシュ)。
    i32      m_CachedFor = -2;        ///< キャッシュ時の target(変化検出用、初期値は target と不一致）。
};

} // namespace acs::game
