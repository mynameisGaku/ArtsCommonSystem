// SPDX-License-Identifier: Apache-2.0
#pragma once

namespace acs::editor_render_policy {

/**
 * Keep the local-fog consumer on exactly the same view/settings domain as its
 * volume producer.
 *
 * A previously generated texture can remain allocated after fog is disabled
 * or the viewport switches to orthographic mode.  Texture availability alone
 * therefore does not mean that the volume belongs to the current frame.
 */
constexpr bool ShouldCompositeLocalFog(
    bool orthographic_view, bool fog_enabled,
    bool volume_available, bool scene_depth_available,
    bool fullscreen_output_available) noexcept {
    return !orthographic_view && fog_enabled &&
           volume_available && scene_depth_available &&
           fullscreen_output_available;
}

/**
 * Use the existing analytic surface-fog fallback only when perspective fog was
 * requested but its current GPU volume is unavailable.
 *
 * Orthographic view never runs the volume producer.  Excluding it here avoids
 * making the 2D view depend on whether a stale perspective volume happens to
 * remain allocated.
 */
constexpr bool ShouldUseAnalyticLocalFog(
    bool orthographic_view, bool fog_enabled,
    bool volumetric_composite_available) noexcept {
    return !orthographic_view && fog_enabled &&
           !volumetric_composite_available;
}

} // namespace acs::editor_render_policy
