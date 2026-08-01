// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

#include <chrono>
#include <cmath>

namespace acs::editor_profiler {

// Version 5 appends native active-render/presentation timings. Version 4 is
// kept as a readable prefix so older editor binaries can continue polling the
// same DLL without being forced to consume fields they do not know about.
constexpr u32 kLegacySnapshotVersion = 4u;
constexpr u32 kLegacySnapshotSize = 224u;
constexpr u32 kSnapshotVersion = 5u;
constexpr u32 kSnapshotSize = 256u;
constexpr u32 kPeakWindowFrames = 120u;
// Roughly 1.5 seconds at the editor's typical 80 Hz render rate. A 30-query
// window proved too short to distinguish shader changes from scheduler noise.
constexpr u32 kGpuQueryWindowQueries = 120u;

enum class ETimingSource : u32 {
    CpuRecordSubmit = 1u,
    GpuTimestamp = 2u,
};

enum ESnapshotFlags : u32 {
    View3D = 1u << 0,
    Clouds = 1u << 1,
    Fog = 1u << 2,
    AerialPerspective = 1u << 3,
    GpuTimingsValid = 1u << 4,
    SceneMeshCacheRebuilt = 1u << 5,
    FrustumCullingEnabled = 1u << 6,
    GameView = 1u << 7,
    RuntimeSceneCamera = 1u << 8,
    ScenePresentationSuppressed = 1u << 9,
};

#pragma pack(push, 4)
struct FSnapshot {
    u32 version = kSnapshotVersion;
    u32 struct_size = kSnapshotSize;
    u32 timing_source = static_cast<u32>(ETimingSource::CpuRecordSubmit);
    u32 flags = 0u;

    u64 frame_index = 0u;
    u64 draw_calls = 0u;
    u64 dispatch_calls = 0u;
    u64 triangles = 0u;

    f32 fps = 0.0f;
    f32 cpu_frame_ms = 0.0f;
    f32 cpu_submit_ms = 0.0f;
    f32 gpu_frame_ms = -1.0f;

    f32 opaque_cpu_ms = 0.0f;
    f32 atmosphere_cpu_ms = 0.0f;
    f32 cloud_cpu_ms = 0.0f;
    f32 fog_cpu_ms = 0.0f;
    f32 post_cpu_ms = 0.0f;

    f32 opaque_gpu_ms = -1.0f;
    f32 atmosphere_gpu_ms = -1.0f;
    f32 cloud_gpu_ms = -1.0f;
    f32 fog_gpu_ms = -1.0f;
    f32 post_gpu_ms = -1.0f;

    u32 viewport_width = 0u;
    u32 viewport_height = 0u;
    u32 cloud_width = 0u;
    u32 cloud_height = 0u;
    u32 cloud_march_steps = 0u;
    u32 cloud_light_steps = 0u;
    f32 cloud_render_scale = 0.0f;

    u64 gpu_frame_index = 0u;
    f32 cpu_frame_peak_ms = 0.0f;
    f32 gpu_frame_peak_ms = -1.0f;
    u32 peak_window_frames = kPeakWindowFrames;
    u32 gpu_latency_frames = 0u;

    // These statistics are produced from unique, completed GPU timestamp
    // frames in native render order. Keeping the window here (rather than in
    // the 10 Hz WPF sampler) prevents repeated asynchronous results and
    // compositor sampling aliases from biasing the pass averages.
    u32 gpu_query_window_count = 0u;
    u32 gpu_query_window_capacity = kGpuQueryWindowQueries;
    f32 gpu_frame_average_ms = -1.0f;
    f32 opaque_gpu_average_ms = -1.0f;
    f32 atmosphere_gpu_average_ms = -1.0f;
    f32 cloud_gpu_average_ms = -1.0f;
    f32 fog_gpu_average_ms = -1.0f;
    f32 post_gpu_average_ms = -1.0f;
    f32 opaque_gpu_window_peak_ms = -1.0f;
    f32 atmosphere_gpu_window_peak_ms = -1.0f;
    f32 cloud_gpu_window_peak_ms = -1.0f;
    f32 fog_gpu_window_peak_ms = -1.0f;
    f32 post_gpu_window_peak_ms = -1.0f;

    // Exact per-frame main-view culling workload. Counts cover eligible
    // authored 3D render nodes once, not the number of downstream passes.
    u32 frustum_tested = 0u;
    u32 frustum_visible = 0u;
    u32 frustum_culled = 0u;
    i32 active_camera_node_id = -1;

