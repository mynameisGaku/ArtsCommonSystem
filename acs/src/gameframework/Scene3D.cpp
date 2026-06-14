// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar B — FScene3D 実装
#include "gameframework/Scene3D.h"
#include "gameframework/MeshComponent3D.h"   // FMeshComponent3D (Raycast の bounds)
#include "asset/MeshAsset.h"                 // FMeshAsset (Mesh 種別の頂点 AABB)
#include "math/Mat.h"                        // Inverse / TransformPoint / TransformVector
#include "memory/UniquePtr.h"

namespace acs::game {

namespace {

/** const ノードから FMeshComponent3D を探す (ComponentAt 経由)。 */
const FMeshComponent3D* FindMeshC(const FNode3D& n) noexcept {
    const void* k = Component3DKindOf<FMeshComponent3D>();
    for (u32 i = 0; i < n.ComponentCount(); ++i) {
        const FComponent3D* c = n.ComponentAt(i);
        if (c != nullptr && c->Kind() == k) return static_cast<const FMeshComponent3D*>(c);
    }
    return nullptr;
}

/** プリミティブ種別ごとのローカル空間 AABB (Mesh は頂点から)。 */
Aabb3 LocalBounds3D(const FMeshComponent3D& m) noexcept {
    if (m.Primitive() == EMeshPrimitive3D::Plane) {
        return Aabb3{ FVec3{ 0, 0, 0 }, FVec3{ 0.5f, 0.02f, 0.5f } };   // 薄い板
    }
    if (m.Primitive() == EMeshPrimitive3D::Mesh) {
        const FMeshAsset* a = m.Mesh();
        if (a != nullptr && a->Vertices().Size() > 0) {
            FVec3 mn = a->Vertices()[0].position, mx = mn;
            for (u32 i = 1; i < a->Vertices().Size(); ++i) {
                const FVec3 p = a->Vertices()[i].position;
                mn.x = p.x < mn.x ? p.x : mn.x; mx.x = p.x > mx.x ? p.x : mx.x;
                mn.y = p.y < mn.y ? p.y : mn.y; mx.y = p.y > mx.y ? p.y : mx.y;
                mn.z = p.z < mn.z ? p.z : mn.z; mx.z = p.z > mx.z ? p.z : mx.z;
            }
            return Aabb3::FromMinMax(mn, mx);
        }
    }
    return Aabb3{ FVec3{ 0, 0, 0 }, FVec3{ 0.5f, 0.5f, 0.5f } };        // Cube/Sphere/フォールバック
}

/** subtree を DFS し、レイと «最も手前で» 交わるメッシュノードを探す。 */
void RaycastRec(const FNode3D* n, const Ray3& ray, FNodeId& best, f32& bestT) noexcept {
    if (n == nullptr) return;
    if (const FMeshComponent3D* m = FindMeshC(*n)) {
        const FMat4 M    = n->World().ToMat4();
        const FMat4 Minv = Inverse(M);
        // レイをノードのローカル空間へ (point/vector で別変換)。t は world レイと共通。
        const Ray3 lr{ TransformPoint(ray.origin, Minv), TransformVector(ray.direction, Minv) };
        const RayHit3 hit = RaycastAabb(lr, LocalBounds3D(*m));
        if (hit.hit && hit.t >= 0.0f && hit.t < bestT) { bestT = hit.t; best = n->Id(); }
    }
    for (u32 i = 0; i < n->ChildCount(); ++i) RaycastRec(n->Child(i), ray, best, bestT);
}

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
    FNode3D& child = p->AddChild(MakeUnique<FNode3D>(name));
    m_Pool.RegisterExistingNode(&child);   // 生成ノードに generational id を振る
    return child;
}

void FScene3D::Update(f32 dt) noexcept {
    m_Root.UpdateTree(dt);
    // reap される «前» に破棄予定ノードを pool から外す (ダングリング防止、どの破棄経路でも)。
    m_Pool.PurgePendingDestroy();
    m_Root.ResolveStructuralChanges();
}

void FScene3D::FixedUpdate(f32 fixed_dt) noexcept {
    m_Root.FixedUpdateTree(fixed_dt);
    m_Pool.PurgePendingDestroy();
    m_Root.ResolveStructuralChanges();
}

FNode3D* FScene3D::FindByName(FStringView name) noexcept {
    return FindByNameRec(&m_Root, name);
}

u32 FScene3D::NodeCount() const noexcept {
    return CountRec(&m_Root);
}

FNodeId FScene3D::Raycast(const Ray3& ray, f32* out_t) const noexcept {
    FNodeId best{};
    f32 bestT = 3.4028235e38f;
    RaycastRec(&m_Root, ray, best, bestT);
    if (out_t != nullptr && best.IsValid()) *out_t = bestT;
    return best;
}

void FScene3D::Clear() noexcept {
    // top-level 子を全て破棄予定にし、pool から外して即 reap (Update を待たない)。
    for (u32 i = 0; i < m_Root.ChildCount(); ++i) {
        if (FNode3D* c = m_Root.Child(i)) c->Destroy();
    }
    m_Pool.PurgePendingDestroy();
    m_Root.ResolveStructuralChanges();
    // root 自身を既定へ戻す (読み込み側が root 行で上書きする)。
    m_Root.Local() = FTransform3D::Identity();
    m_Root.SetName(FStringView("Root"));
}

} // namespace acs::game
