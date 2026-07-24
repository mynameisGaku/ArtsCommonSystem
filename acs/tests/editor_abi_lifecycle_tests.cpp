// SPDX-License-Identifier: Apache-2.0
// Editor ABI DLL の生成・破棄契約を、GPU 接続なしで実 DLL 境界から検証する。
#include "editor_abi/EditorProfiler.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

extern "C" __declspec(dllimport) void* acs_editor_create(void);
extern "C" __declspec(dllimport) void acs_editor_destroy(void* handle);
extern "C" __declspec(dllimport) int acs_editor_node_count(void* handle);
extern "C" __declspec(dllimport) int acs_editor_add_node3d(
    void* handle, int primitive, const char* name);
extern "C" __declspec(dllimport) int acs_editor_add_empty3d(
    void* handle, const char* name);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_transform(
    void* handle, int id, float px, float py, float pz, float rx, float ry, float rz,
    float sx, float sy, float sz);
extern "C" __declspec(dllimport) int acs_editor_node3d_get_transform(
    void* handle, int id, float* out9);
extern "C" __declspec(dllimport) int acs_editor_scene3d_serialize(
    void* handle, char* out, int capacity);
extern "C" __declspec(dllimport) int acs_editor_scene3d_load_text(
    void* handle, const char* text);
extern "C" __declspec(dllimport) const char* acs_editor_scene_serialize(
    void* handle);
extern "C" __declspec(dllimport) int acs_editor_scene_load_text(
    void* handle, const char* text);
extern "C" __declspec(dllimport) int acs_editor_scene_document_load_text(
    void* handle, const char* scene2d_text, const char* scene3d_text);
