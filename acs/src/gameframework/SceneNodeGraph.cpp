// SPDX-License-Identifier: Apache-2.0
// root node treeとgeneration付きpoolを所有し、構造更新・検索・raycastを提供する。
#include "gameframework/SceneNodeGraph.h"
#include "gameframework/MeshComponent3D.h"   // AMeshComponent3D (Raycast の bounds)
#include "asset/MeshAsset.h"                 // AMeshAsset (Mesh 種別の頂点 AABB)
#include "math/Collision3D.h"                // FRay3 / FAabb3 / RaycastAabb (Raycast 本体)
#include "math/Mat.h"                        // Inverse / TransformPoint / TransformVector
#include "memory/UniquePtr.h"

#include <cmath>

namespace acs::game {

namespace {

/** const ノードから AMeshComponent3D を探す (ComponentAt 経由)。 */
const AMeshComponent3D* FindMeshC(const ANode& n) noexcept {
    const void* k = ComponentKindOf<AMeshComponent3D>();
    for (u32 i = 0; i < n.ComponentCount(); ++i) {
        const AComponent* c = n.ComponentAt(i);
        if (c != nullptr && c->Kind() == k) return static_cast<const AMeshComponent3D*>(c);
    }
    return nullptr;
}

/** プリミティブ種別ごとのローカル空間 AABB (Mesh は頂点から)。 */
FAabb3 LocalBounds3D(const AMeshComponent3D& m) noexcept {
    if (m.Primitive() == EMeshPrimitive3D::Plane) {
        return FAabb3{ FVec3{ 0, 0, 0 }, FVec3{ 0.5f, 0.02f, 0.5f } };   // 薄い板
    }
    if (m.Primitive() == EMeshPrimitive3D::Mesh) {
        const AMeshAsset* a = m.Mesh();
        if (a != nullptr && a->Vertices().Num() > 0) {
            FVec3 mn = a->Vertices()[0].position, mx = mn;
            for (u32 i = 1; i < a->Vertices().Num(); ++i) {
                const FVec3 p = a->Vertices()[i].position;
                mn.x = p.x < mn.x ? p.x : mn.x; mx.x = p.x > mx.x ? p.x : mx.x;
                mn.y = p.y < mn.y ? p.y : mn.y; mx.y = p.y > mx.y ? p.y : mx.y;
                mn.z = p.z < mn.z ? p.z : mn.z; mx.z = p.z > mx.z ? p.z : mx.z;
            }
            return FAabb3::FromMinMax(mn, mx);
        }
    }
    return FAabb3{ FVec3{ 0, 0, 0 }, FVec3{ 0.5f, 0.5f, 0.5f } };        // Cube/Sphere/フォールバック
}

/** subtree を DFS し、レイと «最も手前で» 交わるメッシュノードを探す。 */
void RaycastRec(const ANode* n, const FRay3& ray, FNodeId& best, f32& bestT) noexcept {
    if (n == nullptr) return;
    if (const AMeshComponent3D* m = FindMeshC(*n)) {
        const FMat4 M    = n->World().ToMat4();
        const FMat4 Minv = Inverse(M);
        // レイをノードのローカル空間へ (point/vector で別変換)。t は world レイと共通。
        const FRay3 lr{ TransformPoint(ray.origin, Minv), TransformVector(ray.direction, Minv) };
        const FRayHit3 hit = RaycastAabb(lr, LocalBounds3D(*m));
        if (hit.hit && hit.t >= 0.0f && hit.t < bestT) { bestT = hit.t; best = n->Id(); }
    }
    for (u32 i = 0; i < n->ChildCount(); ++i) RaycastRec(n->Child(i), ray, best, bestT);
}

/** world球半径をnode local軸へ写し、mesh boundsを安全側へ拡張する。 */
bool TryExpandBoundsForWorldSphere(const FTransform3D& world, f32 radius, FAabb3& bounds) noexcept
{
    if (radius == 0.0f) return true;
    const f32 scale_x = std::fabs(world.scale.x);
    const f32 scale_y = std::fabs(world.scale.y);
    const f32 scale_z = std::fabs(world.scale.z);
    if (!std::isfinite(scale_x) || !std::isfinite(scale_y) || !std::isfinite(scale_z) || scale_x <= 0.0f || scale_y <= 0.0f || scale_z <= 0.0f) return false;
    /** inverse scale後のsphereを覆うlocal軸別余白。 */
    const FVec3 padding{radius / scale_x, radius / scale_y, radius / scale_z};
    if (!std::isfinite(padding.x) || !std::isfinite(padding.y) || !std::isfinite(padding.z)) return false;
    bounds.half_size = FVec3{bounds.half_size.x + padding.x, bounds.half_size.y + padding.y, bounds.half_size.z + padding.z};
    return std::isfinite(bounds.half_size.x) && std::isfinite(bounds.half_size.y) && std::isfinite(bounds.half_size.z);
}

/** world rayと有限t区間がquery可能かを返す。 */
bool ValidRangeRay(const FRay3& ray, f32 minimum_t, f32 maximum_t) noexcept
{
    const bool finite_ray = std::isfinite(ray.origin.x) && std::isfinite(ray.origin.y) && std::isfinite(ray.origin.z) && std::isfinite(ray.direction.x) && std::isfinite(ray.direction.y) && std::isfinite(ray.direction.z);
    const f32 direction_length_squared = ray.direction.x * ray.direction.x + ray.direction.y * ray.direction.y + ray.direction.z * ray.direction.z;
    return finite_ray && std::isfinite(direction_length_squared) && direction_length_squared > 0.0f && std::isfinite(minimum_t) && std::isfinite(maximum_t) && minimum_t >= 0.0f && maximum_t >= minimum_t;
}

/** local boundsが有限かつ各軸で有効かを返す。 */
bool ValidBounds(const FAabb3& bounds) noexcept
{
    return std::isfinite(bounds.center.x) && std::isfinite(bounds.center.y) && std::isfinite(bounds.center.z) && std::isfinite(bounds.half_size.x) && std::isfinite(bounds.half_size.y) && std::isfinite(bounds.half_size.z) && bounds.half_size.x >= 0.0f && bounds.half_size.y >= 0.0f && bounds.half_size.z >= 0.0f;
}

/** 有効かつ可視なsubtreeから指定t区間内の最近meshを球probeで探す。 */
void SweepSphereActiveRangeRec(const ANode* node, const FRay3& center_ray, f32 radius, f32 minimum_t, f32 maximum_t, bool parent_active, FNodeId& best, f32& best_t) noexcept
{
    if (node == nullptr) return;
    const bool active = parent_active && !node->IsPendingDestroy() && node->IsEnabled() && node->IsVisible();
    if (!active) return;
    if (const AMeshComponent3D* mesh = FindMeshC(*node)) {
        const FTransform3D world = node->World();
        FAabb3 bounds = LocalBounds3D(*mesh);
        if (TryExpandBoundsForWorldSphere(world, radius, bounds)) {
            const FMat4 world_inverse = Inverse(world.ToMat4());
            /** world中心rayのtを保ったままnode localへ移したray。 */
            const FRay3 local_ray{TransformPoint(center_ray.origin, world_inverse), TransformVector(center_ray.direction, world_inverse)};
            const FRayHit3 hit = RaycastAabb(local_ray, bounds, maximum_t);
            if (hit.hit && std::isfinite(hit.t) && hit.t >= minimum_t && hit.t <= maximum_t && hit.t < best_t) {
                best_t = hit.t;
                best = node->Id();
            }
        }
    }
    for (u32 index = 0u; index < node->ChildCount(); ++index)
        SweepSphereActiveRangeRec(node->Child(index), center_ray, radius, minimum_t, maximum_t, active, best, best_t);
}

/** 有効かつ可視なsubtreeから指定t区間内の最近描画形状を厳密raycastする。 */
void RaycastGeometryActiveRangeRec(const ANode* node, const FRay3& ray, f32 minimum_t, f32 maximum_t, bool parent_active, FNodeId& best, f32& best_t) noexcept
{
    if (node == nullptr) return;
    const bool active = parent_active && !node->IsPendingDestroy() && node->IsEnabled() && node->IsVisible();
    if (!active) return;
    if (const AMeshComponent3D* mesh = FindMeshC(*node)) {
        const FMat4 world_inverse = Inverse(node->World().ToMat4());
        const FRay3 local_ray{TransformPoint(ray.origin, world_inverse), TransformVector(ray.direction, world_inverse)};
        const bool finite_local_ray = std::isfinite(local_ray.origin.x) && std::isfinite(local_ray.origin.y) && std::isfinite(local_ray.origin.z) && std::isfinite(local_ray.direction.x) && std::isfinite(local_ray.direction.y) && std::isfinite(local_ray.direction.z);
        const f32 local_direction_length_squared = local_ray.direction.x * local_ray.direction.x + local_ray.direction.y * local_ray.direction.y + local_ray.direction.z * local_ray.direction.z;
        const FAabb3 bounds = LocalBounds3D(*mesh);
        if (finite_local_ray && std::isfinite(local_direction_length_squared) && local_direction_length_squared > 0.0f && ValidBounds(bounds)) {
            const FRayHit3 broad_hit = RaycastAabb(local_ray, bounds, maximum_t);
            if (broad_hit.hit && std::isfinite(broad_hit.t) && broad_hit.t >= minimum_t) {
                const FRayHit3 hit = mesh->RaycastLocalGeometry(local_ray, maximum_t);
                if (hit.hit && std::isfinite(hit.t) && hit.t >= minimum_t && hit.t <= maximum_t && hit.t < best_t) {
                    best_t = hit.t;
                    best = node->Id();
                }
            }
        }
    }
    for (u32 index = 0u; index < node->ChildCount(); ++index)
        RaycastGeometryActiveRangeRec(node->Child(index), ray, minimum_t, maximum_t, active, best, best_t);
}

/** subtree を深さ優先で走査し name に一致する最初のノードを返す (root から再帰)。 */
ANode* FindByNameRec(ANode* n, FStringView name) noexcept {
    if (n == nullptr) return nullptr;
    if (n->Name() == name) return n;
    for (u32 i = 0; i < n->ChildCount(); ++i) {
        if (ANode* hit = FindByNameRec(n->Child(i), name)) return hit;
    }
    return nullptr;
}

/** subtree のノード数を数える (自分 + 全子孫)。 */
u32 CountRec(const ANode* n) noexcept {
    if (n == nullptr) return 0;
    u32 total = 1;
    for (u32 i = 0; i < n->ChildCount(); ++i) {
        total += CountRec(n->Child(i));
    }
    return total;
}

} // namespace

FScene3DSpawnResult CSceneNodeGraph::TrySpawn(FStringView name, ANode* parent) noexcept {
    ANode* p = (parent != nullptr) ? parent : m_Root.Get();
    const FNodeId parent_id = m_Pool.IdOf(p);
    if (!parent_id.IsValid() || m_Pool.Get(parent_id) != p) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::InvalidParent,
            ENodePoolRegisterError::None, EAddChildResult::Added
        };
    }

    const ANode* tree_root = p;
    u32 parent_depth = 0u;
    while (tree_root != nullptr && tree_root->Parent() != nullptr
           && parent_depth <= kNodeMaxTreeDepth) {
        tree_root = tree_root->Parent();
        ++parent_depth;
    }
    if (tree_root != m_Root.Get() || parent_depth > kNodeMaxTreeDepth) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::InvalidParent,
            ENodePoolRegisterError::None, EAddChildResult::Added
        };
    }
    // 新規 child は未所属・非pending・高さ0なので、TryAddChild が拒否し得る条件を
    // pool slot 確保前に判定する。通常の境界失敗では物理 slot 容量すら変更しない。
    if (p->IsPendingDestroy()) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::ChildAttachRejected,
            ENodePoolRegisterError::None, EAddChildResult::ParentPendingDestroy
        };
    }
    if (parent_depth >= kNodeMaxTreeDepth) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::ChildAttachRejected,
            ENodePoolRegisterError::None, EAddChildResult::TreeDepthLimitExceeded
        };
    }

    TObjectPtr<ANode> child = NewObject<ANode>(name);
    if (!child) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::NodeAllocationFailure,
            ENodePoolRegisterError::AllocationFailure, EAddChildResult::Added
        };
    }
    ANode* raw_child = child.Get();
    const FNodePoolRegisterResult registration =
        m_Pool.TryRegisterExistingNode(raw_child);
    if (!registration.Succeeded()) {
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::PoolRegistrationFailure,
            registration.Error, EAddChildResult::Added
        };
    }

    const EAddChildResult add_result = p->TryAddChild(child);
    if (add_result != EAddChildResult::Added) {
        m_Pool.Unregister(registration.Id);
        return FScene3DSpawnResult{
            nullptr, FNodeId{}, EScene3DSpawnError::ChildAttachRejected,
            ENodePoolRegisterError::None, add_result
        };
    }
    return FScene3DSpawnResult{
        raw_child, registration.Id, EScene3DSpawnError::None,
        ENodePoolRegisterError::None, EAddChildResult::Added
    };
}

