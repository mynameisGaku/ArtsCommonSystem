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

/** 登録shapeとのsphere sweep候補。 */
struct FSphereSweepCandidate3D {
    /** 接触候補が有効ならtrue。 */
    bool Hit = false;

    /** T=0で既に重なっていた場合はtrue。 */
    bool StartedOverlapping = false;

    /** sphere中心ray上の接触parameter。 */
    f32 T = 0.0f;

    /** 接触時の移動sphere中心。 */
    FVec3 Center{};

    /** 登録shapeから移動sphereへ向く単位法線。 */
    FVec3 Normal{};
};

/** 一軸分のsphere中心rayとAABB境界。 */
struct FAabbSweepAxis3D {
    /** sphere中心rayの開始座標。 */
    f64 Origin = 0.0;

    /** sphere中心rayの方向成分。 */
    f64 Direction = 0.0;

    /** AABBの最小座標。 */
    f64 Minimum = 0.0;

    /** AABBの最大座標。 */
    f64 Maximum = 0.0;
};

/** AABBまでの距離二乗を表す区間内の二次式係数。 */
struct FQuadraticPolynomial {
    /** T二乗の係数。 */
    f64 Quadratic = 0.0;

    /** Tの係数。 */
    f64 Linear = 0.0;

    /** 定数項。 */
    f64 Constant = 0.0;
};

/** 有効な進行方向と逆向きの単位法線を返す。 */
FVec3 OppositeDirectionNormal_Internal(FVec3 direction) noexcept
{
    const f32 length_squared = Dot(direction, direction);
    return direction * (-1.0f / std::sqrt(length_squared));
}

/** AABB上の最近傍点、または中心が内部なら最寄り面から接触法線を求める。 */
FVec3 CalculateAabbContactNormal_Internal(FVec3 center, const FAabb3& bounds, FVec3 direction) noexcept
{
    const FVec3 minimum = bounds.Min();
    const FVec3 maximum = bounds.Max();
    const FVec3 closest{center.x < minimum.x ? minimum.x : (center.x > maximum.x ? maximum.x : center.x), center.y < minimum.y ? minimum.y : (center.y > maximum.y ? maximum.y : center.y), center.z < minimum.z ? minimum.z : (center.z > maximum.z ? maximum.z : center.z)};
    const FVec3 delta = center - closest;
    const f32 length_squared = Dot(delta, delta);
    if (std::isfinite(length_squared) && length_squared > 0.0f) return delta * (1.0f / std::sqrt(length_squared));
    if (!Contains(bounds, center)) return OppositeDirectionNormal_Internal(direction);

    /** 内部中心から最短で抜ける面までの距離。 */
    f32 nearest_distance = center.x - minimum.x;
    /** 同距離では-X、+X、-Y、+Y、-Z、+Zの順を維持する法線。 */
    FVec3 normal{-1.0f, 0.0f, 0.0f};
    const f32 positive_x_distance = maximum.x - center.x;
    if (positive_x_distance < nearest_distance) {
        nearest_distance = positive_x_distance;
        normal = FVec3{1.0f, 0.0f, 0.0f};
    }
    const f32 negative_y_distance = center.y - minimum.y;
    if (negative_y_distance < nearest_distance) {
        nearest_distance = negative_y_distance;
        normal = FVec3{0.0f, -1.0f, 0.0f};
    }
    const f32 positive_y_distance = maximum.y - center.y;
    if (positive_y_distance < nearest_distance) {
        nearest_distance = positive_y_distance;
        normal = FVec3{0.0f, 1.0f, 0.0f};
    }
    const f32 negative_z_distance = center.z - minimum.z;
    if (negative_z_distance < nearest_distance) {
        nearest_distance = negative_z_distance;
        normal = FVec3{0.0f, 0.0f, -1.0f};
    }
    const f32 positive_z_distance = maximum.z - center.z;
    if (positive_z_distance < nearest_distance) normal = FVec3{0.0f, 0.0f, 1.0f};
    return normal;
}