extern "C" __declspec(dllimport) int acs_editor_node3d_duplicate(
    void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_reparent3d(
    void* handle, int child_id, int parent_id);
extern "C" __declspec(dllimport) void acs_editor_node3d_set_visible(
    void* handle, int id, int visible);
extern "C" __declspec(dllimport) void acs_editor_node3d_set_enabled(
    void* handle, int id, int enabled);
extern "C" __declspec(dllimport) int acs_editor_node3d_add_component(
    void* handle, int id, const char* type_name);
extern "C" __declspec(dllimport) int acs_editor_node3d_component_prop_get(
    void* handle, int id, int slot, int prop,
    float* x, float* y, float* z, float* w);
extern "C" __declspec(dllimport) int acs_editor_node3d_component_prop_set(
    void* handle, int id, int slot, int prop,
    float x, float y, float z, float w);
extern "C" __declspec(dllimport) int acs_editor_component_prop_count(
    const char* type_name);
extern "C" __declspec(dllimport) int acs_editor_component_prop_flags_at(
    const char* type_name, int index);
extern "C" __declspec(dllimport) const char*
acs_editor_component_prop_name_at(const char* type_name, int index);
extern "C" __declspec(dllimport) int acs_editor_water3d_hit_test(
    void* handle, float sx, float sy,
    float viewport_width, float viewport_height,
    int* node_id, float* world_x,
    float* world_y, float* world_z);
extern "C" __declspec(dllimport) void acs_editor_set_view3d(void* handle, int on);
extern "C" __declspec(dllimport) void acs_editor_scene3d_new(void* handle);
extern "C" __declspec(dllimport) int acs_editor_camera3d_set(
    void* handle, float yaw, float pitch, float distance,
    float target_x, float target_y, float target_z);
extern "C" __declspec(dllimport) int acs_editor_camera3d_get(
    void* handle, float* yaw, float* pitch, float* distance,
    float* target_x, float* target_y, float* target_z);
extern "C" __declspec(dllimport) void acs_editor_camera_frame_all(void* handle);
extern "C" __declspec(dllimport) int acs_editor_profiler_get(
    void* handle, acs::editor_profiler::FSnapshot* out_snapshot,
    unsigned out_size);
extern "C" __declspec(dllimport) void acs_editor_profiler_reset_peaks(
    void* handle);
extern "C" __declspec(dllimport) int acs_editor_startup_status(
    void* handle, unsigned* completed, unsigned* total);
extern "C" __declspec(dllimport) void acs_editor_set_scene_presentation_suppressed(
    void* handle, int suppressed);
extern "C" __declspec(dllimport) int acs_editor_attach(
    void* handle, void* hwnd, unsigned width, unsigned height);
extern "C" __declspec(dllimport) void acs_editor_render(
    void* handle, float dt);

namespace {

/** 現在プロセスが保持する Win32 HANDLE 数を返す。 */
DWORD ProcessHandleCount() noexcept
{
    DWORD count = 0;
    return ::GetProcessHandleCount(::GetCurrentProcess(), &count) ? count : 0;
}

/** A production host starts blank and can enter/leave the loading presentation gate. */
bool RunOneLifecycle() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const bool scene_ready = acs_editor_node_count(host) == 0;
    acs_editor_set_scene_presentation_suppressed(host, 1);
    acs_editor_set_scene_presentation_suppressed(host, 0);
    acs_editor_destroy(host);
    return scene_ready;
}

/** Startup warm-up is observable without doing GPU work or mutating the host. */
bool RunStartupStatusContract() noexcept
{
    unsigned completed = 99u;
    unsigned total = 0u;
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const bool waiting_for_attach =
        acs_editor_startup_status(host, &completed, &total) == 0 &&
        completed == 0u && total > 1u;
    const bool optional_outputs =
        acs_editor_startup_status(host, nullptr, nullptr) == 0;
    acs_editor_destroy(host);

    completed = 99u;
    total = 0u;
    const bool rejects_null =
        acs_editor_startup_status(nullptr, &completed, &total) < 0 &&
        completed == 0u && total > 1u;
    return waiting_for_attach && optional_outputs && rejects_null;
}

/** Destroying during shader warm-up must finish/release work before device teardown. */
bool RunDestroyDuringAsyncWarmup() noexcept
{
    HWND const window = ::CreateWindowExW(
        0, L"STATIC", L"ACS editor ABI async teardown test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 320, 240,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) return true; // No desktop in this test environment.

    void* const host = acs_editor_create();
    if (host == nullptr) {
        ::DestroyWindow(window);
        return false;
    }
    if (acs_editor_attach(host, window, 320u, 240u) == 0) {
        // A machine without a usable DX12 adapter still exercises the ordinary
        // unattached destroy contract; async teardown is skipped there.
        acs_editor_destroy(host);
        ::DestroyWindow(window);
        return true;
    }

    unsigned completed = 0u;
    unsigned total = 0u;
    int startup_state =
        acs_editor_startup_status(host, &completed, &total);

#if !ACS_EDITOR_ABI_EXPECTS_ASYNC_WARMUP
    // A backend without an asynchronous compiler still has an attached,
    // incremental startup lifecycle to tear down.
    const bool startup_pending =
        startup_state == 0 && completed < total;
    acs_editor_destroy(host);
    ::DestroyWindow(window);
    return startup_pending;
#else
    bool observed_progress = false;
    bool observed_pending_worker = false;
    for (unsigned pump = 0u;
         pump < 128u && startup_state == 0 && !observed_pending_worker;
         ++pump) {
        const unsigned previous_completed = completed;
        acs_editor_render(host, 1.0f / 60.0f);
        startup_state =
            acs_editor_startup_status(host, &completed, &total);
        if (completed > previous_completed) {
            observed_progress = true;
        } else if (observed_progress && completed == previous_completed &&
                   completed < total) {
            // A startup call that returns without advancing the public progress
            // boundary is the observable contract for either the raw-DX12
            // worker or a backend-managed shader compiler.
            observed_pending_worker = true;
        }
    }
    acs_editor_destroy(host);
    ::DestroyWindow(window);
    return startup_state == 0 && observed_progress &&
           observed_pending_worker && completed < total;
#endif
}

/** Versioned profiler ABI rejects incompatible callers and preserves extension bytes. */
bool RunProfilerSnapshotContract() noexcept
{
    using namespace acs::editor_profiler;
    static_assert(sizeof(FSnapshot) == kSnapshotSize);
    static_assert(kSnapshotVersion == 3u);
    static_assert(kSnapshotSize == 208u);
    static_assert(
        SceneMeshCacheRebuilt == (1u << 5u),
        "mesh-cache rebuild profiling must not change the snapshot ABI");

    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    FSnapshot snapshot{};
    const bool default_snapshot =
        acs_editor_profiler_get(
            host, &snapshot, static_cast<unsigned>(sizeof(snapshot))) != 0 &&
        snapshot.version == kSnapshotVersion &&
        snapshot.struct_size == kSnapshotSize &&
        snapshot.timing_source ==
            static_cast<acs::u32>(ETimingSource::CpuRecordSubmit) &&
        snapshot.frame_index == 0u &&
        snapshot.gpu_frame_ms < 0.0f &&
        snapshot.opaque_gpu_ms < 0.0f &&
        snapshot.atmosphere_gpu_ms < 0.0f &&
        snapshot.cloud_gpu_ms < 0.0f &&
        snapshot.fog_gpu_ms < 0.0f &&
        snapshot.post_gpu_ms < 0.0f &&
        snapshot.gpu_frame_index == 0u &&
        snapshot.cpu_frame_peak_ms == 0.0f &&
        snapshot.gpu_frame_peak_ms < 0.0f &&
        snapshot.peak_window_frames == kPeakWindowFrames &&
        snapshot.gpu_latency_frames == 0u &&
        snapshot.gpu_query_window_count == 0u &&
        snapshot.gpu_query_window_capacity == kGpuQueryWindowQueries &&
        snapshot.gpu_frame_average_ms < 0.0f &&
        snapshot.opaque_gpu_average_ms < 0.0f &&
        snapshot.atmosphere_gpu_average_ms < 0.0f &&
        snapshot.cloud_gpu_average_ms < 0.0f &&
        snapshot.fog_gpu_average_ms < 0.0f &&
        snapshot.post_gpu_average_ms < 0.0f &&
        snapshot.opaque_gpu_window_peak_ms < 0.0f &&
        snapshot.atmosphere_gpu_window_peak_ms < 0.0f &&
        snapshot.cloud_gpu_window_peak_ms < 0.0f &&
        snapshot.fog_gpu_window_peak_ms < 0.0f &&
        snapshot.post_gpu_window_peak_ms < 0.0f &&
        (snapshot.flags &
         (GpuTimingsValid | SceneMeshCacheRebuilt)) == 0u;

    snapshot.version = kSnapshotVersion + 1u;
    const bool rejects_version =
        acs_editor_profiler_get(
            host, &snapshot, static_cast<unsigned>(sizeof(snapshot))) == 0;
    snapshot.version = kSnapshotVersion;
    snapshot.struct_size = kSnapshotSize - 1u;
    const bool rejects_struct_size =
        acs_editor_profiler_get(
            host, &snapshot, static_cast<unsigned>(sizeof(snapshot))) == 0;
    snapshot.struct_size = kSnapshotSize;
    const bool rejects_buffer_size =
        acs_editor_profiler_get(host, &snapshot, kSnapshotSize - 1u) == 0;

    struct FExtendedSnapshot {
        FSnapshot base{};
        unsigned extension_sentinel = 0xA5A55A5Au;
    } extended;
    extended.base.struct_size = sizeof(extended);
    const bool forward_prefix =
        acs_editor_profiler_get(
            host, &extended.base,
            static_cast<unsigned>(sizeof(extended))) != 0 &&
        extended.base.struct_size == kSnapshotSize &&
        extended.extension_sentinel == 0xA5A55A5Au;

    const bool rejects_null =
        acs_editor_profiler_get(host, nullptr, kSnapshotSize) == 0 &&
        acs_editor_profiler_get(
            nullptr, &extended.base,
            static_cast<unsigned>(sizeof(extended))) == 0;

    FRollingPeak peak;
    const bool rolling_peak =
        !peak.HasValues() &&
        !peak.Add(std::numeric_limits<float>::quiet_NaN()) &&
        peak.Add(4.0f) &&
        peak.Add(11.0f) &&
        peak.Add(7.0f) &&
        peak.HasValues() &&
        peak.Peak() == 11.0f;
    peak.Reset();
    const bool peak_reset =
        !peak.HasValues() && peak.Peak() == 0.0f;

    FRollingGpuQueryWindow gpu_window;
    const FGpuQuerySample valid_query{
        8.0f, 1.0f, 2.0f, 3.0f, 0.5f, 1.5f};
    FGpuQuerySample invalid_query = valid_query;
    invalid_query.cloud_ms = std::numeric_limits<float>::quiet_NaN();
    const bool query_validity_and_identity =
        !gpu_window.Add(0u, valid_query) &&
        gpu_window.Add(1u, valid_query) &&
        !gpu_window.Add(1u, valid_query) &&
        !gpu_window.Add(2u, invalid_query) &&
        gpu_window.Count() == 1u;

    gpu_window.Reset();
    bool filled_window = true;
    for (acs::u64 frame = 1u;
         frame <= static_cast<acs::u64>(kGpuQueryWindowQueries + 1u);
         ++frame) {
        const float value = static_cast<float>(frame);
        filled_window = filled_window && gpu_window.Add(
            frame,
            FGpuQuerySample{
                value,
                value * 0.1f,
                value * 0.2f,
                value * 0.3f,
                value * 0.4f,
                value * 0.5f});
    }
    const FGpuQueryWindowStatistics gpu_stats = gpu_window.Statistics();
    const float expected_average =
        (static_cast<float>(kGpuQueryWindowQueries) + 3.0f) * 0.5f;
    const float expected_peak =
        static_cast<float>(kGpuQueryWindowQueries + 1u);
    const bool query_window =
        filled_window &&
        gpu_stats.count == kGpuQueryWindowQueries &&
        std::abs(gpu_stats.frame_average_ms - expected_average) < 0.001f &&
        std::abs(gpu_stats.opaque_average_ms - expected_average * 0.1f) < 0.001f &&
        std::abs(gpu_stats.atmosphere_average_ms - expected_average * 0.2f) < 0.001f &&
        std::abs(gpu_stats.cloud_average_ms - expected_average * 0.3f) < 0.001f &&
        std::abs(gpu_stats.fog_average_ms - expected_average * 0.4f) < 0.001f &&
        std::abs(gpu_stats.post_average_ms - expected_average * 0.5f) < 0.001f &&
        std::abs(gpu_stats.opaque_peak_ms - expected_peak * 0.1f) < 0.001f &&
        std::abs(gpu_stats.atmosphere_peak_ms - expected_peak * 0.2f) < 0.001f &&
        std::abs(gpu_stats.cloud_peak_ms - expected_peak * 0.3f) < 0.001f &&
        std::abs(gpu_stats.fog_peak_ms - expected_peak * 0.4f) < 0.001f &&
        std::abs(gpu_stats.post_peak_ms - expected_peak * 0.5f) < 0.001f;

    gpu_window.Reset(kGpuQueryWindowQueries + 1u);
    const bool query_reset =
        !gpu_window.HasValues() &&
        gpu_window.Count() == 0u &&
        !gpu_window.Add(kGpuQueryWindowQueries + 1u, valid_query) &&
        gpu_window.Statistics().frame_average_ms < 0.0f;
    acs_editor_profiler_reset_peaks(host);
    acs_editor_profiler_reset_peaks(nullptr);

    acs_editor_destroy(host);
    return default_snapshot && rejects_version && rejects_struct_size &&
           rejects_buffer_size && forward_prefix && rejects_null &&
           rolling_peak && peak_reset && query_validity_and_identity &&
           query_window && query_reset;
}

/** Exact zero scale is made invertible before normal-matrix consumers see the transform. */
bool RunZeroScaleSafety() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const int id = acs_editor_add_node3d(host, 0, "ZeroScaleRegression");
    float transform[9]{};
    const bool ok =
        id >= 0 &&
        acs_editor_node3d_set_transform(
            host, id, 1.0f, 2.0f, 3.0f, 10.0f, 20.0f, 30.0f, 0.0f, -0.0f, 0.0f) != 0 &&
        acs_editor_node3d_get_transform(host, id, transform) != 0 &&
        std::isfinite(transform[6]) && std::isfinite(transform[7]) && std::isfinite(transform[8]) &&
        std::abs(transform[6]) >= 1.0e-4f &&
        std::abs(transform[7]) >= 1.0e-4f &&
        std::abs(transform[8]) >= 1.0e-4f;
    acs_editor_destroy(host);
    return ok;
}

/** A too-small output is explicitly reported and must never look like a valid partial scene. */
bool RunScene3DSerializationGrowth() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const int id = acs_editor_add_node3d(host, 0, "SerializationGrowthRegression");
    char too_small[16]{};
    const int overflow = acs_editor_scene3d_serialize(
        host, too_small, static_cast<int>(sizeof(too_small)));
    char complete[4096]{};
    const int written = acs_editor_scene3d_serialize(
        host, complete, static_cast<int>(sizeof(complete)));
    const bool ok =
        id >= 0 &&
        overflow >= static_cast<int>(sizeof(too_small)) &&
        too_small[0] == '\0' &&
        written > 0 && written < static_cast<int>(sizeof(complete)) &&
        static_cast<size_t>(written) == std::strlen(complete) &&
        std::strstr(complete, "ACS3D v2\n") == complete &&
        std::strstr(complete, "N3D ") != nullptr;
    acs_editor_destroy(host);
    return ok;
}

