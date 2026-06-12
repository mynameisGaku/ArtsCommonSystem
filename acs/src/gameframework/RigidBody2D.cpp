// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — FRigidBody2D 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/RigidBody2D.h"
#include "gameframework/Node2D.h"

namespace acs::game {

void FRigidBody2D::SetCircle(f32 radius, f32 mass, f32 restitution, f32 friction) noexcept {
    if (m_World == nullptr || !HasOwner()) return;
    if (m_Registered) m_World->RemoveBody(m_BodyIndex);   // 再登録は古いボディを置換 (ghost を残さない)
    const FVec2 p = Owner().Local().position;
    m_BodyIndex  = m_World->AddCircle(p, radius, mass, restitution, friction);
    m_Registered = true;
}

void FRigidBody2D::SetBox(FVec2 half, f32 mass, f32 restitution, f32 friction) noexcept {
    if (m_World == nullptr || !HasOwner()) return;
    if (m_Registered) m_World->RemoveBody(m_BodyIndex);   // 同上
    const FVec2 p = Owner().Local().position;
    m_BodyIndex  = m_World->AddDynamicAabb(p, half, mass, restitution, friction);
    m_Registered = true;
}

void FRigidBody2D::PullFromWorld() noexcept {
    if (!m_Registered || m_World == nullptr || !HasOwner()) return;
    Owner().Local().position = m_World->Position(m_BodyIndex);
    Owner().Local().rotation = m_World->Angle(m_BodyIndex);
}

void FRigidBody2D::OnDetach() noexcept {
    if (m_Registered && m_World != nullptr) {
        m_World->RemoveBody(m_BodyIndex);   // 破棄時にボディをワールドから除去
        m_Registered = false;
    }
}

namespace {

/** node とその子孫の FRigidBody2D を owner 位置へ同期する (DFS)。 */
void SyncTree(FNode2D& node) noexcept {
    const void* kind = ComponentKindOf<FRigidBody2D>();
    for (u32 c = 0; c < node.ComponentCount(); ++c) {
        FComponent2D* comp = node.ComponentAt(c);
        if (comp != nullptr && comp->Kind() == kind)
            static_cast<FRigidBody2D*>(comp)->PullFromWorld();
    }
    for (u32 i = 0; i < node.ChildCount(); ++i) {
        FNode2D* child = node.Child(i);
        if (child != nullptr) SyncTree(*child);
    }
}

} // namespace

void StepRigidBodies(FRigidWorld2D& world, FNode2D& root, f32 dt, FVec2 gravity) noexcept {
    world.Step(dt, gravity);
    SyncTree(root);
}

} // namespace acs::game