/** sphere中心差、または同心時の進行逆向きから接触法線を求める。 */
FVec3 CalculateSphereContactNormal_Internal(FVec3 center, const FSphere& sphere, FVec3 direction) noexcept
{
    const FVec3 delta = center - sphere.center;
    const f32 length_squared = Dot(delta, delta);
    if (std::isfinite(length_squared) && length_squared > 0.0f) return delta * (1.0f / std::sqrt(length_squared));
    return OppositeDirectionNormal_Internal(direction);
}

/** 指定Tのsphere中心とAABBの距離二乗を倍精度で返す。 */
f64 SquaredDistanceToAabbAt_Internal(const FRay3& center_ray, const FAabb3& bounds, f64 t) noexcept
{
    const FVec3 minimum = bounds.Min();
    const FVec3 maximum = bounds.Max();
    const f64 x = static_cast<f64>(center_ray.origin.x) + static_cast<f64>(center_ray.direction.x) * t;
    const f64 y = static_cast<f64>(center_ray.origin.y) + static_cast<f64>(center_ray.direction.y) * t;
    const f64 z = static_cast<f64>(center_ray.origin.z) + static_cast<f64>(center_ray.direction.z) * t;
    const f64 dx = x < minimum.x ? static_cast<f64>(minimum.x) - x : (x > maximum.x ? x - static_cast<f64>(maximum.x) : 0.0);
    const f64 dy = y < minimum.y ? static_cast<f64>(minimum.y) - y : (y > maximum.y ? y - static_cast<f64>(maximum.y) : 0.0);
    const f64 dz = z < minimum.z ? static_cast<f64>(minimum.z) - z : (z > maximum.z ? z - static_cast<f64>(maximum.z) : 0.0);
    return dx * dx + dy * dy + dz * dz;
}

/** 一軸のAABB距離を区間内の二次式へ加える。 */
void AddAabbDistanceAxisPolynomial_Internal(const FAabbSweepAxis3D& axis, f64 sample_t, FQuadraticPolynomial& polynomial) noexcept
{
    const f64 sample_position = axis.Origin + axis.Direction * sample_t;
    f64 intercept = 0.0;
    f64 slope = 0.0;
    if (sample_position < axis.Minimum) {
        intercept = axis.Minimum - axis.Origin;
        slope = -axis.Direction;
    } else if (sample_position > axis.Maximum) {
        intercept = axis.Origin - axis.Maximum;
        slope = axis.Direction;
    } else {
        return;
    }
    polynomial.Quadratic += slope * slope;
    polynomial.Linear += 2.0 * intercept * slope;
    polynomial.Constant += intercept * intercept;
}

/** 指定TからAABB向けsphere sweep候補を構築する。 */
FSphereSweepCandidate3D MakeAabbSweepCandidate_Internal(const FRay3& center_ray, const FAabb3& bounds, f64 t, bool started_overlapping) noexcept
{
    const f64 center_x = static_cast<f64>(center_ray.origin.x) + static_cast<f64>(center_ray.direction.x) * t;
    const f64 center_y = static_cast<f64>(center_ray.origin.y) + static_cast<f64>(center_ray.direction.y) * t;
    const f64 center_z = static_cast<f64>(center_ray.origin.z) + static_cast<f64>(center_ray.direction.z) * t;
    const FVec3 center{static_cast<f32>(center_x), static_cast<f32>(center_y), static_cast<f32>(center_z)};
    if (!IsFiniteVector3_Internal(center) || !std::isfinite(t)) return {};
    const FVec3 normal = CalculateAabbContactNormal_Internal(center, bounds, center_ray.direction);
    if (!IsFiniteVector3_Internal(normal)) return {};
    return FSphereSweepCandidate3D{true, started_overlapping, static_cast<f32>(t), center, normal};
}