bool SnapshotSceneDocument(
    void* host, std::string& scene2d, std::string& scene3d) noexcept
{
    const char* const text2d = acs_editor_scene_serialize(host);
    if (text2d == nullptr) return false;
    scene2d = text2d;
    char text3d[32768]{};
    const int written = acs_editor_scene3d_serialize(
        host, text3d, static_cast<int>(sizeof(text3d)));
    if (written <= 0 || written >= static_cast<int>(sizeof(text3d)) ||
        static_cast<size_t>(written) != std::strlen(text3d)) {
        return false;
    }
    scene3d = text3d;
    return true;
}

bool SceneDocumentEquals(
    void* host, const std::string& expected2d,
    const std::string& expected3d) noexcept
{
    std::string current2d;
    std::string current3d;
    return SnapshotSceneDocument(host, current2d, current3d) &&
           current2d == expected2d && current3d == expected3d;
}

/**
 * A rejected compatibility payload never retires either half of the singular
 * editor scene document. Parent, auxiliary, and component declaration order
 * is intentionally independent for a valid ACS3D document.
 */
bool RunSceneDocumentStrictPreflight() noexcept
{
    constexpr const char* kValid2D =
        "ACSCENE v1\n"
        "1\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n"
        "SEL 7 1 7\n";
    constexpr const char* kValid3D =
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "SEL3D 41\n";
    constexpr const char* kNode2D =
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n";
    constexpr const char* kNode3D =
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    bool ok =
        acs_editor_scene_document_load_text(host, kValid2D, kValid3D) != 0;
    std::string expected2d;
    std::string expected3d;
    ok = ok && SnapshotSceneDocument(host, expected2d, expected3d);

    const char* const invalid2d[] = {
        "ACSCENE v1\nnot-a-count\n",
        "ACSCENE v1\n2\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 OnlyOne\n",
        "ACSCENE v1\n1\n7 -1 0\n",
        "ACSCENE v1\n1\n"
        "7 -1 nan 34 0 1 1 48 0.5 0.6 0.7 1 NonFinite\n",
        "ACSCENE v1\n1\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n"
        "COMP 999 AWaterSurface3DComponent\n",
        "ACSCENE v1\n1\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n"
        "NFLG 7 1 nope\n",
        "ACSCENE v1\n2\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 DuplicateA\n"
        "7 -1 56 78 0 1 1 48 0.5 0.6 0.7 1 DuplicateB\n",
        "ACSCENE v1\n1\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n"
        "COMP\t7 AWaterSurface3DComponent\n",
        "ACSCENE v1\n1\n"
        "7 -1 12 34 0 1 1 48 0.5 0.6 0.7 1 Stable2D\n"
        "COMP 7 AWaterSurface3DComponent\n"
        "CPROP 7 0 20 1 2 3 4\n",
    };
    for (const char* invalid : invalid2d) {
        ok = ok && acs_editor_scene_load_text(host, invalid) == 0 &&
             SceneDocumentEquals(host, expected2d, expected3d);
        ok = ok &&
             acs_editor_scene_document_load_text(host, invalid, kValid3D) == 0 &&
             SceneDocumentEquals(host, expected2d, expected3d);
    }

    const std::string valid2d = std::string("ACSCENE v1\n1\n") + kNode2D;
    const std::string valid3d = std::string("ACS3D v2\n") + kNode3D;
    const char* const invalid3d[] = {
        "ACS3D v2\nN3D 41 -1 0 1 2\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 A\n"
        "N3D 41 -1 0 4 5 6 0 0 0 1 1 1 0.2 0.3 0.4 1 B\n",
        "ACS3D v2\n"
        "N3D 41 999 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 MissingParent\n",
        "ACS3D v2\n"
        "N3D 41 42 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 CycleA\n"
        "N3D 42 41 0 4 5 6 0 0 0 1 1 1 0.2 0.3 0.4 1 CycleB\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 nan 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 NonFinite\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "CMP3D 999 AWaterSurface3DComponent\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "FLG3D 41 1 nope\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "SEL3D 999\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "SEL3D -1\n",
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Stable3D\n"
        "CMP3D\t41 AWaterSurface3DComponent\n",
    };
    for (const char* invalid : invalid3d) {
        ok = ok && acs_editor_scene3d_load_text(host, invalid) == 0 &&
             SceneDocumentEquals(host, expected2d, expected3d);
        ok = ok &&
             acs_editor_scene_document_load_text(
                 host, valid2d.c_str(), invalid) == 0 &&
             SceneDocumentEquals(host, expected2d, expected3d);
    }

    constexpr const char* kOrderIndependent3D =
        "ACS3D v2\r\n"
        "CPROP3D 77 0 0 3.25 4.5 5.75 7\r\n"
        "CMP3D 77 AWaterSurface3DComponent\r\n"
        "SEL3D 77\r\n"
        "FLG3D 77 0 1\r\n"
        "N3D 78 77 0 4 5 6 0 0 0 1 1 1 0.3 0.4 0.5 1 ChildFirst\r\n"
        "N3D 77 -1 2 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 ParentLast\r\n";
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float w = 0.0f;
    ok = ok &&
         acs_editor_scene3d_load_text(host, kOrderIndependent3D) != 0 &&
         acs_editor_node3d_component_prop_get(
             host, 77, 0, 0, &x, &y, &z, &w) != 0 &&
         std::abs(x - 3.25f) < 1.0e-6f &&
         std::abs(y - 4.5f) < 1.0e-6f &&
         std::abs(z - 5.75f) < 1.0e-6f &&
         std::abs(w - 7.0f) < 1.0e-6f;

    // Zero is the only explicit no-selection sentinel accepted by ACS3D.
    ok = ok &&
         acs_editor_scene3d_load_text(
             host, "ACS3D v2\r\nSEL3D 0\r\n") != 0;
    acs_editor_destroy(host);
    return ok;
}

