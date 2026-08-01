// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — Steering2D 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/Steering2D.h"

#include <cmath>   // std::sqrt

namespace acs::game {

namespace {
inline f32   Len(FVec2 v) noexcept { return std::sqrt(v.x * v.x + v.y * v.y); }
inline FVec2 Sub(FVec2 a, FVec2 b) noexcept { return FVec2{ a.x - b.x, a.y - b.y }; }
inline FVec2 Scale(FVec2 v, f32 s) noexcept { return FVec2{ v.x * s, v.y * s }; }

/** v を最大長 max にクランプする (max<=0 はゼロに潰す = 操舵なし。負値で反転しない)。 */
FVec2 Truncate(FVec2 v, f32 max) noexcept {
    const f32 m = (max > 0.0f) ? max : 0.0f;
    const f32 l = Len(v);
    return (l > m && l > 1e-9f) ? Scale(v, m / l) : v;
}

/** desired_vel から現在速度 vel を引いた操舵力を max_force でクランプして返す。 */
FVec2 SteerToward(FVec2 desired, FVec2 vel, f32 max_force) noexcept {
    return Truncate(Sub(desired, vel), max_force);
}
} // namespace

FVec2 SteerSeek(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p) noexcept {
    const FVec2 to = Sub(target, pos);
    const f32   d  = Len(to);
    if (d < 1e-6f) return SteerToward(FVec2{0.0f, 0.0f}, vel, p.max_force);   // 既に到達
    const FVec2 desired = Scale(to, p.max_speed / d);
    return SteerToward(desired, vel, p.max_force);
}

FVec2 SteerFlee(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p) noexcept {
    const FVec2 away = Sub(pos, target);
    const f32   d    = Len(away);
    if (d < 1e-6f) return FVec2{0.0f, 0.0f};
    const FVec2 desired = Scale(away, p.max_speed / d);
    return SteerToward(desired, vel, p.max_force);
}

FVec2 SteerArrive(FVec2 pos, FVec2 vel, FVec2 target, FSteerParams p, f32 slow_radius) noexcept {
    const FVec2 to = Sub(target, pos);
    const f32   d  = Len(to);
    if (d < 1e-6f) return SteerToward(FVec2{0.0f, 0.0f}, vel, p.max_force);   // 目標で停止 (= -vel)
    // slow_radius 内は距離に比例して減速。
    const f32   speed   = (d < slow_radius) ? p.max_speed * (d / slow_radius) : p.max_speed;
    const FVec2 desired = Scale(to, speed / d);
    return SteerToward(desired, vel, p.max_force);
}

void CPathFollower::SetPath(const FVec2* points, u32 count, f32 arrive_radius) noexcept {
    m_Points       = points;
    m_Count        = count;
    m_Index        = 0u;
    m_ArriveRadius = (arrive_radius > 0.0f) ? arrive_radius : 0.5f;
}

FVec2 CPathFollower::Steer(FVec2 pos, FVec2 vel, FSteerParams p, bool& done) noexcept {
    // 到達済み waypoint を消化する。
    while (m_Points != nullptr && m_Index < m_Count
           && Len(Sub(m_Points[m_Index], pos)) <= m_ArriveRadius) {
        ++m_Index;
    }
    if (m_Points == nullptr || m_Index >= m_Count) {
        done = true;
        return SteerToward(FVec2{0.0f, 0.0f}, vel, p.max_force);   // 経路完了 → 停止操舵
    }
    done = false;
    // 最後の waypoint だけ Arrive で減速停止、それ以外は Seek で通過。
    const bool last = (m_Index + 1u == m_Count);
    return last ? SteerArrive(pos, vel, m_Points[m_Index], p, m_ArriveRadius * 4.0f)
                : SteerSeek(pos, vel, m_Points[m_Index], p);
}

} // namespace acs::game
