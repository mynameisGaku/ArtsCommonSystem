// SPDX-License-Identifier: Apache-2.0
#include "gameframework/MeshComponent3D.h"

#include <cmath>

namespace acs::game {

namespace {

/** 3D座標がすべて有限かを返す。 */
bool FiniteVector(FVec3 value) noexcept
{
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}

/** 有限なtriangleだけをraycastし、現在の最近hitを更新する。 */
void RaycastTriangleIfValid(const FRay3& ray, FVec3 v0, FVec3 v1, FVec3 v2, FRayHit3& best, f32& best_t) noexcept
{
    if (!FiniteVector(v0) || !FiniteVector(v1) || !FiniteVector(v2)) return;
    const FRayHit3 hit = RaycastTriangle(ray, v0, v1, v2, best_t);
    if (!hit.hit || !std::isfinite(hit.t)) return;
    best = hit;
    best_t = hit.t;
}

} // namespace

/** primitiveまたはauthored triangleへlocal空間でraycastする純粋計算。 */
FRayHit3 AMeshComponent3D::RaycastLocalGeometry(const FRay3& ray, f32 maximum_t) const noexcept
{
    if (!FiniteVector(ray.origin) || !FiniteVector(ray.direction) || !std::isfinite(maximum_t) || maximum_t < 0.0f) return FRayHit3{};
    const f32 direction_length_squared = Dot(ray.direction, ray.direction);
    if (!std::isfinite(direction_length_squared) || direction_length_squared < 1.0e-12f) return FRayHit3{};

    switch (Primitive()) {
    case EMeshPrimitive3D::Plane: {
        FRayHit3 hit = RaycastPlane(ray, FPlane::FromPointNormal(FVec3{0.0f, 0.0f, 0.0f}, FVec3{0.0f, 1.0f, 0.0f}), maximum_t);
        if (hit.hit && (Abs(hit.point.x) > 0.5f || Abs(hit.point.z) > 0.5f)) return FRayHit3{};
        return hit;
    }
    case EMeshPrimitive3D::Cube:
        return RaycastAabb(ray, FAabb3::FromCenterExtents(FVec3{0.0f, 0.0f, 0.0f}, FVec3{0.5f, 0.5f, 0.5f}), maximum_t);
    case EMeshPrimitive3D::Sphere:
        return RaycastSphere(ray, FSphere{FVec3{0.0f, 0.0f, 0.0f}, 0.5f}, maximum_t);
    case EMeshPrimitive3D::Mesh:
        break;
    }

    const AMeshAsset* mesh = Mesh();
    if (mesh == nullptr || mesh->Vertices().Num() < 3u) return FRayHit3{};
    const TArray<FMeshVertex>& vertices = mesh->Vertices();
    const TArray<u32>& indices = mesh->Indices();
    FRayHit3 best{};
    f32 best_t = maximum_t;
    if (indices.Num() >= 3u) {
        for (u32 index = 0u; index + 2u < indices.Num(); index += 3u) {
            const u32 i0 = indices[index + 0u];
            const u32 i1 = indices[index + 1u];
            const u32 i2 = indices[index + 2u];
            if (i0 >= vertices.Num() || i1 >= vertices.Num() || i2 >= vertices.Num()) continue;
            RaycastTriangleIfValid(ray, vertices[i0].position, vertices[i1].position, vertices[i2].position, best, best_t);
        }
    } else {
        for (u32 index = 0u; index + 2u < vertices.Num(); index += 3u)
            RaycastTriangleIfValid(ray, vertices[index + 0u].position, vertices[index + 1u].position, vertices[index + 2u].position, best, best_t);
    }
    return best;
}

} // namespace acs::game