/** Interactive-water's complete reflected contract survives duplicate/save/load. */
bool RunWater3DComponentRoundTrip() noexcept
{
    constexpr const char* kType = "AWaterSurface3DComponent";
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const int id = acs_editor_add_node3d(host, 2, "WaterRoundTrip");
    bool ok =
        id >= 0 &&
        acs_editor_component_prop_count(kType) == 20 &&
        std::strcmp(
            acs_editor_component_prop_name_at(kType, 4),
            "roughness") == 0 &&
        std::strcmp(
            acs_editor_component_prop_name_at(kType, 18),
            "phaseAnisotropy") == 0 &&
        std::strcmp(
            acs_editor_component_prop_name_at(kType, 19),
            "foamColor") == 0 &&
        (acs_editor_component_prop_flags_at(kType, 0) & (1u << 3u)) != 0 &&
        (acs_editor_component_prop_flags_at(kType, 1) & (1u << 3u)) != 0 &&
        (acs_editor_component_prop_flags_at(kType, 2) & (1u << 3u)) == 0 &&
        (acs_editor_component_prop_flags_at(kType, 19) & (1u << 3u)) != 0 &&
        acs_editor_node3d_add_component(host, id, kType) != 0;
    for (int property = 0; property < 20 && ok; ++property) {
        const float base = static_cast<float>(property + 1);
        ok = acs_editor_node3d_component_prop_set(
                 host, id, 0, property,
                 base, base + 0.125f, base + 0.25f, base + 0.5f) != 0;
    }

    const int duplicate = ok
        ? acs_editor_node3d_duplicate(host, id)
        : -1;
    for (int property = 0; property < 20 && ok; ++property) {
        float x = 0, y = 0, z = 0, w = 0;
        const float base = static_cast<float>(property + 1);
        ok = duplicate >= 0 &&
             acs_editor_node3d_component_prop_get(
                 host, duplicate, 0, property, &x, &y, &z, &w) != 0 &&
             std::abs(x - base) < 1.0e-6f &&
             std::abs(y - (base + 0.125f)) < 1.0e-6f &&
             std::abs(z - (base + 0.25f)) < 1.0e-6f &&
             std::abs(w - (base + 0.5f)) < 1.0e-6f;
    }

    char scene[32768]{};
    const int written = ok
        ? acs_editor_scene3d_serialize(
              host, scene, static_cast<int>(sizeof(scene)))
        : 0;
    ok = ok && written > 0 &&
         written < static_cast<int>(sizeof(scene)) &&
         std::strstr(scene, "CPROP3D ") != nullptr &&
         acs_editor_scene3d_load_text(host, scene) != 0;
    for (int property = 0; property < 20 && ok; ++property) {
        float x = 0, y = 0, z = 0, w = 0;
        const float base = static_cast<float>(property + 1);
        ok = acs_editor_node3d_component_prop_get(
                 host, id, 0, property, &x, &y, &z, &w) != 0 &&
             std::abs(x - base) < 1.0e-4f &&
             std::abs(y - (base + 0.125f)) < 1.0e-4f &&
             std::abs(z - (base + 0.25f)) < 1.0e-4f &&
             std::abs(w - (base + 0.5f)) < 1.0e-4f;
    }
    acs_editor_destroy(host);
    return ok;
}

