// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FNode3D 実装
#include "gameframework/Node3D.h"
#include "foundation/Move.h"
#include "foundation/Log.h"

namespace acs::game {

/** 親をたどって world transform を合成して返す (キャッシュなし)。 */
FTransform3D FNode3D::World() const noexcept {
    if (m_Parent == nullptr) return m_Local;
    // 親をたどって合成 (キャッシュなし、深いツリーで O(depth) コスト)
    return m_Parent->World().Compose(m_Local);
}

/** 子の所有権を奪い、未 spawn なら OnSpawn を即時に呼ぶ。 */
FNode3D& FNode3D::AddChild(TUniquePtr<FNode3D> child) noexcept {
    if (!child) return *this;   // null はチェイン記述を壊さないよう自身を返す
    child->m_Parent = this;
    m_Children.PushBack(Move(child));
    FNode3D& ref = *m_Children.Back();
    if (!ref.m_Spawned) {
        ref.m_Spawned = true;
        ref.OnSpawn();
    }
    return ref;
}

/** 自身と components の OnUpdate を呼び、子へ可変刻み update を伝播する。 */
void FNode3D::UpdateTree(f32 dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnUpdate(dt);
    // components の OnUpdate を node 自身の後に呼ぶ (合成された振る舞いを適用)
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnUpdate(dt);
    }
    // index 走査で走査中 AddChild に対応 (新しい子は同フレームで OnUpdate される)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode3D* c = m_Children[i].Get();
        if (c != nullptr) c->UpdateTree(dt);
    }
}

/** 自身と components の OnFixedUpdate を呼び、子へ固定刻み update を伝播する。 */
void FNode3D::FixedUpdateTree(f32 fixed_dt) noexcept {
    if (!m_Enabled || m_PendingDestroy) return;
    OnFixedUpdate(fixed_dt);
    for (u32 i = 0; i < m_Components.Size(); ++i) {
        if (m_Components[i]) m_Components[i]->OnFixedUpdate(fixed_dt);
    }
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode3D* c = m_Children[i].Get();
        if (c != nullptr) c->FixedUpdateTree(fixed_dt);
    }
}

bool FNode3D::IsAncestorOf(const FNode3D* candidate) const noexcept {
    if (candidate == nullptr) return false;
    // candidate から親を辿り、自分 (this) に行き着けば ancestor。木構造前提 (cycle 無し)。
    const FNode3D* cur = candidate->m_Parent;
    while (cur != nullptr) {
        if (cur == this) return true;
        cur = cur->m_Parent;
    }
    return false;
}

void FNode3D::Reparent(FNode3D& new_parent) noexcept {
    if (&new_parent == this) {
        ACS_LOG_WARN("FNode3D::Reparent: cannot reparent to self");
        return;
    }
    if (&new_parent == m_Parent) {
        return;   // 既に同 parent = no-op
    }
    if (m_Parent == nullptr) {
        ACS_LOG_WARN("FNode3D::Reparent: root node has no parent (scene root cannot be reparented)");
        return;
    }
    if (IsAncestorOf(&new_parent)) {
        ACS_LOG_WARN("FNode3D::Reparent: target is descendant — would create cycle");
        return;
    }
    if (m_PendingDestroy) {
        ACS_LOG_WARN("FNode3D::Reparent: node is pending destroy — ignored");
        return;
    }
    m_PendingReparentTarget = &new_parent;
}

void FNode3D::ResolveStructuralChanges() noexcept {
    // 1) 子の subtree を先に resolve (子の死を先に確定させる)
    for (u32 i = 0; i < m_Children.Size(); ++i) {
        FNode3D* c = m_Children[i].Get();
        if (c != nullptr) c->ResolveStructuralChanges();
    }

    // 2) pending_destroy な子を OnDespawn して TArray から除く (compact pattern)。
    //    pending_reparent_target が立つ子は new_parent 側へ Move する。
    TArray<TUniquePtr<FNode3D>> reparent_pending;

    u32 w = 0;
    for (u32 r = 0; r < m_Children.Size(); ++r) {
        FNode3D* c = m_Children[r].Get();
        if (c == nullptr) continue;
        if (c->m_PendingDestroy) {
            // components の OnDetach を node の OnDespawn より先に発火
            for (u32 ci = 0; ci < c->m_Components.Size(); ++ci) {
                if (c->m_Components[ci]) c->m_Components[ci]->OnDetach();
            }
            c->m_Components.Clear();
            c->OnDespawn();
            m_Children[r].Reset();   // ノードのデストラクタ (子→親順で解放)
        } else if (c->m_PendingReparentTarget != nullptr) {
            reparent_pending.PushBack(Move(m_Children[r]));
        } else {
            if (w != r) m_Children[w] = Move(m_Children[r]);
            ++w;
        }
    }
    while (m_Children.Size() > w) m_Children.PopBack();

    // 3) reparent 対象を target の m_Children に追加 (target が消えていれば自分に戻す)。
    for (u32 i = 0; i < reparent_pending.Size(); ++i) {
        if (!reparent_pending[i]) continue;
        FNode3D* moved = reparent_pending[i].Get();
        FNode3D* target = moved->m_PendingReparentTarget;
        moved->m_PendingReparentTarget = nullptr;
        if (target == nullptr) {
            moved->m_Parent = this;
            m_Children.PushBack(Move(reparent_pending[i]));
            continue;
        }
        moved->m_Parent = target;
        target->m_Children.PushBack(Move(reparent_pending[i]));
    }
}

} // namespace acs::game