    // CPU residency after the cooperative GPU-ready preflight. Active render
    // excludes submit/Present; present_cpu_ms contains that terminal interval.
    // Peaks and the presented count are reset at an explicit capture boundary,
    // so startup warm-up cannot contaminate a benchmark capture.
    f32 native_render_active_cpu_ms = 0.0f;
    f32 native_present_cpu_ms = 0.0f;
    f32 native_render_active_cpu_peak_ms = 0.0f;
    f32 native_present_cpu_peak_ms = 0.0f;
    u64 presented_frame_count_since_reset = 0u;
    u64 profiler_reset_serial = 0u;
};
#pragma pack(pop)

static_assert(sizeof(FSnapshot) == kSnapshotSize,
              "Editor profiler ABI must match EditorProfilerSnapshot");

struct FAccumulator {
    f32 opaque_cpu_ms = 0.0f;
    f32 atmosphere_cpu_ms = 0.0f;
    f32 cloud_cpu_ms = 0.0f;
    f32 fog_cpu_ms = 0.0f;
    f32 post_cpu_ms = 0.0f;
    bool clouds_active = false;
    bool scene_mesh_cache_rebuilt = false;
    bool frustum_culling_enabled = false;
    bool runtime_scene_camera = false;
    bool render_orthographic = false;
    bool render_camera_resolved = false;
    u32 frustum_tested = 0u;
    u32 frustum_visible = 0u;
    u32 frustum_culled = 0u;
    i32 active_camera_node_id = -1;
};

/** Small allocation-free rolling maximum used to preserve sub-sample spikes. */
class FRollingPeak {
public:
    void Reset() noexcept {
        for (u32 index = 0; index < kPeakWindowFrames; ++index)
            m_Values[index] = 0.0f;
        m_Cursor = 0;
        m_Count = 0;
    }

    bool Add(f32 value) noexcept {
        if (!std::isfinite(value) || value < 0.0f) return false;
        m_Values[m_Cursor] = value;
        m_Cursor = (m_Cursor + 1u) % kPeakWindowFrames;
        if (m_Count < kPeakWindowFrames) ++m_Count;
        return true;
    }

    bool HasValues() const noexcept { return m_Count != 0u; }

    f32 Peak() const noexcept {
        f32 peak = 0.0f;
        for (u32 index = 0; index < m_Count; ++index)
            if (m_Values[index] > peak) peak = m_Values[index];
        return peak;
    }

private:
    f32 m_Values[kPeakWindowFrames]{};
    u32 m_Cursor = 0u;
    u32 m_Count = 0u;
};

struct FGpuQuerySample {
    f32 frame_ms = -1.0f;
    f32 opaque_ms = -1.0f;
    f32 atmosphere_ms = -1.0f;
    f32 cloud_ms = -1.0f;
    f32 fog_ms = -1.0f;
    f32 post_ms = -1.0f;
};

struct FGpuQueryWindowStatistics {
    u32 count = 0u;
    f32 frame_average_ms = -1.0f;
    f32 opaque_average_ms = -1.0f;
    f32 atmosphere_average_ms = -1.0f;
    f32 cloud_average_ms = -1.0f;
    f32 fog_average_ms = -1.0f;
    f32 post_average_ms = -1.0f;
    f32 opaque_peak_ms = -1.0f;
    f32 atmosphere_peak_ms = -1.0f;
    f32 cloud_peak_ms = -1.0f;
    f32 fog_peak_ms = -1.0f;
    f32 post_peak_ms = -1.0f;
};

/**
 * Allocation-free window of unique, fully valid GPU timestamp results.
 *
 * The command-list query API intentionally returns its most recently
 * completed result until a newer frame is ready. Frame-index de-duplication is
 * therefore part of this accumulator rather than a caller convention.
 */
class FRollingGpuQueryWindow {
public:
    void Reset(u64 already_consumed_frame = 0u) noexcept {
        for (u32 index = 0; index < kGpuQueryWindowQueries; ++index)
            m_Values[index] = {};
        m_Cursor = 0u;
        m_Count = 0u;
        m_LastFrameIndex = already_consumed_frame;
        m_HasLastFrame = already_consumed_frame != 0u;
    }

