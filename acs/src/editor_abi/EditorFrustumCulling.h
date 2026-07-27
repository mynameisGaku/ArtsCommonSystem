// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"

#include <algorithm>
#include <cmath>

namespace acs::editor_frustum_culling {

struct FPlane {
    FVec3 normal{0.0f, 0.0f, 0.0f};
    f32 distance = 0.0f;
};

inline bool MakePlane(
    f32 x, f32 y, f32 z, f32 distance,
    FPlane& output) noexcept {
    const f32 length_squared = x * x + y * y + z * z;
    if (!std::isfinite(length_squared) ||
        length_squared <= 1.0e-12f ||
        !std::isfinite(distance)) {
        return false;
    }
    const f32 inverse_length =
        1.0f / std::sqrt(length_squared);
    output.normal = FVec3{
        x * inverse_length,
        y * inverse_length,
        z * inverse_length};
    output.distance = distance * inverse_length;
    return std::isfinite(output.distance);
}

/**
 * Extract normalized planes for ACS row-vector matrices and D3D z=[0,w].
 */
inline bool ExtractPlanes(
    const FMat4& view_projection,
    FPlane (&planes)[6]) noexcept {
    return
        MakePlane(
            view_projection.m[0][3] + view_projection.m[0][0],
            view_projection.m[1][3] + view_projection.m[1][0],
            view_projection.m[2][3] + view_projection.m[2][0],
            view_projection.m[3][3] + view_projection.m[3][0],
            planes[0]) &&
        MakePlane(
            view_projection.m[0][3] - view_projection.m[0][0],
            view_projection.m[1][3] - view_projection.m[1][0],
            view_projection.m[2][3] - view_projection.m[2][0],
            view_projection.m[3][3] - view_projection.m[3][0],
            planes[1]) &&
        MakePlane(
            view_projection.m[0][3] + view_projection.m[0][1],
            view_projection.m[1][3] + view_projection.m[1][1],
            view_projection.m[2][3] + view_projection.m[2][1],
            view_projection.m[3][3] + view_projection.m[3][1],
            planes[2]) &&
        MakePlane(
            view_projection.m[0][3] - view_projection.m[0][1],
            view_projection.m[1][3] - view_projection.m[1][1],
            view_projection.m[2][3] - view_projection.m[2][1],
            view_projection.m[3][3] - view_projection.m[3][1],
            planes[3]) &&
        MakePlane(
            view_projection.m[0][2],
            view_projection.m[1][2],
            view_projection.m[2][2],
            view_projection.m[3][2],
            planes[4]) &&
        MakePlane(
            view_projection.m[0][3] - view_projection.m[0][2],
            view_projection.m[1][3] - view_projection.m[1][2],
            view_projection.m[2][3] - view_projection.m[2][2],
            view_projection.m[3][3] - view_projection.m[3][2],
            planes[5]);
}

struct FNodeDecision {
    bool valid = false;
    bool visible = true; // invalid decisions are fail-open
    f32 world_radius = 0.0f;
};

inline FNodeDecision EvaluateSphere(
    const FPlane (&planes)[6], FVec3 center,
    f32 local_radius, FVec3 world_scale) noexcept {
    FNodeDecision decision{};
    if (!std::isfinite(center.x) ||
        !std::isfinite(center.y) ||
        !std::isfinite(center.z) ||
        !std::isfinite(local_radius) ||
        local_radius < 0.0f ||
        !std::isfinite(world_scale.x) ||
        !std::isfinite(world_scale.y) ||
        !std::isfinite(world_scale.z)) {
        return decision;
    }
    const f32 maximum_scale = std::max(
        std::fabs(world_scale.x),
        std::max(
            std::fabs(world_scale.y),
            std::fabs(world_scale.z)));
    decision.world_radius =
        local_radius * maximum_scale;
    if (!std::isfinite(decision.world_radius))
        return decision;
    decision.valid = true;
    for (const FPlane& plane : planes) {
        const f32 signed_distance =
            center.x * plane.normal.x +
            center.y * plane.normal.y +
            center.z * plane.normal.z +
            plane.distance;
        if (signed_distance < -decision.world_radius) {
            decision.visible = false;
            break;
        }
    }
    return decision;
}

struct FFrameDecision {
    bool enabled = true;
    u32 tested = 0u;
    u32 visible = 0u;
    u32 culled = 0u;

    void Apply(const FNodeDecision& node) noexcept {
        if (!node.valid) {
            enabled = false;
            tested = 0u;
            visible = 0u;
            culled = 0u;
            return;
        }
        if (!enabled) return;
        ++tested;
        if (node.visible)
            ++visible;
        else
            ++culled;
    }
};

inline bool ShouldSubmitOpaque(
    bool culling_enabled, bool visible) noexcept {
    return !culling_enabled || visible;
}

} // namespace acs::editor_frustum_culling
