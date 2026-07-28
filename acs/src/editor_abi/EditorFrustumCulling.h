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
    f32 local_radius, FVec3 world_scale,
    f32 world_radius_padding = 0.0f) noexcept {
    FNodeDecision decision{};
    if (!std::isfinite(center.x) ||
        !std::isfinite(center.y) ||
        !std::isfinite(center.z) ||
        !std::isfinite(local_radius) ||
        local_radius < 0.0f ||
        !std::isfinite(world_scale.x) ||
        !std::isfinite(world_scale.y) ||
        !std::isfinite(world_scale.z) ||
        !std::isfinite(world_radius_padding) ||
        world_radius_padding < 0.0f) {
        return decision;
    }
    const f32 maximum_scale = std::max(
        std::fabs(world_scale.x),
        std::max(
            std::fabs(world_scale.y),
            std::fabs(world_scale.z)));
    decision.world_radius =
        local_radius * maximum_scale +
        world_radius_padding;
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

/**
 * Allocation-free view over the exact main-view visibility mask.
 *
 * A missing or short mask is deliberately fail-open. DrawScene3D builds the
 * mask before opening its first main-view geometry pass, but this additional
 * boundary keeps a stale/incomplete diagnostic buffer from dropping geometry.
 */
struct FSubmissionMaskView {
    bool enabled = false;
    const u8* visibility = nullptr;
    u32 visibility_count = 0u;

    bool ShouldSubmit(u32 node_index) const noexcept {
        if (!enabled || visibility == nullptr ||
            node_index >= visibility_count) {
            return true;
        }
        return visibility[node_index] != 0u;
    }
};

/**
 * Scene-geometry pass identity used by the production submission policy.
 *
 * Main-view visibility is camera-relative. It is therefore valid only for
 * passes which render or query the active camera's image. Shadow casters use
 * light-space coverage and VXGI voxelization uses the complete world-space
 * scene, so masking either with the camera frustum would remove valid lighting.
 */
enum class ESceneGeometryPass : u8 {
    NormalDepthPrepass = 0u,
    MotionVectors,
    PbrOpaqueCount,
    PbrOpaqueDraw,
    InteractiveWaterDraw,
    RefractionPreflight,
    RefractionDraw,
    ShadowCaster,
    VxgiVoxelization,
    Count
};

/** Command form emitted by a pass after its eligibility query. */
enum class ESubmissionCommandForm : u8 {
    None = 0u,
    Draw,
    DrawIndexed,
    Dispatch
};

struct FSceneGeometryPassPolicy {
    bool uses_main_view_mask = false;
    ESubmissionCommandForm command_form =
        ESubmissionCommandForm::None;
};

constexpr FSceneGeometryPassPolicy SceneGeometryPassPolicy(
    ESceneGeometryPass pass) noexcept {
    switch (pass) {
    case ESceneGeometryPass::NormalDepthPrepass:
        return {true, ESubmissionCommandForm::Draw};
    case ESceneGeometryPass::MotionVectors:
    case ESceneGeometryPass::PbrOpaqueDraw:
    case ESceneGeometryPass::InteractiveWaterDraw:
    case ESceneGeometryPass::RefractionDraw:
        return {true, ESubmissionCommandForm::DrawIndexed};
    case ESceneGeometryPass::PbrOpaqueCount:
    case ESceneGeometryPass::RefractionPreflight:
        return {true, ESubmissionCommandForm::None};
    case ESceneGeometryPass::ShadowCaster:
        return {false, ESubmissionCommandForm::Draw};
    case ESceneGeometryPass::VxgiVoxelization:
        return {false, ESubmissionCommandForm::Dispatch};
    case ESceneGeometryPass::Count:
        break;
    }
    return {};
}

/**
 * Apply the production pass policy to the one shared main-view mask.
 *
 * Returning a disabled view is deliberately fail-open, which lets unmasked
 * light/world-space passes traverse the complete scene without owning a second
 * visibility buffer.
 */
inline FSubmissionMaskView SubmissionMaskForPass(
    ESceneGeometryPass pass,
    const FSubmissionMaskView& main_view_mask) noexcept {
    return SceneGeometryPassPolicy(pass).uses_main_view_mask
        ? main_view_mask : FSubmissionMaskView{};
}

/**
 * The aggregate dynamic VB is the exact full scene. When culling is disabled
 * or the already-published frame decision culled no nodes, preserve the
 * original one-Draw path instead of expanding it into one command per node.
 */
inline bool ShouldUseAggregateVertexDraw(
    const FSubmissionMaskView& mask,
    u32 explicitly_culled_count) noexcept {
    return !mask.enabled || explicitly_culled_count == 0u;
}

/**
 * Visit exactly the nodes submitted by camera-visible geometry passes.
 *
 * This is the production traversal used by normal/depth, motion-vector,
 * opaque PBR and refraction recording. Keeping the loop here gives tests a
 * real command-recording seam without constructing a GPU-backed editor host.
 */
template <typename FSubmit>
inline u32 ForEachSubmittedNode(
    const FSubmissionMaskView& mask, u32 node_count,
    FSubmit&& submit) noexcept(noexcept(submit(u32{}))) {
    u32 submitted = 0u;
    for (u32 node_index = 0u;
         node_index < node_count; ++node_index) {
        if (!mask.ShouldSubmit(node_index)) continue;
        submit(node_index);
        ++submitted;
    }
    return submitted;
}

/**
 * Coalesce adjacent vertex spans from submitted nodes.
 *
 * BuildSceneMeshVerts appends node geometry into one dynamic vertex buffer.
 * A culled node creates a gap, while consecutive visible nodes can safely
 * share one non-indexed Draw. Empty nodes do not split an otherwise contiguous
 * range. The return value is the number of emitted ranges/Draw calls.
 */
template <typename FVertexOffset, typename FVertexCount, typename FSubmitRange>
inline u32 ForEachSubmittedVertexRange(
    const FSubmissionMaskView& mask, u32 node_count,
    FVertexOffset&& vertex_offset,
    FVertexCount&& vertex_count,
    FSubmitRange&& submit_range) noexcept(
        noexcept(vertex_offset(u32{})) &&
        noexcept(vertex_count(u32{})) &&
        noexcept(submit_range(u32{}, u32{}))) {
    u32 emitted_ranges = 0u;
    u32 range_offset = 0u;
    u32 range_count = 0u;
    bool has_range = false;

    const auto flush = [&]() noexcept(
        noexcept(submit_range(u32{}, u32{}))) {
        if (!has_range) return;
        submit_range(range_offset, range_count);
        ++emitted_ranges;
        has_range = false;
        range_offset = 0u;
        range_count = 0u;
    };

    for (u32 node_index = 0u;
         node_index < node_count; ++node_index) {
        if (!mask.ShouldSubmit(node_index)) continue;
        const u32 offset = vertex_offset(node_index);
        const u32 count = vertex_count(node_index);
        if (count == 0u) continue;

        const bool range_end_valid =
            has_range &&
            range_count <= ~u32{0} - range_offset;
        const u32 range_end =
            range_end_valid ? range_offset + range_count : 0u;
        const bool adjacent =
            range_end_valid &&
            offset == range_end &&
            count <= ~u32{0} - range_end;
        if (!adjacent) {
            flush();
            range_offset = offset;
            range_count = count;
            has_range = true;
        } else {
            range_count += count;
        }
    }
    flush();
    return emitted_ranges;
}

/** Return true when any submitted node satisfies the supplied predicate. */
template <typename FPredicate>
inline bool AnySubmittedNode(
    const FSubmissionMaskView& mask, u32 node_count,
    FPredicate&& predicate) noexcept(noexcept(predicate(u32{}))) {
    for (u32 node_index = 0u;
         node_index < node_count; ++node_index) {
        if (!mask.ShouldSubmit(node_index)) continue;
        if (predicate(node_index)) return true;
    }
    return false;
}

} // namespace acs::editor_frustum_culling
