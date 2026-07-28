// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"

namespace acs {

/**
 * Per-frame inputs shared by temporal reconstruction passes.
 *
 * Frame zero is the first frame after initialization, resize, camera change,
 * or an explicit history invalidation. It must never reproject or blend an
 * older camera's samples, even when the caller still has a previous matrix or
 * motion texture available.
 */
struct FTemporalHistoryFramePolicy {
    FMat4 previous_view_projection{};
    f32 current_frame_weight = 1.0f;
    bool motion_vectors_enabled = false;
};

/**
 * Select safe temporal inputs without owning any history resources.
 *
 * Cold-start frames use the current view-projection as "previous", accept the
 * current sample at full weight, and disable motion-vector sampling. Warm
 * frames preserve all caller-supplied values verbatim.
 */
inline FTemporalHistoryFramePolicy ResolveTemporalHistoryFrame(
    u32 temporal_frame,
    const FMat4& current_view_projection,
    const FMat4& supplied_previous_view_projection,
    f32 configured_current_frame_weight,
    bool motion_vectors_available) noexcept {
    if (temporal_frame == 0u) {
        return FTemporalHistoryFramePolicy{
            current_view_projection,
            1.0f,
            false};
    }
    return FTemporalHistoryFramePolicy{
        supplied_previous_view_projection,
        configured_current_frame_weight,
        motion_vectors_available};
}

} // namespace acs
