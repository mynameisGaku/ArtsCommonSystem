// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

#include <cstddef>

namespace acs::editor_cloud_workload {

/**
 * Additive, optional snapshot contract for the exact work submitted by the
 * volumetric-cloud renderer. This is deliberately separate from profiler v4.
 */
inline constexpr u32 kSnapshotVersion = 1u;
inline constexpr u32 kSnapshotSize = 168u;

enum ESnapshotFlags : u32 {
    Attempted               = 1u << 0u,
    Submitted               = 1u << 1u,
    HistoryWasAvailable     = 1u << 2u,
    HistoryReused           = 1u << 3u,
    HistoryInvalidated      = 1u << 4u,
    TemporalSuperResolution = 1u << 5u,
};

enum class ESkipReason : u32 {
    None = 0u,
    ResourcesNotReady = 1u,
    InvalidCamera = 2u,
    InvalidProjection = 3u,
};

#pragma pack(push, 4)
struct FSnapshot {
    u32 version = kSnapshotVersion;
    u32 struct_size = kSnapshotSize;
    u32 flags = 0u;
    u32 skip_reason = static_cast<u32>(ESkipReason::None);

    u64 profiler_frame_index = 0u;
    u64 submission_index = 0u;

    u32 trace_width = 0u;
    u32 trace_height = 0u;
    u32 output_width = 0u;
    u32 output_height = 0u;

    u32 steady_dispatches = 0u;
    u32 one_time_bake_dispatches = 0u;
    u32 shadow_cache_dispatches = 0u;
    u32 total_compute_dispatches = 0u;
    u32 composite_draws = 0u;
    u32 reserved0 = 0u;

    u64 trace_logical_invocations = 0u;
    u64 trace_launched_threads = 0u;
    u64 resolve_logical_invocations = 0u;
    u64 resolve_launched_threads = 0u;
    u64 one_time_bake_logical_invocations = 0u;
    u64 one_time_bake_launched_threads = 0u;
    u64 shadow_cache_logical_invocations = 0u;
    u64 shadow_cache_launched_threads = 0u;
    u64 total_logical_invocations = 0u;
    u64 total_launched_threads = 0u;
    u64 maximum_view_samples = 0u;
    u64 maximum_light_samples = 0u;
};
#pragma pack(pop)

static_assert(sizeof(FSnapshot) == kSnapshotSize,
              "Cloud workload ABI must match EditorCloudWorkloadSnapshot");
static_assert(offsetof(FSnapshot, profiler_frame_index) == 16u);
static_assert(offsetof(FSnapshot, trace_width) == 32u);
static_assert(offsetof(FSnapshot, trace_logical_invocations) == 72u);
static_assert(offsetof(FSnapshot, maximum_light_samples) == 160u);

} // namespace acs::editor_cloud_workload
