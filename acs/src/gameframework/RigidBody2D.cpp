// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// GameFramework — ARigidBody2D 実装。詳細はヘッダ参照。
// =============================================================================
#include "gameframework/RigidBody2D.h"
#include "gameframework/ANode.h"

namespace acs::game {

void ARigidBody2D::SetCircle(f32 radius, f32 mass, f32 restitution, f32 friction) noexcept {
    if (m_World == nullptr || !HasOwner()) return;
    if (m_Registered) m_World->RemoveBody(m_BodyIndex);   // 再登録は古いボディを置換 (ghost を残さない)
    const FVec2 p = Owner().Position2D();
    m_BodyIndex  = m_World->AddCircle(p, radius, mass, restitution, friction);
    m_Registered = true;
}

void ARigidBody2D::SetBox(FVec2 half, f32 mass, f32 restitution, f32 friction) noexcept {
    if (m_World == nullptr || !HasOwner()) return;
    if (m_Registered) m_World->RemoveBody(m_BodyIndex);   // 同上
    const FVec2 p = Owner().Position2D();
    m_BodyIndex  = m_World->AddDynamicAabb(p, half, mass, restitution, friction);
    m_Registered = true;
}

void ARigidBody2D::PullFromWorld() noexcept {
    if (!m_Registered || m_World == nullptr || !HasOwner()) return;
    Owner().SetPosition2D(m_World->Position(m_BodyIndex));
    Owner().SetRotation2D(m_World->Angle(m_BodyIndex));
}

void ARigidBody2D::OnDetach() noexcept {
    if (m_Registered && m_World != nullptr) {
        m_World->RemoveBody(m_BodyIndex);   // 破棄時にボディをワールドから除去
        m_Registered = false;
    }
}

namespace {

/** node とその子孫の ARigidBody2D を owner 位置へ同期する (DFS)。 */
void SyncTree(ANode& node) noexcept {
    const void* kind = ComponentKindOf<ARigidBody2D>();
    for (u32 c = 0; c < node.ComponentCount(); ++c) {
        AComponent* comp = node.ComponentAt(c);
        if (comp != nullptr && comp->Kind() == kind)
            static_cast<ARigidBody2D*>(comp)->PullFromWorld();
    }
    for (u32 i = 0; i < node.ChildCount(); ++i) {
        ANode* child = node.Child(i);
        if (child != nullptr) SyncTree(*child);
    }
}

} // namespace

void StepRigidBodies(CRigidWorld2D& world, ANode& root, f32 dt, FVec2 gravity) noexcept {
    world.Step(dt, gravity);
    SyncTree(root);
}

} // namespace acs::game
