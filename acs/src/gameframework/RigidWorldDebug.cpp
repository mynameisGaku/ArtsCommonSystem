// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — RigidWorldDebug 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/RigidWorldDebug.h"
#include "gameframework/RigidWorld2D.h"
#include "gameframework/DebugDraw.h"
#include "math/Collision2D.h"   // Aabb2 / Circle

namespace acs::game {

void DebugDrawRigidWorld(const FRigidWorld2D& world, FDebugDraw& dd,
                         FVec4 color, FVec4 vel_color, f32 vel_scale) noexcept {
    // slot を 0 から走査 (Body は slot 数を超えると nullptr)。非アクティブは飛ばす。
    for (u32 i = 0; ; ++i) {
        const FRigidBodyState* b = world.Body(i);
        if (b == nullptr) break;
        if (!b->active) continue;

        if (b->is_circle) dd.DrawCircle(Circle{ b->pos, b->radius }, color);
        else              dd.DrawAabb(Aabb2{ b->pos, b->half }, color);

        // 動的ボディの速度ベクトルを矢印で。
        if (b->inv_mass > 0.0f && (b->vel.x != 0.0f || b->vel.y != 0.0f)) {
            const FVec2 tip{ b->pos.x + b->vel.x * vel_scale, b->pos.y + b->vel.y * vel_scale };
            dd.DrawArrow(b->pos, tip, vel_color);
        }
    }
}

} // namespace acs::game
