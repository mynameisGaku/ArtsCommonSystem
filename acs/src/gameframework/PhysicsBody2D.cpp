// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F Phase 2 — FPhysicsBody2D 実装 (Phase 11)
#include "gameframework/PhysicsBody2D.h"

namespace acs::game {

void FPhysicsBody2D::OnAttach(FNode2D& owner) noexcept {
    if (_world == nullptr || _kind == ShapeKind::None) return;
    RegisterShapeAt(owner.Local().position);
}

void FPhysicsBody2D::OnDetach() noexcept {
    if (_world != nullptr && _registered) {
        _world->Remove(_handle);
    }
    _handle = FShapeId{};
    _registered = false;
}

void FPhysicsBody2D::RegisterShapeAt(FVec2 pos) noexcept {
    if (_world == nullptr || _registered) return;
    if (_kind == ShapeKind::Circle) {
        _handle = _world->AddCircle(Circle{pos, _radius});
    } else if (_kind == ShapeKind::FAabb) {
        _handle = _world->AddAabb(Aabb2{pos, _half_size});
    }
    _registered = _handle.IsValid();
}

void FPhysicsBody2D::SyncShapeIfRegistered() noexcept {
    if (_world == nullptr || !_registered || !HasOwner()) return;
    const FVec2 pos = Owner().Local().position;
    if (_kind == ShapeKind::Circle) {
        _world->UpdateCircle(_handle, Circle{pos, _radius});
    } else if (_kind == ShapeKind::FAabb) {
        _world->UpdateAabb(_handle, Aabb2{pos, _half_size});
    }
}

bool FPhysicsBody2D::WouldBlockAt(FVec2 pos) noexcept {
    if (_world == nullptr) return false;
    TArray<FShapeId> hits;
    if (_kind == ShapeKind::Circle) {
        _world->OverlapCircle(Circle{pos, _radius}, hits, _handle);
    } else if (_kind == ShapeKind::FAabb) {
        _world->OverlapAabb(Aabb2{pos, _half_size}, hits, _handle);
    }
    return hits.Size() > 0;
}

void FPhysicsBody2D::OnUpdate(f32 dt) noexcept {
    if (_world == nullptr || _kind == ShapeKind::None || dt <= 0.0f) return;
    if (!_registered) RegisterShapeAt(Owner().Local().position);

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
