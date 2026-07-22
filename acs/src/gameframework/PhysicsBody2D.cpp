// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar F — APhysicsBody2D 実装 (collide-and-slide)
#include "gameframework/PhysicsBody2D.h"
#include "math/Math.h"

namespace acs::game {

/** ローカル多角形を pos へ平行移動した world 多角形を返す。 */
FConvexPoly2 APhysicsBody2D::WorldPoly(FVec2 pos) const noexcept {
    FConvexPoly2 p;
    for (u32 i = 0; i < m_LocalPoly.count; ++i) {
        p.Add(FVec2{ m_LocalPoly.verts[i].x + pos.x, m_LocalPoly.verts[i].y + pos.y });
    }
    return p;
}

/** attach 時に owner の現在位置で shape を CollisionWorld に登録する。 */
void APhysicsBody2D::OnAttach(ANode& owner) noexcept {
    if (m_World == nullptr || m_Kind == EShapeKind::None) return;
    RegisterShapeAt(owner.Position2D());
}

/** detach 時に登録済み shape を除去し handle を無効化する。 */
void APhysicsBody2D::OnDetach() noexcept {
    if (m_World != nullptr && m_Registered) {
        m_World->Remove(m_Handle);
    }
    m_Handle = FShapeId{};
    m_Registered = false;
}

/** 未登録時のみ、現在の形状種別に応じて pos に shape を登録する。 */
void APhysicsBody2D::RegisterShapeAt(FVec2 pos) noexcept {
    if (m_World == nullptr || m_Registered) return;
    if (m_Kind == EShapeKind::Circle) {
        m_Handle = m_World->AddCircle(FCircle{pos, m_Radius});
    } else if (m_Kind == EShapeKind::Aabb) {
        m_Handle = m_World->AddAabb(FAabb2{pos, m_HalfSize});
    } else if (m_Kind == EShapeKind::Poly) {
        m_Handle = m_World->AddPolygon(WorldPoly(pos));
    }
    m_Registered = m_Handle.IsValid();
}

/** 登録済みなら owner の現在位置で CollisionWorld の shape を更新する。 */
void APhysicsBody2D::SyncShapeIfRegistered() noexcept {
    if (m_World == nullptr || !m_Registered || !HasOwner()) return;
    const FVec2 pos = Owner().Position2D();
    if (m_Kind == EShapeKind::Circle) {
        m_World->UpdateCircle(m_Handle, FCircle{pos, m_Radius});
    } else if (m_Kind == EShapeKind::Aabb) {
        m_World->UpdateAabb(m_Handle, FAabb2{pos, m_HalfSize});
    } else if (m_Kind == EShapeKind::Poly) {
        m_World->UpdatePolygon(m_Handle, WorldPoly(pos));
    }
}

/** pos に shape を置いたとき自分以外と overlap するかを overlap クエリで判定する。 */
bool APhysicsBody2D::WouldBlockAt(FVec2 pos) noexcept {
    if (m_World == nullptr) return false;
    TArray<FShapeId> hits;
    if (m_Kind == EShapeKind::Circle) {
        m_World->OverlapCircle(FCircle{pos, m_Radius}, hits, m_Handle);
    } else if (m_Kind == EShapeKind::Aabb) {
        m_World->OverlapAabb(FAabb2{pos, m_HalfSize}, hits, m_Handle);
    } else if (m_Kind == EShapeKind::Poly) {
        m_World->OverlapPolygon(WorldPoly(pos), hits, m_Handle);
    }
    return hits.Size() > 0;
}

/**
 * 速度を統合し、collide-and-slide (または軸独立 block) で owner 位置を更新する。
 *
 * @param dt 経過秒 (0 以下なら何もしない)。
 */
void APhysicsBody2D::OnUpdate(f32 dt) noexcept {
    if (m_World == nullptr || m_Kind == EShapeKind::None || dt <= 0.0f) return;
    if (!m_Registered) RegisterShapeAt(Owner().Position2D());

    // 1) 速度統合 (acceleration + gravity)
    velocity.x += (acceleration.x + gravity.x) * dt;
    velocity.y += (acceleration.y + gravity.y) * dt;

    const FVec2 pos = Owner().Position2D();

    if (!slide) {
        // 旧来の軸独立 block (互換用)
        FVec2 next = pos;
        const FVec2 try_x{pos.x + velocity.x * dt, pos.y};
        if (WouldBlockAt(try_x)) velocity.x = 0.0f; else next.x = try_x.x;
        const FVec2 try_y{next.x, next.y + velocity.y * dt};
        if (WouldBlockAt(try_y)) velocity.y = 0.0f; else next.y = try_y.y;
        Owner().SetPosition2D(next);
        SyncShapeIfRegistered();
        return;
    }

    // 2) collide-and-slide: 望む変位だけ動かし、貫通を MTV で押し出し、
    //    押し出し法線方向に入る速度成分を消す (= 面に沿って滑る)。これを数回反復。
    FVec2 p{ pos.x + velocity.x * dt, pos.y + velocity.y * dt };
    for (int iter = 0; iter < 4; ++iter) {
        FVec2 push{ 0, 0 };
        if (m_Kind == EShapeKind::Circle) {
            push = m_World->ResolveCircle(FCircle{p, m_Radius}, m_Handle);
        } else if (m_Kind == EShapeKind::Aabb) {
            push = m_World->ResolvePolygon(ToPoly(FAabb2{p, m_HalfSize}), m_Handle);
        } else { // Poly
            push = m_World->ResolvePolygon(WorldPoly(p), m_Handle);
        }
        const f32 len2 = push.x * push.x + push.y * push.y;
        if (len2 < 1e-8f) break;                       // 貫通なし
        p.x += push.x; p.y += push.y;                   // 押し出して分離
        const f32 inv = 1.0f / Sqrt(len2);
        const FVec2 n{ push.x * inv, push.y * inv };    // 分離法線 (外向き)
        const f32 vn = velocity.x * n.x + velocity.y * n.y;
        if (vn < 0.0f) {                                // 面へ入る速度成分だけ除去 → スライド
            velocity.x -= n.x * vn;
            velocity.y -= n.y * vn;
        }
    }

    // 3) Owner Transform を更新 + CollisionWorld 反映
    Owner().SetPosition2D(p);
    SyncShapeIfRegistered();
}

} // namespace acs::game