/** Opaque foreground geometry consumes a pointer before water behind it. */
bool RunWater3DPointerOcclusion() noexcept
{
    constexpr const char* kType = "AWaterSurface3DComponent";
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    acs_editor_scene3d_new(host);
    const bool camera_ready =
        acs_editor_camera3d_set(
            host, 0.0f, 0.55f, 10.0f,
            0.0f, 0.0f, 0.0f) != 0;
    const int water =
        acs_editor_add_node3d(host, 2, "OccludedWater");
    bool ok =
        camera_ready && water >= 0 &&
        acs_editor_node3d_set_transform(
            host, water,
            0.0f, 0.0f, 0.0f,
            0.0f, 0.0f, 0.0f,
            10.0f, 1.0f, 10.0f) != 0 &&
        acs_editor_node3d_add_component(
            host, water, kType) != 0;

    int hit_node = -1;
    float hit_x = 0.0f, hit_y = 0.0f, hit_z = 0.0f;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, &hit_x, &hit_y, &hit_z) != 0 &&
         hit_node == water &&
         std::abs(hit_x) < 1.0e-3f &&
         std::abs(hit_y) < 1.0e-3f &&
         std::abs(hit_z) < 1.0e-3f;

    // A local-visible water component is still inactive when any ancestor is
    // hidden or disabled. Re-enabling the parent must restore the exact hit
    // without mutating the child's authored flags.
    const int parent =
        acs_editor_add_empty3d(host, "WaterGroup");
    ok = ok && parent >= 0 &&
         acs_editor_reparent3d(host, water, parent) != 0;
    acs_editor_node3d_set_visible(host, parent, 0);
    hit_node = -1;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, nullptr, nullptr, nullptr) == 0 &&
         hit_node == -1;
    acs_editor_node3d_set_visible(host, parent, 1);
    hit_node = -1;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, nullptr, nullptr, nullptr) != 0 &&
         hit_node == water;
    acs_editor_node3d_set_enabled(host, parent, 0);
    hit_node = -1;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, nullptr, nullptr, nullptr) == 0 &&
         hit_node == -1;
    acs_editor_node3d_set_enabled(host, parent, 1);
    hit_node = -1;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, nullptr, nullptr, nullptr) != 0 &&
         hit_node == water;

    const int foreground =
        acs_editor_add_node3d(host, 0, "OpaqueForeground");
    ok = ok && foreground >= 0 &&
         acs_editor_node3d_set_transform(
             host, foreground,
             0.0f, 2.61f, 4.26f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0;
    hit_node = -1;
    ok = ok &&
         acs_editor_water3d_hit_test(
             host, 320.0f, 240.0f, 640.0f, 480.0f,
             &hit_node, nullptr, nullptr, nullptr) == 0 &&
         hit_node == -1;
    acs_editor_destroy(host);
    return ok;
}