/** AABBまでの区分二次距離を解き、丸めた面・辺・角への最初のsphere接触を返す。 */
FSphereSweepCandidate3D SweepSphereAabb_Internal(const FRay3& center_ray, f32 radius, const FAabb3& bounds, f32 maximum_t) noexcept
{
    const f64 radius_squared = static_cast<f64>(radius) * static_cast<f64>(radius);
    if (SquaredDistanceToAabbAt_Internal(center_ray, bounds, 0.0) <= radius_squared)
        return MakeAabbSweepCandidate_Internal(center_ray, bounds, 0.0, true);

    /** t=0、上限、各軸がAABB面を横切る時刻を保持する。 */
    f64 breakpoints[8]{0.0, static_cast<f64>(maximum_t)};
    u32 breakpoint_count = 2u;
    /** 軸ごとのray値とAABB境界。 */
    const FVec3 bounds_minimum = bounds.Min();
    const FVec3 bounds_maximum = bounds.Max();
    const FAabbSweepAxis3D axes[3]{{center_ray.origin.x, center_ray.direction.x, bounds_minimum.x, bounds_maximum.x}, {center_ray.origin.y, center_ray.direction.y, bounds_minimum.y, bounds_maximum.y}, {center_ray.origin.z, center_ray.direction.z, bounds_minimum.z, bounds_maximum.z}};
    for (u32 axis = 0u; axis < 3u; ++axis) {
        if (axes[axis].Direction == 0.0) continue;
        const f64 minimum_crossing = (axes[axis].Minimum - axes[axis].Origin) / axes[axis].Direction;
        if (minimum_crossing > 0.0 && minimum_crossing < maximum_t) breakpoints[breakpoint_count++] = minimum_crossing;
        const f64 maximum_crossing = (axes[axis].Maximum - axes[axis].Origin) / axes[axis].Direction;
        if (maximum_crossing > 0.0 && maximum_crossing < maximum_t) breakpoints[breakpoint_count++] = maximum_crossing;
    }

    /** 少数固定要素を昇順へ並べ、時系列に区間を調べられるようにする。 */
    for (u32 index = 1u; index < breakpoint_count; ++index) {
        const f64 value = breakpoints[index];
        u32 insertion = index;
        while (insertion > 0u && value < breakpoints[insertion - 1u]) {
            breakpoints[insertion] = breakpoints[insertion - 1u];
            --insertion;
        }
        breakpoints[insertion] = value;
    }

    for (u32 interval = 0u; interval + 1u < breakpoint_count; ++interval) {
        const f64 left_t = breakpoints[interval];
        const f64 right_t = breakpoints[interval + 1u];
        if (right_t <= left_t) continue;
        const f64 sample_t = left_t + (right_t - left_t) * 0.5;
        FQuadraticPolynomial polynomial{0.0, 0.0, -radius_squared};
        for (u32 axis = 0u; axis < 3u; ++axis) AddAabbDistanceAxisPolynomial_Internal(axes[axis], sample_t, polynomial);

        const f64 left_value = (polynomial.Quadratic * left_t + polynomial.Linear) * left_t + polynomial.Constant;
        if (left_value <= 0.0) return MakeAabbSweepCandidate_Internal(center_ray, bounds, left_t, false);
        if (polynomial.Quadratic <= 0.0) continue;
        f64 discriminant = polynomial.Linear * polynomial.Linear - 4.0 * polynomial.Quadratic * polynomial.Constant;
        const f64 discriminant_scale = std::fabs(polynomial.Linear * polynomial.Linear) + std::fabs(4.0 * polynomial.Quadratic * polynomial.Constant) + 1.0;
        if (discriminant < 0.0) {
            if (discriminant < -1.0e-12 * discriminant_scale) continue;
            discriminant = 0.0;
        }
        f64 contact_t = (-polynomial.Linear - std::sqrt(discriminant)) / (2.0 * polynomial.Quadratic);
        const f64 interval_tolerance = 1.0e-10 * (1.0 + std::fabs(left_t) + std::fabs(right_t));
        if (contact_t < left_t - interval_tolerance || contact_t > right_t + interval_tolerance) continue;
        if (contact_t < left_t) contact_t = left_t;
        if (contact_t > right_t) contact_t = right_t;
        const f64 contact_error = SquaredDistanceToAabbAt_Internal(center_ray, bounds, contact_t) - radius_squared;
        const f64 distance_tolerance = 1.0e-9 * (1.0 + radius_squared);
        if (contact_error > distance_tolerance) continue;
        return MakeAabbSweepCandidate_Internal(center_ray, bounds, contact_t, false);
    }
    return {};
}

