// SPDX-License-Identifier: Apache-2.0
// AABBとsphereを世代付きhandleで管理し、決定的な3D overlap・raycast queryを提供する。
#include "gameframework/CollisionWorld3D.h"

#include "foundation/Move.h"

#include <cmath>

namespace acs::game {
namespace {

/** 3成分がすべて有限ならtrueを返す。 */
bool IsFiniteVector3_Internal(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** AABBが有限で、半サイズと導出境界が有効ならtrueを返す。 */
bool IsValidAabb_Internal(const FAabb3& bounds) noexcept
{
    if (!IsFiniteVector3_Internal(bounds.center) || !IsFiniteVector3_Internal(bounds.half_size)) return false;
    if (bounds.half_size.x < 0.0f || bounds.half_size.y < 0.0f || bounds.half_size.z < 0.0f) return false;
    return IsFiniteVector3_Internal(bounds.Min()) && IsFiniteVector3_Internal(bounds.Max());
}

/** sphereが有限で、raycast可能な正の半径と導出境界を持てばtrueを返す。 */
bool IsValidSphere_Internal(const FSphere& sphere) noexcept
{
    if (!IsFiniteVector3_Internal(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius <= 0.0f)
        return false;
    const FVec3 radius{sphere.radius, sphere.radius, sphere.radius};
    return IsFiniteVector3_Internal(sphere.center - radius) && IsFiniteVector3_Internal(sphere.center + radius);
}

/** rayとT区間が有限でquery可能ならtrueを返す。 */
bool IsValidRayRange_Internal(const FRay3& ray, f32 minimum_t, f32 maximum_t) noexcept
{
    if (!IsFiniteVector3_Internal(ray.origin) || !IsFiniteVector3_Internal(ray.direction)) return false;
    if (!std::isfinite(minimum_t) || !std::isfinite(maximum_t) || minimum_t < 0.0f || maximum_t < minimum_t)
        return false;
    const f32 direction_length_squared = Dot(ray.direction, ray.direction);
    return std::isfinite(direction_length_squared) && direction_length_squared > 0.0f;
}

/** sphere内部から始まるrayをT=0命中として扱い、それ以外は共通math raycastへ委譲する。 */
FRayHit3 RaycastSphere_Internal(const FRay3& ray, const FSphere& sphere, f32 maximum_t) noexcept
{
    if (!Contains(sphere, ray.origin)) return RaycastSphere(ray, sphere, maximum_t);
    FRayHit3 hit;
    hit.hit = true;
    hit.t = 0.0f;
    hit.point = ray.origin;
    hit.normal = -Normalize(ray.direction);
    return hit;
}

/** raycast結果が有限で指定T区間内ならtrueを返す。 */
bool IsUsableRayHit_Internal(const FRayHit3& hit, f32 minimum_t, f32 maximum_t) noexcept
{
    return hit.hit && std::isfinite(hit.t) && hit.t >= minimum_t && hit.t <= maximum_t &&
           IsFiniteVector3_Internal(hit.point) && IsFiniteVector3_Internal(hit.normal);
}

} // namespace

/** 未使用slotを再利用し、無ければ末尾へ安全に追加する。 */
bool CCollisionWorld3D::TryAcquireSlot_Internal(u32& out_index) noexcept
{
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        if (!m_Slots[index].Active) {
            out_index = index;
            return true;
        }
    }

    if (m_Slots.IsEmpty() && !m_Slots.TryAdd(FSlot{})) return false;
    if (m_Slots.Num() > kMaximumSlotIndex) return false;
    const u32 new_index = static_cast<u32>(m_Slots.Num());
    if (!m_Slots.TryAdd(FSlot{})) return false;
    out_index = new_index;
    return true;
}

/** handleが指す生存slotを返す。 */
CCollisionWorld3D::FSlot* CCollisionWorld3D::FindSlot_Internal(FCollisionShapeId3D id) noexcept
{
    if (!id.IsValid() || id.Index() >= m_Slots.Num()) return nullptr;
    FSlot& slot = m_Slots[id.Index()];
    return slot.Active && slot.Generation == id.Generation() ? &slot : nullptr;
}

/** handleが指す生存slotを返す。 */
const CCollisionWorld3D::FSlot* CCollisionWorld3D::FindSlot_Internal(FCollisionShapeId3D id) const noexcept
{
    if (!id.IsValid() || id.Index() >= m_Slots.Num()) return nullptr;
    const FSlot& slot = m_Slots[id.Index()];
    return slot.Active && slot.Generation == id.Generation() ? &slot : nullptr;
}

/** 有効なAABBを新しい世代付きslotへ登録する。 */
FCollisionShapeId3D CCollisionWorld3D::TryAddAabb(const FAabb3& bounds, u32 layer) noexcept
{
    if (!IsValidAabb_Internal(bounds)) return {};
    u32 index = 0u;
    if (!TryAcquireSlot_Internal(index)) return {};
    FSlot& slot = m_Slots[index];
    slot.Generation = static_cast<u8>(slot.Generation + 1u);
    if (slot.Generation == 0u) slot.Generation = 1u;
    slot.Kind = EKind::Aabb;
    slot.Active = true;
    slot.Layer = layer;
    slot.Aabb = bounds;
    ++m_ShapeCount;
    return FCollisionShapeId3D{index, slot.Generation};
}

/** 有効なsphereを新しい世代付きslotへ登録する。 */
FCollisionShapeId3D CCollisionWorld3D::TryAddSphere(const FSphere& sphere, u32 layer) noexcept
{
    if (!IsValidSphere_Internal(sphere)) return {};
    u32 index = 0u;
    if (!TryAcquireSlot_Internal(index)) return {};
    FSlot& slot = m_Slots[index];
    slot.Generation = static_cast<u8>(slot.Generation + 1u);
    if (slot.Generation == 0u) slot.Generation = 1u;
    slot.Kind = EKind::Sphere;
    slot.Active = true;
    slot.Layer = layer;
    slot.Sphere = sphere;
    ++m_ShapeCount;
    return FCollisionShapeId3D{index, slot.Generation};
}

/** 生存するAABB slotだけを検証済み値へ更新する。 */
bool CCollisionWorld3D::TryUpdateAabb(FCollisionShapeId3D id, const FAabb3& bounds) noexcept
{
    if (!IsValidAabb_Internal(bounds)) return false;
    FSlot* const slot = FindSlot_Internal(id);
    if (slot == nullptr || slot->Kind != EKind::Aabb) return false;
    slot->Aabb = bounds;
    return true;
}

/** 生存するsphere slotだけを検証済み値へ更新する。 */
bool CCollisionWorld3D::TryUpdateSphere(FCollisionShapeId3D id, const FSphere& sphere) noexcept
{
    if (!IsValidSphere_Internal(sphere)) return false;
    FSlot* const slot = FindSlot_Internal(id);
    if (slot == nullptr || slot->Kind != EKind::Sphere) return false;
    slot->Sphere = sphere;
    return true;
}

/** 生存shapeのlayerを更新する。 */
bool CCollisionWorld3D::TrySetLayer(FCollisionShapeId3D id, u32 layer) noexcept
{
    FSlot* const slot = FindSlot_Internal(id);
    if (slot == nullptr) return false;
    slot->Layer = layer;
    return true;
}

/** 生存shapeのlayerを失敗時非破壊で返す。 */
bool CCollisionWorld3D::TryGetLayer(FCollisionShapeId3D id, u32& out_layer) const noexcept
{
    const FSlot* const slot = FindSlot_Internal(id);
    if (slot == nullptr) return false;
    out_layer = slot->Layer;
    return true;
}

/** 生存shapeを削除し、slotを次回登録で再利用可能にする。 */
bool CCollisionWorld3D::TryRemove(FCollisionShapeId3D id) noexcept
{
    FSlot* const slot = FindSlot_Internal(id);
    if (slot == nullptr) return false;
    slot->Active = false;
    slot->Kind = EKind::None;
    if (m_ShapeCount > 0u) --m_ShapeCount;
    return true;
}

/** handleが現在の生存slotを指すか検証する。 */
bool CCollisionWorld3D::IsAlive(FCollisionShapeId3D id) const noexcept
{
    return FindSlot_Internal(id) != nullptr;
}

/** 全slotを未使用化し、再利用時のgeneration更新で既存handleを失効させる。 */
void CCollisionWorld3D::ClearAll() noexcept
{
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        m_Slots[index].Active = false;
        m_Slots[index].Kind = EKind::None;
    }
    m_ShapeCount = 0u;
}

/** AABB queryへ重なるlayer対象shapeを決定的なslot順で列挙する。 */
bool CCollisionWorld3D::TryOverlapAabb(const FAabb3& bounds, TArray<FCollisionShapeId3D>& out_shapes, FCollisionShapeId3D exclude, u32 mask) const noexcept
{
    if (!IsValidAabb_Internal(bounds)) return false;
    TArray<FCollisionShapeId3D> result;
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        const FSlot& slot = m_Slots[index];
        if (!slot.Active || (slot.Layer & mask) == 0u) continue;
        const FCollisionShapeId3D current{index, slot.Generation};
        if (current == exclude) continue;
        const bool overlaps = slot.Kind == EKind::Aabb ? Intersect(slot.Aabb, bounds)
                                                       : slot.Kind == EKind::Sphere && Intersect(slot.Sphere, bounds);
        if (overlaps && !result.TryAdd(current)) return false;
    }
    out_shapes = Move(result);
    return true;
}

