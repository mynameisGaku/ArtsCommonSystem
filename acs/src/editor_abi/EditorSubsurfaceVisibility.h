// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "editor_abi/EditorFrustumCulling.h"

namespace acs::editor_subsurface_visibility {

/**
 * Separates scene-wide SSSS warm-up from this frame's main-view workload.
 *
 * A material outside the camera still requests shader compilation so entering
 * the view never causes a cold visual transition. Full-resolution MRT and
 * diffusion work, however, is needed only when at least one SSSS material is
 * submitted by the main-view opaque pass.
 */
struct FPresence {
    bool scene_has_material = false;
    bool main_view_has_material = false;

    void Observe(
        u32 node_index,
        bool has_opaque_subsurface_material,
        bool eligible_for_main_view_draw,
        const editor_frustum_culling::FSubmissionMaskView&
            main_view_mask) noexcept {
        if (!has_opaque_subsurface_material) return;
        scene_has_material = true;
        if (eligible_for_main_view_draw &&
            main_view_mask.ShouldSubmit(node_index)) {
            main_view_has_material = true;
        }
    }

    bool Complete() const noexcept {
        return scene_has_material && main_view_has_material;
    }
};

} // namespace acs::editor_subsurface_visibility