/** Direct 3D camera control is finite, clamped, atomic, and available without a GPU attachment. */
bool RunCamera3DStateSafety() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
    float target_x = 0.0f, target_y = 0.0f, target_z = 0.0f;
    const bool minimum_clamped =
        acs_editor_camera3d_set(host, 1.25f, 99.0f, 0.01f, 4.0f, 5.0f, 6.0f) != 0 &&
        acs_editor_camera3d_get(
            host, &yaw, &pitch, &distance, &target_x, &target_y, &target_z) != 0 &&
        std::abs(yaw - 1.25f) < 1.0e-6f &&
        std::abs(pitch - 1.5533f) < 1.0e-5f &&
        std::abs(distance - 1.0f) < 1.0e-6f &&
        std::abs(target_x - 4.0f) < 1.0e-6f &&
        std::abs(target_y - 5.0f) < 1.0e-6f &&
        std::abs(target_z - 6.0f) < 1.0e-6f;

    const bool maximum_clamped =
        acs_editor_camera3d_set(host, -2.0f, -99.0f, 999.0f, -4.0f, -5.0f, -6.0f) != 0 &&
        acs_editor_camera3d_get(
            host, &yaw, &pitch, &distance, &target_x, &target_y, &target_z) != 0 &&
        std::abs(yaw + 2.0f) < 1.0e-6f &&
        std::abs(pitch + 1.5533f) < 1.0e-5f &&
        std::abs(distance - 200.0f) < 1.0e-6f &&
        std::abs(target_x + 4.0f) < 1.0e-6f &&
        std::abs(target_y + 5.0f) < 1.0e-6f &&
        std::abs(target_z + 6.0f) < 1.0e-6f;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const bool rejected =
        acs_editor_camera3d_set(host, nan, 0.0f, 10.0f, 0.0f, 0.0f, 0.0f) == 0 &&
        acs_editor_camera3d_get(
            host, &yaw, &pitch, &distance, &target_x, &target_y, &target_z) != 0 &&
        std::abs(yaw + 2.0f) < 1.0e-6f &&
        std::abs(pitch + 1.5533f) < 1.0e-5f &&
        std::abs(distance - 200.0f) < 1.0e-6f &&
        std::abs(target_x + 4.0f) < 1.0e-6f &&
        std::abs(target_y + 5.0f) < 1.0e-6f &&
        std::abs(target_z + 6.0f) < 1.0e-6f;

    acs_editor_destroy(host);
    return minimum_clamped && maximum_clamped && rejected;
}

