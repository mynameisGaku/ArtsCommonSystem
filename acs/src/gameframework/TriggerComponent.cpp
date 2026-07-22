// SPDX-License-Identifier: Apache-2.0
#include "gameframework/TriggerComponent.h"
#include "gameframework/ANode.h"

namespace acs::game {

void ATriggerComponent::OnAttach(ANode& owner) noexcept {
    // AddComponent は構築直後に OnAttach を呼ぶため、利用者が SetCircle/SetAabb を
    // 呼ぶ前に形状が確定しない。登録は初回 OnUpdate まで遅延する (APhysicsBody2D
    // と同様、設定後の形状で登録するため)。
    (void)owner;
}

void ATriggerComponent::Register() noexcept {
    if (m_World == nullptr) return;
    const FTransform2D wt = Owner().World2D();
    if (m_Kind == EKind::Circle) {
        m_Id = m_World->AddCircle(FCircle{wt.position, m_Radius}, m_Layer);
    } else {
        m_Id = m_World->AddAabb(FAabb2{wt.position, m_Half}, m_Layer);
    }
    m_Registered = m_Id.IsValid();
    if (m_Registered) {
        m_World->SetUserData(m_Id, this);
        // world の global コールバックを静的ディスパッチャに差し替える (idempotent:
        // 同じ fn ptr + user なので何度呼んでも安全)。これにより各 component の
        // OnTriggerEnter/Exit が id 逆引きで呼ばれる。
        m_World->SetOnEnter(&SDispatchEnter, m_World);
        m_World->SetOnExit(&SDispatchExit, m_World);
    }
}

void ATriggerComponent::SyncShape() noexcept {
    if (!m_Registered || m_World == nullptr) return;
    const FTransform2D wt = Owner().World2D();
    if (m_Kind == EKind::Circle) {
        m_World->UpdateCircle(m_Id, FCircle{wt.position, m_Radius});
    } else {
        m_World->UpdateAabb(m_Id, FAabb2{wt.position, m_Half});
    }
}

void ATriggerComponent::OnUpdate(f32 /*dt*/) noexcept {
    if (!m_Registered) Register();
    SyncShape();
}

void ATriggerComponent::OnDetach() noexcept {
    if (m_Registered && m_World != nullptr) {
        m_World->Remove(m_Id);
    }
    m_Registered = false;
}

void ATriggerComponent::HandleEnter(ATriggerComponent* other) noexcept {
    OnTriggerEnter(other);
    if (m_OnEnter != nullptr) m_OnEnter(*this, other, m_OnEnterUser);
}

void ATriggerComponent::HandleExit(ATriggerComponent* other) noexcept {
    OnTriggerExit(other);
    if (m_OnExit != nullptr) m_OnExit(*this, other, m_OnExitUser);
}

void ATriggerComponent::SDispatchEnter(void* user, FTriggerId self, FTriggerId other) noexcept {
    auto* world = static_cast<FTriggerWorld2D*>(user);
    if (world == nullptr) return;
    auto* a = static_cast<ATriggerComponent*>(world->UserData(self));
    auto* b = static_cast<ATriggerComponent*>(world->UserData(other));
    if (a == nullptr || b == nullptr) return;
    // 各 component は「相手の layer が自分の mask に合致するとき」だけ発火する。
    if ((b->m_Layer & a->m_Mask) != 0u) a->HandleEnter(b);
    if ((a->m_Layer & b->m_Mask) != 0u) b->HandleEnter(a);
}

void ATriggerComponent::SDispatchExit(void* user, FTriggerId self, FTriggerId other) noexcept {
    auto* world = static_cast<FTriggerWorld2D*>(user);
    if (world == nullptr) return;
    auto* a = static_cast<ATriggerComponent*>(world->UserData(self));
    auto* b = static_cast<ATriggerComponent*>(world->UserData(other));
    if (a == nullptr || b == nullptr) return;
    if ((b->m_Layer & a->m_Mask) != 0u) a->HandleExit(b);
    if ((a->m_Layer & b->m_Mask) != 0u) b->HandleExit(a);
}

} // namespace acs::game