    bool Add(u64 frame_index, const FGpuQuerySample& sample) noexcept {
        if (frame_index == 0u ||
            (m_HasLastFrame && frame_index <= m_LastFrameIndex)) {
            return false;
        }

        // Consume malformed results once as well. A query result is immutable;
        // retrying the same frame on every render would only hide the fault and
        // could later give it disproportionate weight.
        m_LastFrameIndex = frame_index;
        m_HasLastFrame = true;
        if (!IsValid(sample)) return false;

        m_Values[m_Cursor] = sample;
        m_Cursor = (m_Cursor + 1u) % kGpuQueryWindowQueries;
        if (m_Count < kGpuQueryWindowQueries) ++m_Count;
        return true;
    }

    bool HasValues() const noexcept { return m_Count != 0u; }
    u32 Count() const noexcept { return m_Count; }

    FGpuQueryWindowStatistics Statistics() const noexcept {
        FGpuQueryWindowStatistics out{};
        if (m_Count == 0u) return out;

        double frame_sum = 0.0;
        double opaque_sum = 0.0;
        double atmosphere_sum = 0.0;
        double cloud_sum = 0.0;
        double fog_sum = 0.0;
        double post_sum = 0.0;
        f32 opaque_peak = 0.0f;
        f32 atmosphere_peak = 0.0f;
        f32 cloud_peak = 0.0f;
        f32 fog_peak = 0.0f;
        f32 post_peak = 0.0f;
        for (u32 index = 0; index < m_Count; ++index) {
            const FGpuQuerySample& value = m_Values[index];
            frame_sum += value.frame_ms;
            opaque_sum += value.opaque_ms;
            atmosphere_sum += value.atmosphere_ms;
            cloud_sum += value.cloud_ms;
            fog_sum += value.fog_ms;
            post_sum += value.post_ms;
            if (value.opaque_ms > opaque_peak) opaque_peak = value.opaque_ms;
            if (value.atmosphere_ms > atmosphere_peak)
                atmosphere_peak = value.atmosphere_ms;
            if (value.cloud_ms > cloud_peak) cloud_peak = value.cloud_ms;
            if (value.fog_ms > fog_peak) fog_peak = value.fog_ms;
            if (value.post_ms > post_peak) post_peak = value.post_ms;
        }

        const double inv_count = 1.0 / static_cast<double>(m_Count);
        out.count = m_Count;
        out.frame_average_ms = static_cast<f32>(frame_sum * inv_count);
        out.opaque_average_ms = static_cast<f32>(opaque_sum * inv_count);
        out.atmosphere_average_ms =
            static_cast<f32>(atmosphere_sum * inv_count);
        out.cloud_average_ms = static_cast<f32>(cloud_sum * inv_count);
        out.fog_average_ms = static_cast<f32>(fog_sum * inv_count);
        out.post_average_ms = static_cast<f32>(post_sum * inv_count);
        out.opaque_peak_ms = opaque_peak;
        out.atmosphere_peak_ms = atmosphere_peak;
        out.cloud_peak_ms = cloud_peak;
        out.fog_peak_ms = fog_peak;
        out.post_peak_ms = post_peak;
        return out;
    }

private:
    static bool IsValid(const FGpuQuerySample& sample) noexcept {
        const auto valid = [](f32 value) noexcept {
            return std::isfinite(value) && value >= 0.0f;
        };
        return valid(sample.frame_ms) && valid(sample.opaque_ms) &&
               valid(sample.atmosphere_ms) && valid(sample.cloud_ms) &&
               valid(sample.fog_ms) && valid(sample.post_ms);
    }

    FGpuQuerySample m_Values[kGpuQueryWindowQueries]{};
    u64 m_LastFrameIndex = 0u;
    u32 m_Cursor = 0u;
    u32 m_Count = 0u;
    bool m_HasLastFrame = false;
};

using CClock = std::chrono::steady_clock;
using FTimePoint = CClock::time_point;

inline f32 ElapsedMilliseconds(FTimePoint begin) noexcept {
    return static_cast<f32>(
        std::chrono::duration<double, std::milli>(
            CClock::now() - begin).count());
}

class FCpuScope {
public:
    explicit FCpuScope(f32& destination) noexcept
        : m_Destination(destination), m_Begin(CClock::now()) {}

    ~FCpuScope() noexcept {
        m_Destination += ElapsedMilliseconds(m_Begin);
    }

    FCpuScope(const FCpuScope&) = delete;
    FCpuScope& operator=(const FCpuScope&) = delete;

private:
    f32& m_Destination;
    FTimePoint m_Begin;
};

} // namespace acs::editor_profiler
