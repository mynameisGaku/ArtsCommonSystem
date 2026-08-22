// SPDX-License-Identifier: Apache-2.0
// 3D sphereの複数接触をworld非破壊の明示的な反復処理で解消する。
#include "gameframework/SpherePenetrationResolution3D.h"

#include <cmath>

namespace acs::game {
namespace {

/** 3成分がすべて有限ならtrueを返す。 */
bool IsFiniteVector3_Internal(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** sphereが有限で、貫通queryに使える正の半径と導出境界を持てばtrueを返す。 */
bool IsValidSphere_Internal(const FSphere& sphere) noexcept
{
    if (!IsFiniteVector3_Internal(sphere.center) || !std::isfinite(sphere.radius) || sphere.radius <= 0.0f)
        return false;
    const FVec3 radius{sphere.radius, sphere.radius, sphere.radius};
    return IsFiniteVector3_Internal(sphere.center - radius) && IsFiniteVector3_Internal(sphere.center + radius);
}

} // namespace

/** 最深接触を順に解消し、worldを変更せず収束状態を返す。 */
bool TryResolveSpherePenetrations3D(const CCollisionWorld3D& world, const FSphere& sphere, FSpherePenetrationResolution3D& out_result, u32 max_iterations, FCollisionShapeId3D exclude, u32 mask) noexcept
{
    if (!IsValidSphere_Internal(sphere) || max_iterations > FSpherePenetrationResolution3D::kMaximumIterations)
        return false;

    FSphere resolved_sphere = sphere;
    u32 iteration_count = 0u;
    bool fully_resolved = false;
    while (iteration_count < max_iterations) {
        FCollisionPenetration3D penetration;
        if (!world.TryFindSpherePenetration(resolved_sphere, penetration, exclude, mask)) {
            fully_resolved = true;
            break;
        }

        FSphere candidate = resolved_sphere;
        candidate.center += penetration.Translation();
        if (!IsValidSphere_Internal(candidate)) return false;
        resolved_sphere = candidate;
        ++iteration_count;
    }

    if (!fully_resolved) {
        FCollisionPenetration3D remaining_penetration;
        fully_resolved = !world.TryFindSpherePenetration(resolved_sphere, remaining_penetration, exclude, mask);
    }

    const FVec3 translation = resolved_sphere.center - sphere.center;
    if (!IsFiniteVector3_Internal(translation)) return false;
    out_result = FSpherePenetrationResolution3D{resolved_sphere, translation, iteration_count, fully_resolved};
    return true;
}

} // namespace acs::game