/** static sphereとの半径和を使い、移動sphere中心の最初の接触を返す。 */
FSphereSweepCandidate3D SweepSphereSphere_Internal(const FRay3& center_ray, f32 radius, const FSphere& sphere, f32 maximum_t) noexcept
{
    const f64 combined_radius = static_cast<f64>(radius) + static_cast<f64>(sphere.radius);
    if (!std::isfinite(combined_radius)) return {};
    const f64 offset_x = static_cast<f64>(center_ray.origin.x) - static_cast<f64>(sphere.center.x);
    const f64 offset_y = static_cast<f64>(center_ray.origin.y) - static_cast<f64>(sphere.center.y);
    const f64 offset_z = static_cast<f64>(center_ray.origin.z) - static_cast<f64>(sphere.center.z);
    const f64 direction_x = center_ray.direction.x;
    const f64 direction_y = center_ray.direction.y;
    const f64 direction_z = center_ray.direction.z;
    const f64 quadratic = direction_x * direction_x + direction_y * direction_y + direction_z * direction_z;
    const f64 linear = 2.0 * (offset_x * direction_x + offset_y * direction_y + offset_z * direction_z);
    const f64 constant = offset_x * offset_x + offset_y * offset_y + offset_z * offset_z - combined_radius * combined_radius;
    if (constant <= 0.0) {
        const FVec3 normal = CalculateSphereContactNormal_Internal(center_ray.origin, sphere, center_ray.direction);
        return FSphereSweepCandidate3D{true, true, 0.0f, center_ray.origin, normal};
    }
    f64 discriminant = linear * linear - 4.0 * quadratic * constant;
    const f64 discriminant_scale = std::fabs(linear * linear) + std::fabs(4.0 * quadratic * constant) + 1.0;
    if (discriminant < 0.0) {
        if (discriminant < -1.0e-12 * discriminant_scale) return {};
        discriminant = 0.0;
    }
    const f64 contact_t = (-linear - std::sqrt(discriminant)) / (2.0 * quadratic);
    if (!std::isfinite(contact_t) || contact_t < 0.0 || contact_t > maximum_t) return {};
    const f64 center_x = static_cast<f64>(center_ray.origin.x) + direction_x * contact_t;
    const f64 center_y = static_cast<f64>(center_ray.origin.y) + direction_y * contact_t;
    const f64 center_z = static_cast<f64>(center_ray.origin.z) + direction_z * contact_t;
    const FVec3 center{static_cast<f32>(center_x), static_cast<f32>(center_y), static_cast<f32>(center_z)};
    if (!IsFiniteVector3_Internal(center)) return {};
    const FVec3 normal = CalculateSphereContactNormal_Internal(center, sphere, center_ray.direction);
    return FSphereSweepCandidate3D{true, false, static_cast<f32>(contact_t), center, normal};
}