/** Frame-all in 3D uses 3D renderer bounds rather than the unrelated 2D node registry. */
bool RunCamera3DFrameAll() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    acs_editor_set_view3d(host, 1);
    acs_editor_scene3d_new(host);
    const int left = acs_editor_add_node3d(host, 0, "FrameLeft");
    const int right = acs_editor_add_node3d(host, 0, "FrameRight");
    const bool transformed =
        left >= 0 && right >= 0 &&
        acs_editor_node3d_set_transform(
            host, left, -5.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f) != 0 &&
        acs_editor_node3d_set_transform(
            host, right, 7.0f, 2.0f, 3.0f, 0.0f, 0.0f, 0.0f, 2.0f, 2.0f, 2.0f) != 0 &&
        acs_editor_camera3d_set(host, 0.4f, -1.0f, 150.0f, 90.0f, 80.0f, 70.0f) != 0;

    acs_editor_camera_frame_all(host);
    float yaw = 0.0f, pitch = 0.0f, distance = 0.0f;
    float target_x = 0.0f, target_y = 0.0f, target_z = 0.0f;
    const bool framed =
        acs_editor_camera3d_get(
            host, &yaw, &pitch, &distance, &target_x, &target_y, &target_z) != 0 &&
        std::abs(yaw - 0.4f) < 1.0e-6f &&
        std::abs(pitch - 0.55f) < 1.0e-6f &&
        std::abs(target_x - 1.0f) < 1.0e-5f &&
        std::abs(target_y - 2.0f) < 1.0e-5f &&
        std::abs(target_z - 3.0f) < 1.0e-5f &&
        std::isfinite(distance) && distance > 1.0f && distance < 150.0f;
    acs_editor_destroy(host);
    return transformed && framed;
}

} // namespace

