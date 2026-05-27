// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 2 — FPhysicsBody2D 実装 (Phase 11)
#include "gameframework/PhysicsBody2D.h"

namespace acs::game {

void FPhysicsBody2D::OnAttach(FNode2D& owner) noexcept {
    if (m_World == nullptr || m_Kind == ShapeKind::None) return;
    RegisterShapeAt(owner.Local().position);
}

void FPhysicsBody2D::OnDetach() noexcept {
    if (m_World != nullptr && m_Registered) {
        m_World->Remove(m_Handle);
    }
    m_Handle = FShapeId{};
    m_Registered = false;
}

void FPhysicsBody2D::RegisterShapeAt(FVec2 pos) noexcept {
    if (m_World == nullptr || m_Registered) return;
    if (m_Kind == ShapeKind::Circle) {
        m_Handle = m_World->AddCircle(Circle{pos, m_Radius});
    } else if (m_Kind == ShapeKind::FAabb) {
        m_Handle = m_World->AddAabb(Aabb2{pos, m_HalfSize});
    }
    m_Registered = m_Handle.IsValid();
}

void FPhysicsBody2D::SyncShapeIfRegistered() noexcept {
    if (m_World == nullptr || !m_Registered || !HasOwner()) return;
    const FVec2 pos = Owner().Local().position;
    if (m_Kind == ShapeKind::Circle) {
        m_World->UpdateCircle(m_Handle, Circle{pos, m_Radius});
    } else if (m_Kind == ShapeKind::FAabb) {
        m_World->UpdateAabb(m_Handle, Aabb2{pos, m_HalfSize});
    }
}

bool FPhysicsBody2D::WouldBlockAt(FVec2 pos) noexcept {
    if (m_World == nullptr) return false;
    TArray<FShapeId> hits;
    if (m_Kind == ShapeKind::Circle) {
        m_World->OverlapCircle(Circle{pos, m_Radius}, hits, m_Handle);
    } else if (m_Kind == ShapeKind::FAabb) {
        m_World->OverlapAabb(Aabb2{pos, m_HalfSize}, hits, m_Handle);
    }
    return hits.Size() > 0;
}

void FPhysicsBody2D::OnUpdate(f32 dt) noexcept {
    if (m_World == nullptr || m_Kind == ShapeKind::None || dt <= 0.0f) return;
    if (!m_Registered) RegisterShapeAt(Owner().Local().position);

    // 1) 速度統合 (acceleration + gravity)
    velocity.x += (acceleration.x + gravity.x) * dt;
    velocity.y += (acceleration.y + gravity.y) * dt;

    // 2) 軸独立移動 (X 試行 → blocked なら v.x=0、続けて Y 試行)
    const FVec2 pos = Owner().Local().position;
    FVec2 next = pos;

    // X 軸
    const FVec2 try_x{pos.x + velocity.x * dt, pos.y};
    if (WouldBlockAt(try_x)) {
        velocity.x = 0.0f;
    } else {
        next.x = try_x.x;
    }
    // Y 軸 (X 試行後の位置から)
    const FVec2 try_y{next.x, next.y + velocity.y * dt};
    if (WouldBlockAt(try_y)) {
        velocity.y = 0.0f;
    } else {
        next.y = try_y.y;
    }

    // 3) Owner Transform を更新 + CollisionWorld 反映
    Owner().Local().position = next;
    SyncShapeIfRegistered();
}

} // namespace acs::game