ANode& CSceneNodeGraph::Spawn(FStringView name, ANode* parent) noexcept {
    const FScene3DSpawnResult result = TrySpawn(name, parent);
    if (result.Succeeded()) return *result.Node;

    if (parent != nullptr) {
        const FNodeId parent_id = m_Pool.IdOf(parent);
        if (parent_id.IsValid() && m_Pool.Get(parent_id) == parent) return *parent;
    }
    return *m_Root;
}

void CSceneNodeGraph::Update(f32 dt) noexcept {
    m_Root->UpdateTree(dt);
    // reap される «前» に破棄予定ノードを pool から外す (ダングリング防止、どの破棄経路でも)。
    m_Pool.PurgePendingDestroy();
    m_Root->ResolveStructuralChanges();
}

void CSceneNodeGraph::FixedUpdate(f32 fixed_dt) noexcept {
    m_Root->FixedUpdateTree(fixed_dt);
    m_Pool.PurgePendingDestroy();
    m_Root->ResolveStructuralChanges();
}

void CSceneNodeGraph::ResolveStructuralChanges() noexcept {
    m_Pool.PurgePendingDestroy();
    m_Root->ResolveStructuralChanges();
}

ANode* CSceneNodeGraph::FindByName(FStringView name) noexcept {
    return FindByNameRec(m_Root.Get(), name);
}