/** sphere queryへ重なるlayer対象shapeを決定的なslot順で列挙する。 */
bool CCollisionWorld3D::TryOverlapSphere(const FSphere& sphere, TArray<FCollisionShapeId3D>& out_shapes, FCollisionShapeId3D exclude, u32 mask) const noexcept
{
    if (!IsValidSphere_Internal(sphere)) return false;
    TArray<FCollisionShapeId3D> result;
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        const FSlot& slot = m_Slots[index];
        if (!slot.Active || (slot.Layer & mask) == 0u) continue;
        const FCollisionShapeId3D current{index, slot.Generation};
        if (current == exclude) continue;
        const bool overlaps = slot.Kind == EKind::Aabb ? Intersect(slot.Aabb, sphere)
                                                       : slot.Kind == EKind::Sphere && Intersect(slot.Sphere, sphere);
        if (overlaps && !result.TryAdd(current)) return false;
    }
    out_shapes = Move(result);
    return true;
}

/** 指定ray区間で最も近いlayer対象shapeを失敗時非破壊で返す。 */
bool CCollisionWorld3D::TryRaycast(const FRay3& ray, f32 minimum_t, f32 maximum_t, FRayHit3& out_hit, FCollisionShapeId3D& out_shape, FCollisionShapeId3D exclude, u32 mask) const noexcept
{
    if (!IsValidRayRange_Internal(ray, minimum_t, maximum_t)) return false;
    FRayHit3 best_hit;
    FCollisionShapeId3D best_shape;
    f32 best_t = maximum_t;
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        const FSlot& slot = m_Slots[index];
        if (!slot.Active || (slot.Layer & mask) == 0u) continue;
        const FCollisionShapeId3D current{index, slot.Generation};
        if (current == exclude) continue;
        FRayHit3 hit;
        if (slot.Kind == EKind::Aabb)
            hit = RaycastAabb(ray, slot.Aabb, best_t);
        else if (slot.Kind == EKind::Sphere)
            hit = RaycastSphere_Internal(ray, slot.Sphere, best_t);
        if (!IsUsableRayHit_Internal(hit, minimum_t, best_t)) continue;
        if (best_shape.IsValid() && hit.t >= best_t) continue;
        best_t = hit.t;
        best_hit = hit;
        best_shape = current;
    }
    if (!best_shape.IsValid()) return false;
    out_hit = best_hit;
    out_shape = best_shape;
    return true;
}

} // namespace acs::game