int main()
{
    acs_editor_destroy(nullptr); // null 破棄は常に no-op であることも通す。

    // OS とランタイムの初回遅延初期化を基準値から除外する。
    if (!RunOneLifecycle()) return 1;
    if (!RunStartupStatusContract()) return 13;
    if (!RunDestroyDuringAsyncWarmup()) return 14;
    if (!RunProfilerSnapshotContract()) return 12;
    if (!RunZeroScaleSafety()) return 8;
    if (!RunScene3DSerializationGrowth()) return 9;
    if (!RunSceneDocumentStrictPreflight()) return 17;
    if (!RunWater3DComponentRoundTrip()) return 15;
    if (!RunWater3DPointerOcclusion()) return 16;
    if (!RunCamera3DStateSafety()) return 10;
    if (!RunCamera3DFrameAll()) return 11;
    const DWORD baseline_handles = ProcessHandleCount();
    if (baseline_handles == 0) return 2;

    for (int cycle = 0; cycle < 8; ++cycle) {
        if (!RunOneLifecycle()) return 3;
    }

    // 複数ホストの一方を先に破棄しても、共有基盤と残りのホストは生存する。
    void* const first = acs_editor_create();
    void* const second = acs_editor_create();
    if (first == nullptr || second == nullptr) {
        acs_editor_destroy(first);
        acs_editor_destroy(second);
        return 4;
    }
    if (acs_editor_node_count(first) != 0 || acs_editor_node_count(second) != 0) {
        acs_editor_destroy(first);
        acs_editor_destroy(second);
        return 5;
    }

    acs_editor_destroy(first);
    if (acs_editor_node_count(second) != 0) {
        acs_editor_destroy(second);
        return 6;
    }
    acs_editor_destroy(second);

    const DWORD final_handles = ProcessHandleCount();
    if (final_handles > baseline_handles + 1u) {
        std::printf("Editor ABI handle leak: before=%lu after=%lu\n", static_cast<unsigned long>(baseline_handles),
                    static_cast<unsigned long>(final_handles));
        return 7;
    }
    return 0;
}