u32 CSceneNodeGraph::NodeCount() const noexcept {
    return CountRec(m_Root.Get());
}

FNodeId CSceneNodeGraph::Raycast(const FRay3& ray, f32* out_t) const noexcept {
    FNodeId best{};
    f32 bestT = 3.4028235e38f;
    RaycastRec(m_Root.Get(), ray, best, bestT);
    if (out_t != nullptr && best.IsValid()) *out_t = bestT;
    return best;
}

/** 有効な描画meshだけを有限t区間で検索し、外れでは出力を維持する。 */
FNodeId CSceneNodeGraph::RaycastActiveRange(const FRay3& ray, f32 minimum_t, f32 maximum_t, f32* out_t) const noexcept
{
    return SweepSphereActiveRange(ray, 0.0f, minimum_t, maximum_t, out_t);
}

/** 有効な描画形状へ有限区間の厳密raycastを行い、外れでは出力を維持する。 */
FNodeId CSceneNodeGraph::RaycastGeometryActiveRange(const FRay3& ray, f32 minimum_t, f32 maximum_t, f32* out_t) const noexcept
{
    if (!ValidRangeRay(ray, minimum_t, maximum_t)) return FNodeId{};
    FNodeId best{};
    f32 best_t = 3.4028235e38f;
    RaycastGeometryActiveRangeRec(m_Root.Get(), ray, minimum_t, maximum_t, true, best, best_t);
    if (out_t != nullptr && best.IsValid()) *out_t = best_t;
    return best;
}

