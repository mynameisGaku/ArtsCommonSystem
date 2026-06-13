// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FScene3D 実装
#include "gameframework/Scene3D.h"
#include "memory/UniquePtr.h"

namespace acs::game {

namespace {

/** subtree を深さ優先で走査し name に一致する最初のノードを返す (root から再帰)。 */
FNode3D* FindByNameRec(FNode3D* n, FStringView name) noexcept {
    if (n == nullptr) return nullptr;
    if (n->Name() == name) return n;
    for (u32 i = 0; i < n->ChildCount(); ++i) {
        if (FNode3D* hit = FindByNameRec(n->Child(i), name)) return hit;
    }
    return nullptr;
}

/** subtree のノード数を数える (自分 + 全子孫)。 */
u32 CountRec(const FNode3D* n) noexcept {
    if (n == nullptr) return 0;
    u32 total = 1;
    for (u32 i = 0; i < n->ChildCount(); ++i) {
        total += CountRec(n->Child(i));
    }
    return total;
}

} // namespace

FNode3D& FScene3D::Spawn(FStringView name, FNode3D* parent) noexcept {
    FNode3D* p = (parent != nullptr) ? parent : &m_Root;
    return p->AddChild(MakeUnique<FNode3D>(name));
}

void FScene3D::Update(f32 dt) noexcept {
    m_Root.UpdateTree(dt);
    m_Root.ResolveStructuralChanges();
}

void FScene3D::FixedUpdate(f32 fixed_dt) noexcept {
    m_Root.FixedUpdateTree(fixed_dt);
    m_Root.ResolveStructuralChanges();
}

FNode3D* FScene3D::FindByName(FStringView name) noexcept {
    return FindByNameRec(&m_Root, name);
}

u32 FScene3D::NodeCount() const noexcept {
    return CountRec(&m_Root);
}

} // namespace acs::game