/** sphere sweep候補が有限なT・中心・単位化可能な法線を持つか返す。 */
bool IsUsableSphereSweepCandidate_Internal(const FSphereSweepCandidate3D& candidate, f32 minimum_t, f32 maximum_t) noexcept
{
    const f32 normal_length_squared = Dot(candidate.Normal, candidate.Normal);
    return candidate.Hit && std::isfinite(candidate.T) && candidate.T >= minimum_t && candidate.T <= maximum_t &&
           IsFiniteVector3_Internal(candidate.Center) && IsFiniteVector3_Internal(candidate.Normal) &&
           std::isfinite(normal_length_squared) && normal_length_squared > 0.0f;
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

/** query sphereの最深penetrationを決定的なslot順で失敗時非破壊に返す。 */
bool CCollisionWorld3D::TryFindSpherePenetration(const FSphere& sphere, FCollisionPenetration3D& out_penetration, FCollisionShapeId3D exclude, u32 mask) const noexcept
{
    if (!IsValidSphere_Internal(sphere)) return false;
    FCollisionPenetration3D best_penetration;
    f32 best_depth = 0.0f;
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        const FSlot& slot = m_Slots[index];
        if (!slot.Active || (slot.Layer & mask) == 0u) continue;
        const FCollisionShapeId3D current{index, slot.Generation};
        if (current == exclude) continue;
        FVec3 translation;
        bool penetrates = false;
        if (slot.Kind == EKind::Aabb)
            penetrates = Resolve(sphere, slot.Aabb, translation);
        else if (slot.Kind == EKind::Sphere)
            penetrates = Resolve(sphere, slot.Sphere, translation);
        if (!penetrates || !IsFiniteVector3_Internal(translation)) continue;
        const f32 depth_squared = Dot(translation, translation);
        if (!std::isfinite(depth_squared) || depth_squared <= 0.0f) continue;
        const f32 depth = std::sqrt(depth_squared);
        if (!std::isfinite(depth) || (best_penetration.IsValid() && depth <= best_depth)) continue;
        const FVec3 normal = translation * (1.0f / depth);
        if (!IsFiniteVector3_Internal(normal)) continue;
        best_depth = depth;
        best_penetration = FCollisionPenetration3D{current, depth, normal};
    }
    if (!best_penetration.IsValid()) return false;
    out_penetration = best_penetration;
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

/** 移動sphereとlayer対象shapeの最初の連続接触を失敗時非破壊で返す。 */
bool CCollisionWorld3D::TrySweepSphere(const FRay3& center_ray, f32 radius, f32 minimum_t, f32 maximum_t, FCollisionSweepHit3D& out_hit, FCollisionShapeId3D exclude, u32 mask) const noexcept
{
    if (!IsValidRayRange_Internal(center_ray, minimum_t, maximum_t) || !std::isfinite(radius) || radius < 0.0f)
        return false;
    FSphereSweepCandidate3D best_candidate;
    FCollisionShapeId3D best_shape;
    f32 best_t = maximum_t;
    for (u32 index = 1u; index < m_Slots.Num(); ++index) {
        const FSlot& slot = m_Slots[index];
        if (!slot.Active || (slot.Layer & mask) == 0u) continue;
        const FCollisionShapeId3D current{index, slot.Generation};
        if (current == exclude) continue;
        FSphereSweepCandidate3D candidate;
        if (slot.Kind == EKind::Aabb)
            candidate = SweepSphereAabb_Internal(center_ray, radius, slot.Aabb, best_t);
        else if (slot.Kind == EKind::Sphere)
            candidate = SweepSphereSphere_Internal(center_ray, radius, slot.Sphere, best_t);
        if (!IsUsableSphereSweepCandidate_Internal(candidate, minimum_t, best_t)) continue;
        if (best_shape.IsValid() && candidate.T >= best_t) continue;
        best_t = candidate.T;
        best_candidate = candidate;
        best_shape = current;
    }
    if (!best_shape.IsValid()) return false;
    out_hit = FCollisionSweepHit3D{best_shape, best_candidate.T, best_candidate.Center, best_candidate.Normal, best_candidate.StartedOverlapping};
    return true;
}

} // namespace acs::game