/** 有効な描画mesh boundsへ有限区間のworld球をsweepし、外れでは出力を維持する。 */
FNodeId CSceneNodeGraph::SweepSphereActiveRange(const FRay3& center_ray, f32 radius, f32 minimum_t, f32 maximum_t, f32* out_t) const noexcept
{
    if (!ValidRangeRay(center_ray, minimum_t, maximum_t) || !std::isfinite(radius) || radius < 0.0f) return FNodeId{};
    FNodeId best{};
    f32 best_t = 3.4028235e38f;
    SweepSphereActiveRangeRec(m_Root.Get(), center_ray, radius, minimum_t, maximum_t, true, best, best_t);
    if (out_t != nullptr && best.IsValid()) *out_t = best_t;
    return best;
}

void CSceneNodeGraph::Clear() noexcept {
    // top-level 子を全て破棄予定にし、pool から外して即 reap (Update を待たない)。
    for (u32 i = 0; i < m_Root->ChildCount(); ++i) {
        if (ANode* c = m_Root->Child(i)) c->Destroy();
    }
    m_Pool.PurgePendingDestroy();
    m_Root->ResolveStructuralChanges();
    // root 自身を既定へ戻す (読み込み側が root 行で上書きする)。
    m_Root->Local() = FTransform3D::Identity();
    m_Root->SetName(FStringView("Root"));
}

} // namespace acs::game
