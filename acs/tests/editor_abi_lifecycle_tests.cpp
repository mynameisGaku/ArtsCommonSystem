// SPDX-License-Identifier: Apache-2.0
// Editor ABI DLL の生成・破棄契約を、GPU 接続なしで実 DLL 境界から検証する。
#include "editor_abi/EditorProfiler.h"
#include "editor_abi/EditorCameraViewRequests.h"
#include "editor_abi/EditorFrustumCulling.h"
#include "editor_abi/EditorAbiCapabilities.h"
#include "editor_abi/EditorCloudWorkload.h"
#include "editor_abi/EditorServiceDiagnostics.h"

#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <thread>

extern "C" __declspec(dllimport) void* acs_editor_create(void);
extern "C" __declspec(dllimport) std::uint32_t
acs_editor_abi_contract_version(void);
extern "C" __declspec(dllimport) std::uint64_t
acs_editor_abi_capabilities(void);
extern "C" __declspec(dllimport) int acs_editor_abi_query(
    std::uint32_t requested_version,
    std::uint64_t required_capabilities,
    std::uint32_t* out_version,
    std::uint64_t* out_capabilities);
extern "C" __declspec(dllimport) int
acs_editor_optional_service_diagnostic_get(
    void* handle,
    std::uint32_t service,
    acs::editor_service_diagnostics::FDiagnostic* out_diagnostic,
    std::uint32_t out_size);
extern "C" __declspec(dllimport) const char* acs_editor_render_backend(void);
extern "C" __declspec(dllimport) void acs_editor_destroy(void* handle);
extern "C" __declspec(dllimport) int acs_editor_node_count(void* handle);
extern "C" __declspec(dllimport) int acs_editor_node3d_count(void* handle);
extern "C" __declspec(dllimport) int acs_editor_selected3d(void* handle);
extern "C" __declspec(dllimport) int acs_editor_add_node3d(
    void* handle, int primitive, const char* name);
extern "C" __declspec(dllimport) int acs_editor_add_empty3d(
    void* handle, const char* name);
extern "C" __declspec(dllimport) int acs_editor_add_node(
    void* handle, const char* type_name, int parent_id);
extern "C" __declspec(dllimport) void acs_editor_node_set_transform(
    void* handle, int id, float x, float y, float rotation,
    float scale_x, float scale_y);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_transform(
    void* handle, int id, float px, float py, float pz, float rx, float ry, float rz,
    float sx, float sy, float sz);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_transform_masked(
    void* handle, int id, std::uint32_t component_mask, const float* values9,
    std::uint32_t value_count);
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
extern "C" __declspec(dllimport) int acs_editor_paste_subtree3d(
    void* handle, const char* text, int parent_id);
extern "C" __declspec(dllimport) const char* acs_editor_copy_subtree3d(
    void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_refresh(
    void* handle, int id, const char* source, const char* text);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_refresh_with_root_overrides(void* handle, int id, const char* source, const char* text, std::uint32_t preserve_mask);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_revert_root_overrides(void* handle, int id, const char* source, const char* text, std::uint32_t revert_mask);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_revert_root_component_property_override(void* handle, int id, const char* source, const char* text, int slot, int property);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_clear_root_component_property_override(void* handle, int id, int slot, int property);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_instantiate(
    void* handle, const char* source, const char* instance_id,
    const char* text, int parent_id);
extern "C" __declspec(dllimport) int acs_editor_can_undo(void* handle);
extern "C" __declspec(dllimport) int acs_editor_undo(void* handle);
extern "C" __declspec(dllimport) int acs_editor_node3d_duplicate(
    void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_reparent3d(
    void* handle, int child_id, int parent_id);
extern "C" __declspec(dllimport) int acs_editor_node3d_parent(
    void* handle, int id);
extern "C" __declspec(dllimport) const char*
acs_editor_node3d_get_prefab_src(void* handle, int id);
extern "C" __declspec(dllimport) const char*
acs_editor_node3d_get_prefab_instance_id(void* handle, int id);
extern "C" __declspec(dllimport) const char* acs_editor_node3d_get_prefab_source_node_id(void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_find_node_by_source_id(void* handle, int root_id, const char* source_node_id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_root_for_node(void* handle, int id);
extern "C" __declspec(dllimport) std::uint32_t acs_editor_prefab_instance3d_property_override_mask(void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_mark_property_override(void* handle, int id, std::uint32_t mask);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_clear_property_overrides(void* handle, int id, std::uint32_t mask);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_clear_child_property_overrides(void* handle, int root_id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_revert_node_overrides(void* handle, int id, const char* source, const char* text, std::uint32_t revert_mask);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_prefab_link(
    void* handle, int id, const char* source, const char* instance_id);
extern "C" __declspec(dllimport) std::uint32_t acs_editor_prefab_instance3d_root_override_mask(void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_mark_root_override(void* handle, int id, std::uint32_t mask);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_clear_root_overrides(void* handle, int id, std::uint32_t mask);
extern "C" __declspec(dllimport) std::uint32_t acs_editor_prefab_instance3d_root_component_property_override_mask(void* handle, int id, int slot);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_mark_root_component_property_override(void* handle, int id, int slot, int property);
extern "C" __declspec(dllimport) int acs_editor_prefab_instance3d_clear_root_component_property_overrides(void* handle, int id);
extern "C" __declspec(dllimport) void acs_editor_node3d_set_visible(
    void* handle, int id, int visible);
extern "C" __declspec(dllimport) int acs_editor_node3d_get_visible(void* handle, int id);
extern "C" __declspec(dllimport) void acs_editor_node3d_set_enabled(
    void* handle, int id, int enabled);
extern "C" __declspec(dllimport) int acs_editor_node3d_get_enabled(void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_color(void* handle, int id, float r, float g, float b, float a);
extern "C" __declspec(dllimport) int acs_editor_node3d_get_color(void* handle, int id, float* out4);
extern "C" __declspec(dllimport) int acs_editor_node3d_set_material(void* handle, int id, const char* utf8_path);
extern "C" __declspec(dllimport) const char* acs_editor_node3d_get_material(void* handle, int id);
extern "C" __declspec(dllimport) int acs_editor_node3d_clear_material(void* handle, int id);
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
extern "C" __declspec(dllimport) int acs_editor_component_prop_default_at(
    const char* type_name, int index,
    float* x, float* y, float* z, float* w);
extern "C" __declspec(dllimport) const char*
acs_editor_component_prop_name_at(const char* type_name, int index);
extern "C" __declspec(dllimport) int acs_editor_water3d_hit_test(
    void* handle, float sx, float sy,
    float viewport_width, float viewport_height,
    int* node_id, float* world_x,
    float* world_y, float* world_z);
extern "C" __declspec(dllimport) void acs_editor_set_view3d(void* handle, int on);
extern "C" __declspec(dllimport) int acs_editor_get_view3d(void* handle);
extern "C" __declspec(dllimport) void acs_editor_set_ortho3d(
    void* handle, int on);
extern "C" __declspec(dllimport) int acs_editor_get_ortho3d(void* handle);
extern "C" __declspec(dllimport) void acs_editor_scene3d_new(void* handle);
extern "C" __declspec(dllimport) int acs_editor_play_start(void* handle);
extern "C" __declspec(dllimport) int acs_editor_play_stop(void* handle);
extern "C" __declspec(dllimport) int acs_editor_play_state(void* handle);
extern "C" __declspec(dllimport) void acs_editor_set_game_view(
    void* handle, int on);
extern "C" __declspec(dllimport) int acs_editor_is_game_view(void* handle);
extern "C" __declspec(dllimport) void acs_editor_camera_pan(
    void* handle, float dx, float dy);
extern "C" __declspec(dllimport) void acs_editor_camera_zoom(
    void* handle, float factor, float anchor_x, float anchor_y);
extern "C" __declspec(dllimport) void acs_editor_camera_get(
    void* handle, float* pan_x, float* pan_y, float* zoom);
extern "C" __declspec(dllimport) int acs_editor_camera3d_set(
    void* handle, float yaw, float pitch, float distance,
    float target_x, float target_y, float target_z);
extern "C" __declspec(dllimport) int acs_editor_camera3d_get(
    void* handle, float* yaw, float* pitch, float* distance,
    float* target_x, float* target_y, float* target_z);
extern "C" __declspec(dllimport) int acs_editor_game_camera2d_get(
    void* handle, unsigned viewport_width, unsigned viewport_height,
    float* center_x, float* center_y, float* zoom);
extern "C" __declspec(dllimport) int acs_editor_game_camera3d_get(
    void* handle, float aspect, int* projection,
    int* source_node_id, float* position3, float* forward3,
    float* up3, float* projection4);
extern "C" __declspec(dllimport) int acs_editor_add_camera3d(
    void* handle, const char* name, const char* stable_id);
extern "C" __declspec(dllimport) int acs_editor_node3d_camera_set(
    void* handle, int node_id, const char* stable_id,
    int projection, int priority, int active,
    float fov_deg, float ortho_height,
    float near_plane, float far_plane);
extern "C" __declspec(dllimport) int acs_editor_game_camera_preview_set(
    void* handle, int node_id);
extern "C" __declspec(dllimport) void acs_editor_game_camera_preview_clear(
    void* handle);
extern "C" __declspec(dllimport) int acs_editor_game_camera_preview_get(
    void* handle, int* node_id);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_create(
    void* handle, int node_id, const char* stable_camera_id,
    std::uint32_t width, std::uint32_t height,
    std::uint64_t* out_request_id);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_update(
    void* handle, std::uint64_t request_id,
    int node_id, const char* stable_camera_id,
    std::uint32_t width, std::uint32_t height);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_bind_presenter(
    void* handle, std::uint64_t request_id);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_unbind_presenter(
    void* handle, std::uint64_t request_id);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_destroy(
    void* handle, std::uint64_t request_id);
extern "C" __declspec(dllimport) int
acs_editor_camera_view_request_get(
    void* handle, std::uint64_t request_id,
    acs::editor_camera_view::FSnapshot* out_snapshot,
    std::uint32_t snapshot_size);
extern "C" __declspec(dllimport) int acs_editor_camera3d_count(
    void* handle);
extern "C" __declspec(dllimport) int acs_editor_camera3d_node_id_at(
    void* handle, int index);
extern "C" __declspec(dllimport) void
acs_editor_camera_frustum_set_visible(void* handle, int visible);
extern "C" __declspec(dllimport) int
acs_editor_camera_frustum_get_visible(void* handle);
extern "C" __declspec(dllimport) void acs_editor_camera_frame_all(void* handle);
extern "C" __declspec(dllimport) int acs_editor_profiler_get(
    void* handle, acs::editor_profiler::FSnapshot* out_snapshot,
    unsigned out_size);
extern "C" __declspec(dllimport) int acs_editor_cloud_workload_get(
    void* handle, void* out_snapshot,
    unsigned out_size);
extern "C" __declspec(dllimport) void acs_editor_profiler_reset_peaks(
    void* handle);
extern "C" __declspec(dllimport) int acs_editor_startup_status(
    void* handle, unsigned* completed, unsigned* total);
extern "C" __declspec(dllimport) void acs_editor_set_scene_presentation_suppressed(
    void* handle, int suppressed);
extern "C" __declspec(dllimport) int acs_editor_settings_set(
    void* handle, const char* category,
    const char* key, const char* value);
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

/** Host compatibility is negotiated explicitly and never inferred from a label. */
bool RunAbiCapabilityContract() noexcept
{
    using namespace acs::editor_abi;
    static_assert(kContractVersion == 1u);
    static_assert(sizeof(std::uint32_t) == 4u);
    static_assert(sizeof(std::uint64_t) == 8u);
    static_assert(
        (kCapabilities & kRequiredManagedHostCapabilities) ==
        kRequiredManagedHostCapabilities);
    static_assert(
        (kCapabilities &
         CapabilityBit(ECapability::VolumetricCloudWorkloadV1)) != 0u);
    static_assert((kCapabilities & CapabilityBit(ECapability::VolumetricCloudWorkloadV2)) != 0u);
    static_assert(
        (kCapabilities &
         CapabilityBit(ECapability::CameraViewRequestsV1)) != 0u);
    static_assert(
        (kRequiredManagedHostCapabilities &
         CapabilityBit(ECapability::VolumetricCloudWorkloadV1)) == 0u,
        "cloud workload diagnostics must remain optional");
    static_assert(
        (kRequiredManagedHostCapabilities &
         CapabilityBit(ECapability::CameraViewRequestsV1)) == 0u,
        "multi-view request scheduling must remain optional");
    static_assert(
        (kCapabilities &
         CapabilityBit(
             ECapability::OptionalServiceDiagnosticsV2)) != 0u);
    static_assert(
        (kRequiredManagedHostCapabilities &
         CapabilityBit(
             ECapability::OptionalServiceDiagnosticsV2)) == 0u,
        "service diagnostics must remain optional");
    static_assert(
        (kCapabilities &
         CapabilityBit(
             ECapability::SparseTransformMutationV1)) != 0u);
    static_assert(
        (kRequiredManagedHostCapabilities &
         CapabilityBit(
             ECapability::SparseTransformMutationV1)) != 0u,
        "managed Details relies on sparse transform mutation");
    static_assert(
        (kCapabilities &
         CapabilityBit(
             ECapability::PrefabInstanceRefresh3DV1)) != 0u);
    static_assert(
        (kRequiredManagedHostCapabilities &
         CapabilityBit(
             ECapability::PrefabInstanceRefresh3DV1)) != 0u,
        "managed Prefab Apply/Revert relies on transactional 3D refresh");
    static_assert(
        (kCapabilities & CapabilityBit(
             ECapability::PrefabStableInstanceId3DV1)) != 0u);
    static_assert(
        (kRequiredManagedHostCapabilities & CapabilityBit(
             ECapability::PrefabStableInstanceId3DV1)) != 0u,
        "managed Prefab authoring relies on stable 3D instance identity");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabRootPropertyOverride3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabRootPropertyOverride3DV1)) != 0u, "managed Prefab refresh relies on explicit 3D root property overrides");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabRootPropertySelectiveRevert3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabRootPropertySelectiveRevert3DV1)) != 0u, "managed Prefab authoring relies on selective 3D root override Revert");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabRootComponentPropertyOverride3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabRootComponentPropertyOverride3DV1)) != 0u, "managed Prefab refresh relies on 3D root component property overrides");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabNodePropertyOverride3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabNodePropertyOverride3DV1)) != 0u, "managed Prefab authoring relies on source-identified 3D child node overrides");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabNodeTransformOverride3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabNodeTransformOverride3DV1)) != 0u, "managed Prefab authoring relies on transactional 3D child transform overrides");
    static_assert((kCapabilities & CapabilityBit(ECapability::PrefabNodeMaterialOverride3DV1)) != 0u);
    static_assert((kRequiredManagedHostCapabilities & CapabilityBit(ECapability::PrefabNodeMaterialOverride3DV1)) != 0u, "managed Prefab authoring relies on transactional 3D child material overrides");

    std::uint32_t version = 0u;
    std::uint64_t capabilities = 0ull;
    const bool accepts_current =
        acs_editor_abi_query(
            kContractVersion,
            kRequiredManagedHostCapabilities,
            &version,
            &capabilities) == 1 &&
        version == kContractVersion &&
        capabilities == acs_editor_abi_capabilities() &&
        acs_editor_abi_contract_version() == kContractVersion;
    const bool rejects_invalid_version =
        acs_editor_abi_query(0u, 0ull, nullptr, nullptr) == 0;
    const bool rejects_future_version =
        acs_editor_abi_query(
            kContractVersion + 1u,
            0ull,
            nullptr,
            nullptr) == 0;
    const bool rejects_unknown_requirement =
        acs_editor_abi_query(
            kContractVersion,
            1ull << 63u,
            nullptr,
            nullptr) == 0;
    const char* const backend = acs_editor_render_backend();
    return accepts_current &&
           rejects_invalid_version &&
           rejects_future_version &&
           rejects_unknown_requirement &&
           backend != nullptr &&
           backend[0] != '\0';
}

bool RunOptionalServiceDiagnosticContract() noexcept
{
    using namespace acs::editor_service_diagnostics;
    static_assert(sizeof(FDiagnostic) == kDiagnosticSize);
    static_assert(kLegacyDiagnosticSize == 192u);
    static_assert(kDiagnosticSize == 256u);

    FDiagnostic malformed{};
    malformed.version = 99u;
    malformed.struct_size = kDiagnosticSize;
    malformed.state = 0xA5A5A5A5u;
    const bool rejects_null_output =
        acs_editor_optional_service_diagnostic_get(
            nullptr,
            static_cast<acs::u32>(EService::Profiler),
            nullptr,
            kDiagnosticSize) == 0;
    const bool rejects_unknown_version =
        acs_editor_optional_service_diagnostic_get(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(1u)),
            static_cast<acs::u32>(EService::Profiler),
            &malformed,
            kDiagnosticSize) == 0 &&
        malformed.state == 0xA5A5A5A5u;
    malformed = {};
    malformed.version = kDiagnosticVersion;
    malformed.struct_size = kDiagnosticSize - 1u;
    const bool rejects_short_declared_size =
        acs_editor_optional_service_diagnostic_get(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(1u)),
            static_cast<acs::u32>(EService::Profiler),
            &malformed,
            kDiagnosticSize) == 0;
    malformed = {};
    malformed.state = 0xA5A5A5A5u;
    const bool rejects_output_smaller_than_declared =
        acs_editor_optional_service_diagnostic_get(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(1u)),
            static_cast<acs::u32>(EService::Profiler),
            &malformed,
            kLegacyDiagnosticSize) == 0 &&
        malformed.state == 0xA5A5A5A5u;
    const bool rejects_header_too_short =
        acs_editor_optional_service_diagnostic_get(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(1u)),
            static_cast<acs::u32>(EService::Profiler),
            &malformed,
            sizeof(acs::u32) * 2u - 1u) == 0 &&
        malformed.state == 0xA5A5A5A5u;

    FDiagnostic invalid_host{};
    const bool reports_invalid_host =
        acs_editor_optional_service_diagnostic_get(
            nullptr,
            static_cast<acs::u32>(EService::Profiler),
            &invalid_host,
            kDiagnosticSize) == 1 &&
        invalid_host.version == kDiagnosticVersion &&
        invalid_host.struct_size == kDiagnosticSize &&
        invalid_host.state == static_cast<acs::u32>(EState::Failed) &&
        invalid_host.reason ==
            static_cast<acs::u32>(EReason::InvalidHost) &&
        invalid_host.error_domain ==
            static_cast<acs::u32>(EErrorDomain::EditorHost) &&
        invalid_host.error_code ==
            static_cast<acs::i32>(EErrorCode::InvalidHost) &&
        invalid_host.host_generation == 0u &&
        invalid_host.diagnostic_generation != 0u &&
        std::memchr(
            invalid_host.message_utf8,
            '\0',
            kMessageBytes) != nullptr &&
        std::memchr(
            invalid_host.stable_code_utf8,
            '\0',
            kStableCodeBytes) != nullptr;
    FDiagnostic invalid_nonnull_host{};
    const bool never_dereferences_unregistered_host =
        acs_editor_optional_service_diagnostic_get(
            reinterpret_cast<void*>(
                static_cast<std::uintptr_t>(1u)),
            static_cast<acs::u32>(EService::Profiler),
            &invalid_nonnull_host,
            kDiagnosticSize) == 1 &&
        invalid_nonnull_host.state ==
            static_cast<acs::u32>(EState::Failed) &&
        invalid_nonnull_host.reason ==
            static_cast<acs::u32>(EReason::InvalidHost) &&
        invalid_nonnull_host.host_generation == 0u;

    void* const first = acs_editor_create();
    void* const second = acs_editor_create();
    if (first == nullptr || second == nullptr) {
        acs_editor_destroy(first);
        acs_editor_destroy(second);
        return false;
    }

    FDiagnostic profiler{};
    const bool profiler_enabled =
        acs_editor_optional_service_diagnostic_get(
            first,
            static_cast<acs::u32>(EService::Profiler),
            &profiler,
            kDiagnosticSize) == 1 &&
        profiler.service == static_cast<acs::u32>(EService::Profiler) &&
        profiler.state == static_cast<acs::u32>(EState::Enabled) &&
        profiler.reason == static_cast<acs::u32>(EReason::None) &&
        (profiler.flags & Callable) != 0u &&
        profiler.host_generation != 0u &&
        profiler.diagnostic_generation != 0u &&
        profiler.error_domain ==
            static_cast<acs::u32>(EErrorDomain::None) &&
        profiler.error_code ==
            static_cast<acs::i32>(EErrorCode::None);

    FDiagnostic cloud{};
    const bool cloud_pending_before_attach =
        acs_editor_optional_service_diagnostic_get(
            first,
            static_cast<acs::u32>(
                EService::VolumetricCloudWorkload),
            &cloud,
            kDiagnosticSize) == 1 &&
        cloud.state == static_cast<acs::u32>(EState::Pending) &&
        cloud.reason ==
            static_cast<acs::u32>(EReason::StartupPending) &&
        (cloud.flags & Retryable) != 0u &&
        (cloud.flags & Callable) == 0u &&
        cloud.host_generation == profiler.host_generation &&
        cloud.diagnostic_generation >
            profiler.diagnostic_generation &&
        cloud.error_domain ==
            static_cast<acs::u32>(EErrorDomain::Renderer) &&
        cloud.error_code ==
            static_cast<acs::i32>(EErrorCode::StartupPending);

    FDiagnostic legacy;
    std::memset(&legacy, 0xA5, sizeof(legacy));
    legacy.version = kLegacyDiagnosticVersion;
    legacy.struct_size = kLegacyDiagnosticSize;
    const bool legacy_prefix_only =
        acs_editor_optional_service_diagnostic_get(
            first,
            static_cast<acs::u32>(EService::Profiler),
            &legacy,
            kLegacyDiagnosticSize) == 1 &&
        legacy.version == kLegacyDiagnosticVersion &&
        legacy.struct_size == kLegacyDiagnosticSize &&
        legacy.state == static_cast<acs::u32>(EState::Enabled) &&
        legacy.host_generation == profiler.host_generation &&
        std::memchr(
            legacy.message_utf8,
            '\0',
            kMessageBytes) != nullptr &&
        legacy.error_domain == 0xA5A5A5A5u &&
        legacy.error_code == static_cast<acs::i32>(0xA5A5A5A5u);

    FDiagnostic unknown{};
    const bool reports_unknown_service =
        acs_editor_optional_service_diagnostic_get(
            first,
            0xFFFFFFFFu,
            &unknown,
            kDiagnosticSize) == 1 &&
        unknown.state == static_cast<acs::u32>(EState::Disabled) &&
        unknown.reason ==
            static_cast<acs::u32>(EReason::UnknownService) &&
        unknown.error_domain ==
            static_cast<acs::u32>(EErrorDomain::EditorAbi) &&
        unknown.error_code ==
            static_cast<acs::i32>(EErrorCode::UnknownService);

    FDiagnostic second_profiler{};
    const bool host_generations_are_unique =
        acs_editor_optional_service_diagnostic_get(
            second,
            static_cast<acs::u32>(EService::Profiler),
            &second_profiler,
            kDiagnosticSize) == 1 &&
        second_profiler.host_generation != 0u &&
        second_profiler.host_generation !=
            profiler.host_generation;

    void* const stale_first = first;
    acs_editor_destroy(first);
    FDiagnostic after_destroy{};
    const bool destroyed_host_is_rejected_without_dereference =
        acs_editor_optional_service_diagnostic_get(
            stale_first,
            static_cast<acs::u32>(EService::Profiler),
            &after_destroy,
            kDiagnosticSize) == 1 &&
        after_destroy.state ==
            static_cast<acs::u32>(EState::Failed) &&
        after_destroy.reason ==
            static_cast<acs::u32>(EReason::InvalidHost) &&
        after_destroy.host_generation == 0u &&
        after_destroy.diagnostic_generation >
            second_profiler.diagnostic_generation;
    // Registry removal also makes a repeated stale destroy a no-op.
    acs_editor_destroy(stale_first);

    bool query_destroy_race_is_safe = false;
    void* const raced = acs_editor_create();
    if (raced != nullptr) {
        std::atomic<bool> stop_reader{false};
        std::atomic<bool> reader_valid{true};
        std::atomic<bool> saw_live{false};
        std::atomic<bool> saw_invalid{false};
        std::thread reader([&]() {
            while (!stop_reader.load(std::memory_order_acquire)) {
                FDiagnostic sample{};
                if (acs_editor_optional_service_diagnostic_get(
                        raced,
                        static_cast<acs::u32>(EService::Profiler),
                        &sample,
                        kDiagnosticSize) != 1) {
                    reader_valid.store(
                        false, std::memory_order_release);
                    break;
                }
                if (sample.host_generation != 0u &&
                    sample.state ==
                        static_cast<acs::u32>(EState::Enabled)) {
                    saw_live.store(true, std::memory_order_release);
                } else if (
                    sample.host_generation == 0u &&
                    sample.reason ==
                        static_cast<acs::u32>(EReason::InvalidHost)) {
                    saw_invalid.store(true, std::memory_order_release);
                } else {
                    reader_valid.store(
                        false, std::memory_order_release);
                    break;
                }
            }
        });
        for (int spin = 0;
             spin < 10000 &&
             !saw_live.load(std::memory_order_acquire);
             ++spin) {
            std::this_thread::yield();
        }
        acs_editor_destroy(raced);
        for (int spin = 0;
             spin < 10000 &&
             !saw_invalid.load(std::memory_order_acquire);
             ++spin) {
            std::this_thread::yield();
        }
        stop_reader.store(true, std::memory_order_release);
        reader.join();
        query_destroy_race_is_safe =
            reader_valid.load(std::memory_order_acquire) &&
            saw_live.load(std::memory_order_acquire) &&
            saw_invalid.load(std::memory_order_acquire);
    }
    acs_editor_destroy(second);
    return rejects_null_output &&
           rejects_unknown_version &&
           rejects_short_declared_size &&
           rejects_output_smaller_than_declared &&
           rejects_header_too_short &&
           reports_invalid_host &&
           never_dereferences_unregistered_host &&
           profiler_enabled &&
           cloud_pending_before_attach &&
           legacy_prefix_only &&
           reports_unknown_service &&
           host_generations_are_unique &&
           destroyed_host_is_rejected_without_dereference &&
           query_destroy_race_is_safe;
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

/** 新しいホストの 3D グラフを検証し、空状態または最初の明示追加 id が不正なら false を返す。 */
bool RunEmptyScene3DStartupContract() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    bool ok = acs_editor_node3d_count(host) == 0 &&
              acs_editor_selected3d(host) == -1;
    acs_editor_set_view3d(host, 1);
    char empty_scene[32]{};
    const int written = acs_editor_scene3d_serialize(
        host, empty_scene, static_cast<int>(sizeof(empty_scene)));
    ok = ok && acs_editor_get_view3d(host) != 0 &&
         acs_editor_node3d_count(host) == 0 &&
         acs_editor_selected3d(host) == -1 &&
         written == static_cast<int>(std::strlen("ACS3D v2\n")) &&
         std::strcmp(empty_scene, "ACS3D v2\n") == 0;

    acs_editor_set_view3d(host, 0);
    acs_editor_set_view3d(host, 1);
    ok = ok && acs_editor_node3d_count(host) == 0 &&
         acs_editor_selected3d(host) == -1;

    const int explicit_node = acs_editor_add_node3d(
        host, 0, "ExplicitFixtureNode");
    ok = ok && explicit_node == 1 &&
         acs_editor_node3d_count(host) == 1 &&
         acs_editor_selected3d(host) == explicit_node;
    acs_editor_destroy(host);
    return ok;
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
    static_assert(kSnapshotVersion == 5u);
    static_assert(kSnapshotSize == 256u);
    static_assert(kLegacySnapshotVersion == 4u);
    static_assert(kLegacySnapshotSize == 224u);
    static_assert(
        SceneMeshCacheRebuilt == (1u << 5u),
        "mesh-cache rebuild profiling must not change the snapshot ABI");
    static_assert(
        ScenePresentationSuppressed == (1u << 9u),
        "presentation suppression must reuse the profiler v4 flags field");

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
        snapshot.frustum_tested == 0u &&
        snapshot.frustum_visible == 0u &&
        snapshot.frustum_culled == 0u &&
        snapshot.active_camera_node_id == -1 &&
        snapshot.native_render_active_cpu_ms == 0.0f &&
        snapshot.native_present_cpu_ms == 0.0f &&
        snapshot.native_render_active_cpu_peak_ms == 0.0f &&
        snapshot.native_present_cpu_peak_ms == 0.0f &&
        snapshot.presented_frame_count_since_reset == 0u &&
        snapshot.profiler_reset_serial == 0u &&
        (snapshot.flags &
         (GpuTimingsValid | SceneMeshCacheRebuilt |
          FrustumCullingEnabled | RuntimeSceneCamera |
          ScenePresentationSuppressed)) == 0u;

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

    SYSTEM_INFO system_info{};
    ::GetSystemInfo(&system_info);
    const SIZE_T page_size =
        static_cast<SIZE_T>(system_info.dwPageSize);
    void* const guard_region = page_size > sizeof(acs::u32)
        ? ::VirtualAlloc(
              nullptr,
              page_size * 2u,
              MEM_RESERVE | MEM_COMMIT,
              PAGE_READWRITE)
        : nullptr;
    bool rejects_four_byte_guard = false;
    if (guard_region != nullptr) {
        DWORD previous_protection = 0;
        auto* const inaccessible_page =
            static_cast<unsigned char*>(guard_region) + page_size;
        if (::VirtualProtect(
                inaccessible_page,
                page_size,
                PAGE_NOACCESS,
                &previous_protection)) {
            auto* const version_only =
                inaccessible_page - sizeof(acs::u32);
            const acs::u32 requested_version = kSnapshotVersion;
            std::memcpy(
                version_only,
                &requested_version,
                sizeof(requested_version));
            rejects_four_byte_guard =
                acs_editor_profiler_get(
                    host,
                    reinterpret_cast<FSnapshot*>(version_only),
                    sizeof(requested_version)) == 0;
        }
        ::VirtualFree(guard_region, 0u, MEM_RELEASE);
    }

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

    struct FLegacySnapshot {
        acs::u32 version = kLegacySnapshotVersion;
        acs::u32 struct_size = kLegacySnapshotSize;
        unsigned char remaining[kLegacySnapshotSize - 8u]{};
    } legacy;
    static_assert(sizeof(FLegacySnapshot) == kLegacySnapshotSize);
    const bool legacy_prefix =
        acs_editor_profiler_get(
            host,
            reinterpret_cast<FSnapshot*>(&legacy),
            static_cast<unsigned>(sizeof(legacy))) != 0 &&
        legacy.version == kLegacySnapshotVersion &&
        legacy.struct_size == kLegacySnapshotSize;

    acs_editor_profiler_reset_peaks(host);
    FSnapshot reset_snapshot{};
    const bool capture_boundary_reset =
        acs_editor_profiler_get(
            host,
            &reset_snapshot,
            static_cast<unsigned>(sizeof(reset_snapshot))) != 0 &&
        reset_snapshot.profiler_reset_serial == 1u &&
        reset_snapshot.presented_frame_count_since_reset == 0u &&
        reset_snapshot.fps == 0.0f &&
        reset_snapshot.cpu_frame_ms == 0.0f &&
        reset_snapshot.cpu_submit_ms == 0.0f &&
        reset_snapshot.gpu_frame_ms < 0.0f &&
        reset_snapshot.opaque_cpu_ms == 0.0f &&
        reset_snapshot.atmosphere_cpu_ms == 0.0f &&
        reset_snapshot.cloud_cpu_ms == 0.0f &&
        reset_snapshot.fog_cpu_ms == 0.0f &&
        reset_snapshot.post_cpu_ms == 0.0f &&
        reset_snapshot.opaque_gpu_ms < 0.0f &&
        reset_snapshot.atmosphere_gpu_ms < 0.0f &&
        reset_snapshot.cloud_gpu_ms < 0.0f &&
        reset_snapshot.fog_gpu_ms < 0.0f &&
        reset_snapshot.post_gpu_ms < 0.0f &&
        reset_snapshot.gpu_frame_index == 0u &&
        (reset_snapshot.flags & GpuTimingsValid) == 0u &&
        reset_snapshot.native_render_active_cpu_peak_ms == 0.0f &&
        reset_snapshot.native_present_cpu_peak_ms == 0.0f;
    acs::editor_cloud_workload::FSnapshot reset_cloud{};
    const bool capture_boundary_cloud_reset =
        acs_editor_cloud_workload_get(
            host,
            &reset_cloud,
            static_cast<unsigned>(sizeof(reset_cloud))) == 0 &&
        reset_cloud.profiler_frame_index == 0u &&
        reset_cloud.flags == 0u &&
        reset_cloud.total_compute_dispatches == 0u;

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
           rejects_buffer_size && rejects_four_byte_guard &&
           forward_prefix && legacy_prefix &&
           capture_boundary_reset && capture_boundary_cloud_reset &&
           rejects_null &&
           rolling_peak && peak_reset && query_validity_and_identity &&
           query_window && query_reset;
}

/**
 * The optional cloud-workload contract is unavailable before renderer startup,
 * rejects incompatible callers, and remains independent from profiler v5.
 */
bool RunCloudWorkloadSnapshotContract() noexcept
{
    using namespace acs::editor_cloud_workload;
    static_assert(kSnapshotVersionV1 == 1u);
    static_assert(kSnapshotSizeV1 == 168u);
    static_assert(kSnapshotVersionV2 == 2u);
    static_assert(kSnapshotSizeV2 == 200u);
    static_assert(sizeof(FSnapshot) == 168u);
    static_assert(sizeof(FSnapshotV2) == 200u);
    static_assert(acs::editor_profiler::kSnapshotVersion == 5u);
    static_assert(acs::editor_profiler::kSnapshotSize == 256u);

    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    FSnapshot snapshotV1{};
    const bool v1_unavailable_before_attach =
        acs_editor_cloud_workload_get(host, &snapshotV1, static_cast<unsigned>(sizeof(snapshotV1))) == 0 &&
        snapshotV1.version == kSnapshotVersionV1 &&
        snapshotV1.struct_size == kSnapshotSizeV1 &&
        snapshotV1.flags == 0u &&
        snapshotV1.skip_reason == static_cast<acs::u32>(ESkipReason::None) &&
        snapshotV1.profiler_frame_index == 0u &&
        snapshotV1.submission_index == 0u &&
        snapshotV1.total_compute_dispatches == 0u &&
        snapshotV1.total_logical_invocations == 0u &&
        snapshotV1.total_launched_threads == 0u;

    FSnapshotV2 snapshotV2{};
    const bool v2_unavailable_before_attach =
        acs_editor_cloud_workload_get(host, &snapshotV2, static_cast<unsigned>(sizeof(snapshotV2))) == 0 &&
        snapshotV2.base.version == kSnapshotVersionV2 &&
        snapshotV2.base.struct_size == kSnapshotSizeV2 &&
        snapshotV2.base.flags == 0u &&
        snapshotV2.world_shadow_dispatches == 0u &&
        snapshotV2.world_shadow_logical_invocations == 0u &&
        snapshotV2.world_shadow_launched_threads == 0u &&
        snapshotV2.maximum_world_shadow_samples == 0u;

    snapshotV2.base.version = kSnapshotVersionV2 + 1u;
    const bool rejects_version =
        acs_editor_cloud_workload_get(host, &snapshotV2, static_cast<unsigned>(sizeof(snapshotV2))) < 0;
    snapshotV2.base.version = kSnapshotVersionV2;
    snapshotV2.base.struct_size = kSnapshotSizeV2 - 1u;
    const bool rejects_struct_size =
        acs_editor_cloud_workload_get(host, &snapshotV2, static_cast<unsigned>(sizeof(snapshotV2))) < 0;
    snapshotV2.base.struct_size = kSnapshotSizeV2;
    const bool rejects_buffer_size =
        acs_editor_cloud_workload_get(host, &snapshotV2, kSnapshotSizeV2 - 1u) < 0;

    struct FExtendedSnapshot {
        FSnapshotV2 base{};
        unsigned extension_sentinel = 0xC10D5A5Au;
    } extended;
    extended.base.base.struct_size = sizeof(extended);
    const bool forward_prefix =
        acs_editor_cloud_workload_get(host, &extended, static_cast<unsigned>(sizeof(extended))) == 0 &&
        extended.base.base.version == kSnapshotVersionV2 &&
        extended.base.base.struct_size == kSnapshotSizeV2 &&
        extended.extension_sentinel == 0xC10D5A5Au;

    const bool rejects_null =
        acs_editor_cloud_workload_get(host, nullptr, kSnapshotSizeV2) < 0 &&
        acs_editor_cloud_workload_get(nullptr, &extended, static_cast<unsigned>(sizeof(extended))) < 0;

    acs_editor_destroy(host);
    return v1_unavailable_before_attach &&
           v2_unavailable_before_attach && rejects_version &&
           rejects_struct_size && rejects_buffer_size &&
           forward_prefix && rejects_null;
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

/**
 * Sparse Inspector writes preserve every unmasked transform component,
 * including legacy finite scales below the current invertibility threshold.
 */
bool RunMaskedTransformMutationContract() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    constexpr const char* kLegacyScene =
        "ACS3D v2\n"
        "N3D 17 -1 0 1 2 3 10 20 30 0 -0 0.00005 0.2 0.3 0.4 1 LegacyScale\n";
    float initial[9]{};
    bool ok =
        acs_editor_scene3d_load_text(host, kLegacyScene) != 0 &&
        acs_editor_node3d_get_transform(host, 17, initial) != 0 &&
        initial[6] == 0.0f &&
        initial[7] == 0.0f &&
        std::signbit(initial[7]) &&
        initial[8] == 5.0e-5f;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    float location_request[9]{
        9.0f, nan, nan,
        nan, nan, nan,
        nan, nan, nan,
    };
    float after_location[9]{};
    ok = ok &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 1u << 0u, location_request, 9u) != 0 &&
         acs_editor_node3d_get_transform(
             host, 17, after_location) != 0 &&
         after_location[0] == 9.0f &&
         after_location[1] == initial[1] &&
         after_location[2] == initial[2] &&
         after_location[6] == 0.0f &&
         !std::signbit(after_location[6]) &&
         after_location[7] == 0.0f &&
         std::signbit(after_location[7]) &&
         after_location[8] == initial[8];

    float scale_request[9]{
        nan, nan, nan,
        nan, nan, nan,
        nan, nan, 0.0f,
    };
    float after_scale[9]{};
    ok = ok &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 1u << 8u, scale_request, 9u) != 0 &&
         acs_editor_node3d_get_transform(host, 17, after_scale) != 0 &&
         after_scale[0] == after_location[0] &&
         after_scale[6] == 0.0f &&
         !std::signbit(after_scale[6]) &&
         after_scale[7] == 0.0f &&
         std::signbit(after_scale[7]) &&
         after_scale[8] == 1.0e-4f;

    float invalid_selected[9]{};
    invalid_selected[0] = nan;
    ok = ok &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 0u, scale_request, 9u) == 0 &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 1u << 9u, scale_request, 9u) == 0 &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 1u << 0u, invalid_selected, 9u) == 0 &&
         acs_editor_node3d_set_transform_masked(
             host, 17, 1u << 0u, location_request, 8u) == 0;

    float after_rejections[9]{};
    ok = ok &&
         acs_editor_node3d_get_transform(
             host, 17, after_rejections) != 0 &&
         std::memcmp(
             after_scale,
             after_rejections,
             sizeof(after_scale)) == 0;

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

/** 3D subtree貼り付けは不正入力でsceneと履歴を変えず、成功時は1回のUndoで戻る。 */
bool RunTransactionalScene3DSubtreePaste() noexcept
{
    constexpr const char* kStableScene =
        "ACS3D v2\n"
        "N3D 41 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 StableRoot\n"
        "SEL3D 41\n";
    constexpr const char* kInvalidSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 1 2\n";
    constexpr const char* kValidSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 4 5 6 0 0 0 1 1 1 0.6 0.5 0.4 1 PastedRoot\n"
        "N3D 2 1 0 7 8 9 0 0 0 1 1 1 0.3 0.4 0.5 1 PastedChild\n";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    bool ok = acs_editor_scene3d_load_text(host, kStableScene) != 0;
    std::string expected2d;
    std::string expected3d;
    ok = ok && SnapshotSceneDocument(host, expected2d, expected3d) &&
         acs_editor_can_undo(host) == 0;

    ok = ok &&
         acs_editor_paste_subtree3d(host, kInvalidSubtree, 41) == -1 &&
         acs_editor_can_undo(host) == 0 &&
         acs_editor_node3d_count(host) == 1 &&
         SceneDocumentEquals(host, expected2d, expected3d);

    const int root = ok
        ? acs_editor_paste_subtree3d(host, kValidSubtree, 41)
        : -1;
    ok = ok && root >= 0 &&
         acs_editor_node3d_count(host) == 3 &&
         acs_editor_node3d_parent(host, root) == 41 &&
         acs_editor_selected3d(host) == root &&
         acs_editor_can_undo(host) != 0 &&
         acs_editor_undo(host) != 0 &&
         acs_editor_can_undo(host) == 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);
    if (!ok) std::printf("Transactional 3D subtree paste contract failed.\n");
    acs_editor_destroy(host);
    return ok;
}

/** 3D Prefab再生成は親とtransformを維持し、失敗時はsceneと履歴を完全に戻す。 */
bool RunPrefabInstance3DRefreshTransaction() noexcept
{
    constexpr const char* kStableScene =
        "ACS3D v2\n"
        "N3D 40 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Container\n"
        "EMPTY3D 40\n"
        "N3D 41 40 0 3 4 5 10 20 30 2 3 4 0.2 0.3 0.4 1 OldInstance\n"
        "N3D 42 41 0 0 1 0 0 0 0 1 1 1 0.4 0.5 0.6 1 OldCamera\n"
        "CAM3D 42 prefab.camera 0 10 1 60 10 0.05 1000\n"
        "PFAB3D 41 Assets/Old.acsprefab\n"
        "PINS3D 41 0123456789abcdef0123456789abcdef\n"
        "SEL3D 41\n";
    constexpr const char* kInvalidSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 invalid\n";
    constexpr const char* kUpdatedSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.7 0.6 0.5 1 UpdatedInstance\n"
        "N3D 2 1 0 0 2 0 0 0 0 1 1 1 0.5 0.6 0.7 1 UpdatedCamera\n"
        "CAM3D 2 prefab.camera 0 20 1 70 12 0.1 2000\n";
    constexpr const char* kUpdatedSource = "Assets/Updated.acsprefab";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    bool ok = acs_editor_scene3d_load_text(host, kStableScene) != 0;
    std::string expected2d;
    std::string expected3d;
    ok = ok && SnapshotSceneDocument(host, expected2d, expected3d) &&
         acs_editor_can_undo(host) == 0;

    std::string oversized_source(256u, 'a');
    ok = ok &&
         acs_editor_prefab_instance3d_refresh(host, 41, kUpdatedSource, kInvalidSubtree) == -1 &&
         acs_editor_prefab_instance3d_refresh(host, 41, oversized_source.c_str(), kUpdatedSubtree) == -1 &&
         acs_editor_can_undo(host) == 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);

    const int replacement = ok
        ? acs_editor_prefab_instance3d_refresh(host, 41, kUpdatedSource, kUpdatedSubtree)
        : -1;
    float transform[9]{};
    char refreshed_scene[32768]{};
    const int written = replacement >= 0
        ? acs_editor_scene3d_serialize(host, refreshed_scene, static_cast<int>(sizeof(refreshed_scene)))
        : 0;
    ok = ok && replacement >= 0 && replacement != 41 &&
         acs_editor_node3d_count(host) == 3 &&
         acs_editor_node3d_parent(host, replacement) == 40 &&
         acs_editor_selected3d(host) == replacement &&
         acs_editor_node3d_get_transform(host, replacement, transform) != 0 &&
         transform[0] == 3.0f && transform[1] == 4.0f && transform[2] == 5.0f &&
         transform[3] == 10.0f && transform[4] == 20.0f && transform[5] == 30.0f &&
         transform[6] == 2.0f && transform[7] == 3.0f && transform[8] == 4.0f &&
         std::strcmp(acs_editor_node3d_get_prefab_src(host, replacement), kUpdatedSource) == 0 &&
         std::strcmp(acs_editor_node3d_get_prefab_instance_id(host, replacement), "0123456789abcdef0123456789abcdef") == 0 &&
         written > 0 && written < static_cast<int>(sizeof(refreshed_scene)) &&
         std::strstr(refreshed_scene, "prefab.camera") != nullptr &&
         std::strstr(refreshed_scene, "-copy-") == nullptr &&
         acs_editor_can_undo(host) != 0 &&
         acs_editor_undo(host) != 0 &&
         acs_editor_can_undo(host) == 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);
    if (!ok) std::printf("3D Prefab refresh transaction contract failed.\n");
    acs_editor_destroy(host);
    return ok;
}

bool IsLowerHexPrefabInstanceId(const char* value) noexcept
{
    if (value == nullptr || std::strlen(value) != 32u) return false;
    for (std::uint32_t index = 0u; index < 32u; ++index) {
        if (!((value[index] >= '0' && value[index] <= '9') || (value[index] >= 'a' && value[index] <= 'f'))) return false;
    }
    return true;
}

/** 3D Prefab生成は明示IDをtransactionで設定し、複製だけ新しいIDへ移る。 */
bool RunPrefabInstance3DStableIdentity() noexcept
{
    constexpr const char* kStableScene =
        "ACS3D v2\n"
        "N3D 40 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Container\n"
        "EMPTY3D 40\n"
        "SEL3D 40\n";
    constexpr const char* kPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 1 2 3 0 0 0 1 1 1 0.2 0.3 0.4 1 Vehicle\n";
    constexpr const char* kSource = "Assets/Vehicle.acsprefab";
    constexpr const char* kIdentity = "fedcba9876543210fedcba9876543210";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    bool ok = acs_editor_scene3d_load_text(host, kStableScene) != 0;
    std::string expected2d;
    std::string expected3d;
    ok = ok && SnapshotSceneDocument(host, expected2d, expected3d) &&
         acs_editor_prefab_instance3d_instantiate(host, kSource, "FEDCBA9876543210FEDCBA9876543210", kPrefab, 40) == -1 &&
         acs_editor_can_undo(host) == 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);

    int instance = ok ? acs_editor_prefab_instance3d_instantiate(host, kSource, kIdentity, kPrefab, 40) : -1;
    ok = ok && instance >= 0 && acs_editor_node3d_parent(host, instance) == 40 &&
         std::strcmp(acs_editor_node3d_get_prefab_src(host, instance), kSource) == 0 &&
         std::strcmp(acs_editor_node3d_get_prefab_instance_id(host, instance), kIdentity) == 0 &&
         acs_editor_can_undo(host) != 0 && acs_editor_undo(host) != 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);

    instance = ok ? acs_editor_prefab_instance3d_instantiate(host, kSource, kIdentity, kPrefab, 40) : -1;
    const int duplicate = instance >= 0 ? acs_editor_node3d_duplicate(host, instance) : -1;
    const char* duplicate_identity = duplicate >= 0 ? acs_editor_node3d_get_prefab_instance_id(host, duplicate) : "";
    const std::string duplicate_identity_copy = duplicate_identity;
    ok = ok && duplicate >= 0 && IsLowerHexPrefabInstanceId(duplicate_identity) &&
         std::strcmp(duplicate_identity, kIdentity) != 0 &&
         std::strcmp(acs_editor_node3d_get_prefab_src(host, duplicate), kSource) == 0 &&
         acs_editor_node3d_set_prefab_link(host, duplicate, kSource, kIdentity) == 0;

    const std::string copied = ok ? acs_editor_copy_subtree3d(host, instance) : "";
    const int pasted = !copied.empty() ? acs_editor_paste_subtree3d(host, copied.c_str(), 40) : -1;
    const char* pasted_identity = pasted >= 0 ? acs_editor_node3d_get_prefab_instance_id(host, pasted) : "";
    ok = ok && pasted >= 0 && IsLowerHexPrefabInstanceId(pasted_identity) &&
         std::strcmp(pasted_identity, kIdentity) != 0 &&
         std::strcmp(pasted_identity, duplicate_identity_copy.c_str()) != 0 &&
         std::strcmp(acs_editor_node3d_get_prefab_src(host, pasted), kSource) == 0;

    char scene[32768]{};
    const int written = ok ? acs_editor_scene3d_serialize(host, scene, static_cast<int>(sizeof(scene))) : 0;
    ok = ok && written > 0 && std::strstr(scene, "PINS3D ") != nullptr &&
         acs_editor_scene3d_load_text(host, scene) != 0 &&
         std::strcmp(acs_editor_node3d_get_prefab_instance_id(host, instance), kIdentity) == 0;
    if (!ok) std::printf("3D Prefab stable instance identity contract failed.\n");
    acs_editor_destroy(host);
    return ok;
}

/** PSID3Dは原本nodeの再採番を越えて対応childを解決し、複製と保存でも同じsource identityを保つ。 */
bool RunPrefabInstance3DSourceNodeIdentity() noexcept
{
    constexpr const char* kSource = "Assets/Vehicle.acsprefab";
    constexpr const char* kRootSourceId = "0123456789abcdef0123456789abcdef";
    constexpr const char* kChildSourceId = "fedcba9876543210fedcba9876543210";
    constexpr const char* kPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.2 0.3 0.4 1 Vehicle\n"
        "PSID3D 1 0123456789abcdef0123456789abcdef\n"
        "N3D 2 1 0 1 0 0 0 0 0 1 1 1 0.5 0.6 0.7 1 Wheel\n"
        "PSID3D 2 fedcba9876543210fedcba9876543210\n";
    constexpr const char* kRenumberedPrefab =
        "ACS3D v2\n"
        "N3D 200 100 0 2 0 0 0 0 0 1 1 1 0.7 0.6 0.5 1 UpdatedWheel\n"
        "PSID3D 200 fedcba9876543210fedcba9876543210\n"
        "N3D 100 -1 0 0 0 0 0 0 0 1 1 1 0.4 0.3 0.2 1 UpdatedVehicle\n"
        "PSID3D 100 0123456789abcdef0123456789abcdef\n";
    constexpr const char* kLegacyPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 LegacyRoot\n"
        "N3D 2 1 0 0 1 0 0 0 0 1 1 1 1 1 1 1 LegacyChild\n";
    constexpr const char* kMalformedPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Invalid\n"
        "PSID3D 1 0123456789ABCDEF0123456789ABCDEF\n";
    constexpr const char* kDuplicateSourceIdPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Root\n"
        "PSID3D 1 0123456789abcdef0123456789abcdef\n"
        "N3D 2 1 0 0 1 0 0 0 0 1 1 1 1 1 1 1 Child\n"
        "PSID3D 2 0123456789abcdef0123456789abcdef\n";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    int instance = acs_editor_prefab_instance3d_instantiate(host, kSource, "0123456789abcdef0123456789abcdef", kPrefab, -1);
    int child = instance >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, instance, kChildSourceId) : -1;
    bool ok = instance >= 0 && child >= 0 && acs_editor_node3d_parent(host, child) == instance && std::strcmp(acs_editor_node3d_get_prefab_source_node_id(host, instance), kRootSourceId) == 0 && std::strcmp(acs_editor_node3d_get_prefab_source_node_id(host, child), kChildSourceId) == 0 && acs_editor_prefab_instance3d_find_node_by_source_id(host, instance, "FEDCBA9876543210FEDCBA9876543210") == -1;

    const int duplicate = ok ? acs_editor_node3d_duplicate(host, instance) : -1;
    const int duplicate_child = duplicate >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, duplicate, kChildSourceId) : -1;
    ok = ok && duplicate >= 0 && duplicate_child >= 0 && std::strcmp(acs_editor_node3d_get_prefab_source_node_id(host, duplicate), kRootSourceId) == 0 && std::strcmp(acs_editor_node3d_get_prefab_source_node_id(host, duplicate_child), kChildSourceId) == 0;

    const int refreshed = ok ? acs_editor_prefab_instance3d_refresh(host, instance, kSource, kRenumberedPrefab) : -1;
    const int refreshed_child = refreshed >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, refreshed, kChildSourceId) : -1;
    ok = ok && refreshed >= 0 && refreshed_child >= 0 && acs_editor_node3d_parent(host, refreshed_child) == refreshed && std::strcmp(acs_editor_node3d_get_prefab_source_node_id(host, refreshed), kRootSourceId) == 0;

    char serialized[32768]{};
    const int serialized_bytes = ok ? acs_editor_scene3d_serialize(host, serialized, static_cast<int>(sizeof(serialized))) : 0;
    ok = ok && serialized_bytes > 0 && std::strstr(serialized, "PSID3D ") != nullptr && acs_editor_scene3d_load_text(host, serialized) != 0 && acs_editor_prefab_instance3d_find_node_by_source_id(host, refreshed, kChildSourceId) >= 0;

    std::string before_invalid2d;
    std::string before_invalid3d;
    ok = ok && SnapshotSceneDocument(host, before_invalid2d, before_invalid3d) && acs_editor_prefab_instance3d_instantiate(host, kSource, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", kMalformedPrefab, -1) == -1 && acs_editor_prefab_instance3d_instantiate(host, kSource, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaab", kDuplicateSourceIdPrefab, -1) == -1 && SceneDocumentEquals(host, before_invalid2d, before_invalid3d);

    const int legacy = ok ? acs_editor_prefab_instance3d_instantiate(host, kSource, "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb", kLegacyPrefab, -1) : -1;
    const char* legacy_root_source_id = legacy >= 0 ? acs_editor_node3d_get_prefab_source_node_id(host, legacy) : "";
    const int legacy_child = legacy >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, legacy, "1c5a7ecfeb80b56902d7afc7d82b3250") : -1;
    ok = ok && legacy >= 0 && std::strcmp(legacy_root_source_id, "bc552ad841b671fae2c7b3e0dacc6803") == 0 && legacy_child >= 0;
    if (!ok) std::printf("3D Prefab source node identity contract failed.\n");
    acs_editor_destroy(host);
    return ok;
}

/** 3D Prefab root overrideは明示maskだけをsource更新後も保持し、全Revertで破棄する。 */
bool RunPrefabInstance3DRootPropertyOverrides() noexcept
{
    constexpr std::uint32_t kVisible = 1u << 0u;
    constexpr std::uint32_t kEnabled = 1u << 1u;
    constexpr std::uint32_t kColor = 1u << 2u;
    constexpr std::uint32_t kAll = kVisible | kEnabled | kColor;
    constexpr const char* kScene =
        "ACS3D v2\n"
        "N3D 40 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Container\n"
        "EMPTY3D 40\n"
        "N3D 41 40 0 0 0 0 0 0 0 1 1 1 0.2 0.3 0.4 1 Instance\n"
        "PFAB3D 41 Assets/Vehicle.acsprefab\n"
        "PINS3D 41 0123456789abcdef0123456789abcdef\n"
        "SEL3D 41\n";
    constexpr const char* kInvalidOverrideScene =
        "ACS3D v2\n"
        "N3D 41 -1 0 0 0 0 0 0 0 1 1 1 0.2 0.3 0.4 1 Instance\n"
        "PFAB3D 41 Assets/Vehicle.acsprefab\n"
        "PINS3D 41 0123456789abcdef0123456789abcdef\n"
        "POVR3D 41 8\n";
    constexpr const char* kUpdatedSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.1 0.2 0.3 1 Updated\n";
    constexpr const char* kMeshlessSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Meshless\n"
        "EMPTY3D 1\n";
    constexpr const char* kSource = "Assets/Vehicle.acsprefab";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    std::uint32_t completed_stage = 0u;
    bool ok = acs_editor_scene3d_load_text(host, kScene) != 0;
    std::string expected2d;
    std::string expected3d;
    ok = ok && SnapshotSceneDocument(host, expected2d, expected3d) &&
         acs_editor_scene3d_load_text(host, kInvalidOverrideScene) == 0 &&
         SceneDocumentEquals(host, expected2d, expected3d);
    if (ok) completed_stage = 1u;

    acs_editor_node3d_set_visible(host, 41, 0);
    acs_editor_node3d_set_enabled(host, 41, 0);
    ok = ok && acs_editor_node3d_set_color(host, 41, 0.9f, 0.8f, 0.7f, 1.0f) != 0 &&
         acs_editor_prefab_instance3d_mark_root_override(host, 41, kAll) != 0 &&
         acs_editor_prefab_instance3d_root_override_mask(host, 41) == kAll;
    if (ok) completed_stage = 2u;

    char overridden_scene[32768]{};
    const int overridden_written = ok
        ? acs_editor_scene3d_serialize(host, overridden_scene, static_cast<int>(sizeof(overridden_scene)))
        : 0;
    std::string before_failed_refresh2d;
    std::string before_failed_refresh3d;
    const bool emitted_override =
        overridden_written > 0 &&
        std::strstr(overridden_scene, "POVR3D 41 7\n") != nullptr;
    const bool captured_failed_refresh_baseline =
        SnapshotSceneDocument(host, before_failed_refresh2d, before_failed_refresh3d);
    const int unknown_mask_result = acs_editor_prefab_instance3d_refresh_with_root_overrides(host, 41, kSource, kUpdatedSubtree, 8u);
    const int meshless_result = acs_editor_prefab_instance3d_refresh_with_root_overrides(host, 41, kSource, kMeshlessSubtree, kAll);
    const bool failed_refresh_unchanged =
        SceneDocumentEquals(host, before_failed_refresh2d, before_failed_refresh3d);
    ok = ok && emitted_override && captured_failed_refresh_baseline &&
         unknown_mask_result == -1 && meshless_result == -1 &&
         failed_refresh_unchanged;
    if (ok) completed_stage = 3u;

    const int preserved = ok ? acs_editor_prefab_instance3d_refresh_with_root_overrides(host, 41, kSource, kUpdatedSubtree, kAll) : -1;
    float preserved_color[4]{};
    ok = ok && preserved >= 0 &&
         acs_editor_node3d_get_visible(host, preserved) == 0 &&
         acs_editor_node3d_get_enabled(host, preserved) == 0 &&
         acs_editor_node3d_get_color(host, preserved, preserved_color) != 0 &&
         preserved_color[0] == 0.9f && preserved_color[1] == 0.8f &&
         preserved_color[2] == 0.7f && preserved_color[3] == 1.0f &&
         acs_editor_prefab_instance3d_root_override_mask(host, preserved) == kAll &&
         acs_editor_prefab_instance3d_clear_root_overrides(host, preserved, kColor) != 0 &&
         acs_editor_prefab_instance3d_root_override_mask(host, preserved) == (kVisible | kEnabled) &&
         acs_editor_prefab_instance3d_mark_root_override(host, preserved, kColor) != 0;
    if (ok) completed_stage = 4u;

    std::string before_selective_invalid2d;
    std::string before_selective_invalid3d;
    ok = ok && SnapshotSceneDocument(host, before_selective_invalid2d, before_selective_invalid3d) && acs_editor_prefab_instance3d_revert_root_overrides(host, preserved, kSource, kUpdatedSubtree, 8u) == -1 && SceneDocumentEquals(host, before_selective_invalid2d, before_selective_invalid3d);
    const int selectively_reverted = ok ? acs_editor_prefab_instance3d_revert_root_overrides(host, preserved, kSource, kUpdatedSubtree, kVisible) : -1;
    float selective_color[4]{};
    char selective_scene[32768]{};
    const int selective_written = selectively_reverted >= 0 ? acs_editor_scene3d_serialize(host, selective_scene, static_cast<int>(sizeof(selective_scene))) : 0;
    char selective_override_line[64]{};
    const int selective_override_written = std::snprintf(selective_override_line, sizeof(selective_override_line), "POVR3D %d 6\n", selectively_reverted);
    ok = ok && selectively_reverted >= 0 && acs_editor_node3d_get_visible(host, selectively_reverted) != 0 && acs_editor_node3d_get_enabled(host, selectively_reverted) == 0 && acs_editor_node3d_get_color(host, selectively_reverted, selective_color) != 0 && selective_color[0] == 0.9f && selective_color[1] == 0.8f && selective_color[2] == 0.7f && selective_color[3] == 1.0f && acs_editor_prefab_instance3d_root_override_mask(host, selectively_reverted) == (kEnabled | kColor) && selective_written > 0 && selective_override_written > 0 && std::strstr(selective_scene, selective_override_line) != nullptr;
    std::string before_repeated_selective2d;
    std::string before_repeated_selective3d;
    ok = ok && SnapshotSceneDocument(host, before_repeated_selective2d, before_repeated_selective3d) && acs_editor_prefab_instance3d_revert_root_overrides(host, selectively_reverted, kSource, kUpdatedSubtree, kVisible) == -1 && SceneDocumentEquals(host, before_repeated_selective2d, before_repeated_selective3d);
    if (ok) completed_stage = 5u;

    const int reverted = ok ? acs_editor_prefab_instance3d_refresh(host, selectively_reverted, kSource, kUpdatedSubtree) : -1;
    float reverted_color[4]{};
    char reverted_scene[32768]{};
    const int reverted_written = reverted >= 0
        ? acs_editor_scene3d_serialize(host, reverted_scene, static_cast<int>(sizeof(reverted_scene)))
        : 0;
    ok = ok && reverted >= 0 &&
         acs_editor_node3d_get_visible(host, reverted) != 0 &&
         acs_editor_node3d_get_enabled(host, reverted) != 0 &&
         acs_editor_node3d_get_color(host, reverted, reverted_color) != 0 &&
         reverted_color[0] == 0.1f && reverted_color[1] == 0.2f &&
         reverted_color[2] == 0.3f && reverted_color[3] == 1.0f &&
         acs_editor_prefab_instance3d_root_override_mask(host, reverted) == 0u &&
         reverted_written > 0 && std::strstr(reverted_scene, "POVR3D ") == nullptr;
    if (!ok) {
        std::printf("3D Prefab root property override contract failed after stage %u.\n", completed_stage);
        std::printf("emitted=%d snapshot=%d unknown=%d meshless=%d unchanged=%d preserved=%d mask=%u color=%g,%g,%g,%g\n", emitted_override ? 1 : 0, captured_failed_refresh_baseline ? 1 : 0, unknown_mask_result, meshless_result, failed_refresh_unchanged ? 1 : 0, preserved, acs_editor_prefab_instance3d_root_override_mask(host, preserved), preserved_color[0], preserved_color[1], preserved_color[2], preserved_color[3]);
        std::printf("reverted=%d mask=%u color=%g,%g,%g,%g\n", reverted, acs_editor_prefab_instance3d_root_override_mask(host, reverted), reverted_color[0], reverted_color[1], reverted_color[2], reverted_color[3]);
    }
    acs_editor_destroy(host);
    return ok;
}

/** child node overrideはPSID3Dで再解決され、refreshとselective Revertを越えて保持される。 */
bool RunPrefabInstance3DNodePropertyOverrides() noexcept
{
    constexpr std::uint32_t kVisible = 1u << 0u;
    constexpr std::uint32_t kEnabled = 1u << 1u;
    constexpr std::uint32_t kColor = 1u << 2u;
    constexpr std::uint32_t kPosition = 1u << 3u;
    constexpr std::uint32_t kRotation = 1u << 4u;
    constexpr std::uint32_t kScale = 1u << 5u;
    constexpr std::uint32_t kMaterial = 1u << 6u;
    constexpr std::uint32_t kAll = kVisible | kEnabled | kColor | kPosition | kRotation | kScale | kMaterial;
    constexpr const char* kSource = "Assets/Vehicle.acsprefab";
    constexpr const char* kChildSourceId = "fedcba9876543210fedcba9876543210";
    constexpr const char* kPrefab =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.2 0.3 0.4 1 Vehicle\n"
        "PSID3D 1 0123456789abcdef0123456789abcdef\n"
        "N3D 2 1 0 1 0 0 0 0 0 1 1 1 0.5 0.6 0.7 1 Wheel\n"
        "PSID3D 2 fedcba9876543210fedcba9876543210\n"
        "MAT3D 2 Assets/Original.acsmat\n";
    constexpr const char* kUpdatedPrefab =
        "ACS3D v2\n"
        "N3D 100 -1 0 0 0 0 0 0 0 1 1 1 0.3 0.4 0.5 1 UpdatedVehicle\n"
        "PSID3D 100 0123456789abcdef0123456789abcdef\n"
        "N3D 200 100 0 2 3 4 5 6 7 0.5 0.6 0.7 0.1 0.2 0.3 1 UpdatedWheel\n"
        "PSID3D 200 fedcba9876543210fedcba9876543210\n"
        "MAT3D 200 Assets/Updated.acsmat\n";
    constexpr const char* kMissingChildPrefab =
        "ACS3D v2\n"
        "N3D 100 -1 0 0 0 0 0 0 0 1 1 1 0.3 0.4 0.5 1 UpdatedVehicle\n"
        "PSID3D 100 0123456789abcdef0123456789abcdef\n";
    constexpr const char* kMeshlessChildPrefab =
        "ACS3D v2\n"
        "N3D 100 -1 0 0 0 0 0 0 0 1 1 1 0.3 0.4 0.5 1 UpdatedVehicle\n"
        "PSID3D 100 0123456789abcdef0123456789abcdef\n"
        "N3D 200 100 -1 2 0 0 0 0 0 1 1 1 1 1 1 1 EmptyWheel\n"
        "PSID3D 200 fedcba9876543210fedcba9876543210\n"
        "EMPTY3D 200\n";
    constexpr const char* kInvalidNodeOverride =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 1 1 1 1 Orphan\n"
        "PSID3D 1 fedcba9876543210fedcba9876543210\n"
        "PNOVR3D 1 1\n";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    int instance = acs_editor_prefab_instance3d_instantiate(host, kSource, "0123456789abcdef0123456789abcdef", kPrefab, -1);
    int child = instance >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, instance, kChildSourceId) : -1;
    bool ok = instance >= 0 && child >= 0 && acs_editor_prefab_instance3d_root_for_node(host, child) == instance && acs_editor_prefab_instance3d_root_for_node(host, instance) == instance;

    acs_editor_node3d_set_visible(host, child, 0);
    acs_editor_node3d_set_enabled(host, child, 0);
    ok = ok && acs_editor_node3d_set_color(host, child, 0.9f, 0.8f, 0.7f, 0.6f) != 0 && acs_editor_node3d_set_transform(host, child, 9.0f, 8.0f, 7.0f, 10.0f, 20.0f, 30.0f, 2.0f, 3.0f, 4.0f) != 0 && acs_editor_node3d_set_material(host, child, "Assets/Override.acsmat") != 0 && acs_editor_prefab_instance3d_mark_property_override(host, child, kAll) != 0 && acs_editor_prefab_instance3d_property_override_mask(host, child) == kAll && acs_editor_prefab_instance3d_mark_property_override(host, instance, kMaterial) == 0 && acs_editor_prefab_instance3d_root_override_mask(host, instance) == 0u;

    char overridden_scene[32768]{};
    const int overridden_written = ok ? acs_editor_scene3d_serialize(host, overridden_scene, static_cast<int>(sizeof(overridden_scene))) : 0;
    char override_line[64]{};
    const int override_line_written = std::snprintf(override_line, sizeof(override_line), "PNOVR3D %d 127\n", child);
    char override_material_line[128]{};
    const int override_material_line_written = std::snprintf(override_material_line, sizeof(override_material_line), "MAT3D %d Assets/Override.acsmat\n", child);
    ok = ok && overridden_written > 0 && override_line_written > 0 && override_material_line_written > 0 && std::strstr(overridden_scene, override_line) != nullptr && std::strstr(overridden_scene, override_material_line) != nullptr;

    std::string before_invalid2d;
    std::string before_invalid3d;
    ok = ok && SnapshotSceneDocument(host, before_invalid2d, before_invalid3d) && acs_editor_scene3d_load_text(host, kInvalidNodeOverride) == 0 && SceneDocumentEquals(host, before_invalid2d, before_invalid3d);

    std::string before_missing2d;
    std::string before_missing3d;
    ok = ok && SnapshotSceneDocument(host, before_missing2d, before_missing3d) && acs_editor_prefab_instance3d_refresh_with_root_overrides(host, instance, kSource, kMissingChildPrefab, 0u) == -1 && SceneDocumentEquals(host, before_missing2d, before_missing3d);
    std::string before_meshless2d;
    std::string before_meshless3d;
    ok = ok && SnapshotSceneDocument(host, before_meshless2d, before_meshless3d) && acs_editor_prefab_instance3d_refresh_with_root_overrides(host, instance, kSource, kMeshlessChildPrefab, 0u) == -1 && SceneDocumentEquals(host, before_meshless2d, before_meshless3d);

    const int refreshed = ok ? acs_editor_prefab_instance3d_refresh_with_root_overrides(host, instance, kSource, kUpdatedPrefab, 0u) : -1;
    child = refreshed >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, refreshed, kChildSourceId) : -1;
    float preserved_color[4]{};
    float preserved_transform[9]{};
    ok = ok && refreshed >= 0 && child >= 0 && acs_editor_node3d_get_visible(host, child) == 0 && acs_editor_node3d_get_enabled(host, child) == 0 && acs_editor_node3d_get_color(host, child, preserved_color) != 0 && preserved_color[0] == 0.9f && preserved_color[1] == 0.8f && preserved_color[2] == 0.7f && preserved_color[3] == 0.6f && acs_editor_node3d_get_transform(host, child, preserved_transform) != 0 && preserved_transform[0] == 9.0f && preserved_transform[1] == 8.0f && preserved_transform[2] == 7.0f && preserved_transform[3] == 10.0f && preserved_transform[4] == 20.0f && preserved_transform[5] == 30.0f && preserved_transform[6] == 2.0f && preserved_transform[7] == 3.0f && preserved_transform[8] == 4.0f && std::strcmp(acs_editor_node3d_get_material(host, child), "Assets/Override.acsmat") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, child) == kAll;

    ok = ok && acs_editor_node3d_clear_material(host, child) != 0 && std::strcmp(acs_editor_node3d_get_material(host, child), "") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, child) == kAll;

    ok = ok && acs_editor_prefab_instance3d_clear_property_overrides(host, child, kColor) != 0 && acs_editor_prefab_instance3d_property_override_mask(host, child) == (kAll & ~kColor) && acs_editor_prefab_instance3d_mark_property_override(host, child, kColor) != 0 && acs_editor_prefab_instance3d_clear_child_property_overrides(host, refreshed) != 0 && acs_editor_prefab_instance3d_property_override_mask(host, child) == 0u && acs_editor_prefab_instance3d_mark_property_override(host, child, kAll) != 0;

    std::string before_bad_revert2d;
    std::string before_bad_revert3d;
    ok = ok && SnapshotSceneDocument(host, before_bad_revert2d, before_bad_revert3d) && acs_editor_prefab_instance3d_revert_node_overrides(host, child, kSource, kUpdatedPrefab, 128u) == -1 && SceneDocumentEquals(host, before_bad_revert2d, before_bad_revert3d);
    const int reverted_child = ok ? acs_editor_prefab_instance3d_revert_node_overrides(host, child, kSource, kUpdatedPrefab, kVisible) : -1;
    const int reverted_root = reverted_child >= 0 ? acs_editor_prefab_instance3d_root_for_node(host, reverted_child) : -1;
    float selective_color[4]{};
    float selective_transform[9]{};
    ok = ok && reverted_child >= 0 && reverted_root >= 0 && acs_editor_node3d_get_visible(host, reverted_child) != 0 && acs_editor_node3d_get_enabled(host, reverted_child) == 0 && acs_editor_node3d_get_color(host, reverted_child, selective_color) != 0 && selective_color[0] == 0.9f && selective_color[1] == 0.8f && selective_color[2] == 0.7f && selective_color[3] == 0.6f && acs_editor_node3d_get_transform(host, reverted_child, selective_transform) != 0 && selective_transform[0] == 9.0f && selective_transform[3] == 10.0f && selective_transform[6] == 2.0f && std::strcmp(acs_editor_node3d_get_material(host, reverted_child), "") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, reverted_child) == (kAll & ~kVisible);

    std::string before_repeated2d;
    std::string before_repeated3d;
    ok = ok && SnapshotSceneDocument(host, before_repeated2d, before_repeated3d) && acs_editor_prefab_instance3d_revert_node_overrides(host, reverted_child, kSource, kUpdatedPrefab, kVisible) == -1 && SceneDocumentEquals(host, before_repeated2d, before_repeated3d);

    const int position_reverted_child = ok ? acs_editor_prefab_instance3d_revert_node_overrides(host, reverted_child, kSource, kUpdatedPrefab, kPosition) : -1;
    const int position_reverted_root = position_reverted_child >= 0 ? acs_editor_prefab_instance3d_root_for_node(host, position_reverted_child) : -1;
    float position_reverted_transform[9]{};
    ok = ok && position_reverted_child >= 0 && position_reverted_root >= 0 && acs_editor_node3d_get_transform(host, position_reverted_child, position_reverted_transform) != 0 && position_reverted_transform[0] == 2.0f && position_reverted_transform[1] == 3.0f && position_reverted_transform[2] == 4.0f && position_reverted_transform[3] == 10.0f && position_reverted_transform[4] == 20.0f && position_reverted_transform[5] == 30.0f && position_reverted_transform[6] == 2.0f && position_reverted_transform[7] == 3.0f && position_reverted_transform[8] == 4.0f && std::strcmp(acs_editor_node3d_get_material(host, position_reverted_child), "") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, position_reverted_child) == (kAll & ~(kVisible | kPosition));

    const int material_reverted_child = ok ? acs_editor_prefab_instance3d_revert_node_overrides(host, position_reverted_child, kSource, kUpdatedPrefab, kMaterial) : -1;
    const int material_reverted_root = material_reverted_child >= 0 ? acs_editor_prefab_instance3d_root_for_node(host, material_reverted_child) : -1;
    ok = ok && material_reverted_child >= 0 && material_reverted_root >= 0 && std::strcmp(acs_editor_node3d_get_material(host, material_reverted_child), "Assets/Updated.acsmat") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, material_reverted_child) == (kAll & ~(kVisible | kPosition | kMaterial));

    const int fully_reverted = ok ? acs_editor_prefab_instance3d_refresh(host, material_reverted_root, kSource, kUpdatedPrefab) : -1;
    const int fully_reverted_child = fully_reverted >= 0 ? acs_editor_prefab_instance3d_find_node_by_source_id(host, fully_reverted, kChildSourceId) : -1;
    float source_color[4]{};
    float source_transform[9]{};
    char reverted_scene[32768]{};
    const int reverted_written = fully_reverted_child >= 0 ? acs_editor_scene3d_serialize(host, reverted_scene, static_cast<int>(sizeof(reverted_scene))) : 0;
    ok = ok && fully_reverted >= 0 && fully_reverted_child >= 0 && acs_editor_node3d_get_visible(host, fully_reverted_child) != 0 && acs_editor_node3d_get_enabled(host, fully_reverted_child) != 0 && acs_editor_node3d_get_color(host, fully_reverted_child, source_color) != 0 && source_color[0] == 0.1f && source_color[1] == 0.2f && source_color[2] == 0.3f && source_color[3] == 1.0f && acs_editor_node3d_get_transform(host, fully_reverted_child, source_transform) != 0 && source_transform[0] == 2.0f && source_transform[1] == 3.0f && source_transform[2] == 4.0f && source_transform[3] == 5.0f && source_transform[4] == 6.0f && source_transform[5] == 7.0f && source_transform[6] == 0.5f && source_transform[7] == 0.6f && source_transform[8] == 0.7f && std::strcmp(acs_editor_node3d_get_material(host, fully_reverted_child), "Assets/Updated.acsmat") == 0 && acs_editor_prefab_instance3d_property_override_mask(host, fully_reverted_child) == 0u && reverted_written > 0 && std::strstr(reverted_scene, "PNOVR3D ") == nullptr;
    if (!ok) std::printf("3D Prefab child node property override contract failed.\n");
    acs_editor_destroy(host);
    return ok;
}

/** root component property overrideは型IDでsource再配置を越えて保持される。 */
bool RunPrefabInstance3DRootComponentPropertyOverrides() noexcept
{
    constexpr std::uint32_t kRoughness = 1u << 4u;
    constexpr std::uint32_t kNormalStrength = 1u << 5u;
    constexpr std::uint32_t kMeshRoughness = 1u << 1u;
    constexpr std::uint32_t kEnabled = 1u << 1u;
    constexpr const char* kSource = "Assets/WaterVehicle.acsprefab";
    constexpr const char* kScene =
        "ACS3D v2\n"
        "N3D 40 -1 -1 0 0 0 0 0 0 1 1 1 1 1 1 1 Container\n"
        "EMPTY3D 40\n"
        "N3D 41 40 0 0 0 0 0 0 0 1 1 1 0.2 0.3 0.4 1 Instance\n"
        "CMP3D 41 AMeshComponent3D\n"
        "CPROP3D 41 0 1 0.2 0 0 0\n"
        "CMP3D 41 AWaterSurface3DComponent\n"
        "CPROP3D 41 1 4 0.25 0 0 0\n"
        "PFAB3D 41 Assets/WaterVehicle.acsprefab\n"
        "PINS3D 41 0123456789abcdef0123456789abcdef\n"
        "PCOVR3D 41 1 4\n"
        "SEL3D 41\n";
    constexpr const char* kMissingComponentSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.1 0.2 0.3 1 Updated\n"
        "CMP3D 1 AMeshComponent3D\n"
        "CPROP3D 1 0 1 0.4 0 0 0\n";
    constexpr const char* kReorderedSubtree =
        "ACS3D v2\n"
        "N3D 1 -1 0 0 0 0 0 0 0 1 1 1 0.1 0.2 0.3 1 Updated\n"
        "CMP3D 1 AWaterSurface3DComponent\n"
        "CPROP3D 1 0 4 0.1 0 0 0\n"
        "CPROP3D 1 0 5 0.2 0 0 0\n"
        "CMP3D 1 AMeshComponent3D\n"
        "CPROP3D 1 1 1 0.4 0 0 0\n";

    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    std::uint32_t completed_stage = 0u;
    bool ok = acs_editor_scene3d_load_text(host, kScene) != 0 && acs_editor_prefab_instance3d_root_component_property_override_mask(host, 41, 1) == kRoughness;
    if (ok) completed_stage = 1u;

    float ignored = 0.0f;
    ok = ok && acs_editor_node3d_component_prop_set(host, 41, 1, 4, 0.9f, 0.0f, 0.0f, 0.0f) != 0 && acs_editor_prefab_instance3d_mark_root_component_property_override(host, 41, 1, 4) != 0 && acs_editor_prefab_instance3d_mark_root_component_property_override(host, 41, 1, 24) == 0 && acs_editor_node3d_component_prop_get(host, 41, 1, 4, &ignored, nullptr, nullptr, nullptr) != 0 && ignored == 0.9f;
    if (ok) completed_stage = 2u;

    char overridden_scene[32768]{};
    const int overridden_written = ok ? acs_editor_scene3d_serialize(host, overridden_scene, static_cast<int>(sizeof(overridden_scene))) : 0;
    std::string before_failed_refresh2d;
    std::string before_failed_refresh3d;
    ok = ok && overridden_written > 0 && std::strstr(overridden_scene, "PCOVR3D 41 1 4\n") != nullptr && SnapshotSceneDocument(host, before_failed_refresh2d, before_failed_refresh3d) && acs_editor_prefab_instance3d_refresh_with_root_overrides(host, 41, kSource, kMissingComponentSubtree, 0u) == -1 && SceneDocumentEquals(host, before_failed_refresh2d, before_failed_refresh3d);
    if (ok) completed_stage = 3u;

    const int preserved = ok ? acs_editor_prefab_instance3d_refresh_with_root_overrides(host, 41, kSource, kReorderedSubtree, 0u) : -1;
    float preserved_roughness = 0.0f;
    char preserved_scene[32768]{};
    const int preserved_written = preserved >= 0 ? acs_editor_scene3d_serialize(host, preserved_scene, static_cast<int>(sizeof(preserved_scene))) : 0;
    char preserved_override_line[64]{};
    const int preserved_override_written = std::snprintf(preserved_override_line, sizeof(preserved_override_line), "PCOVR3D %d 0 4\n", preserved);
    ok = ok && preserved >= 0 && acs_editor_node3d_component_prop_get(host, preserved, 0, 4, &preserved_roughness, nullptr, nullptr, nullptr) != 0 && preserved_roughness == 0.9f && acs_editor_prefab_instance3d_root_component_property_override_mask(host, preserved, 0) == kRoughness && acs_editor_prefab_instance3d_root_component_property_override_mask(host, preserved, 1) == 0u && preserved_written > 0 && preserved_override_written > 0 && std::strstr(preserved_scene, preserved_override_line) != nullptr && acs_editor_scene3d_load_text(host, preserved_scene) != 0 && acs_editor_prefab_instance3d_root_component_property_override_mask(host, preserved, 0) == kRoughness;
    if (ok) completed_stage = 4u;

    ok = ok && acs_editor_node3d_component_prop_set(host, preserved, 0, 5, 0.8f, 0.0f, 0.0f, 0.0f) != 0 && acs_editor_prefab_instance3d_mark_root_component_property_override(host, preserved, 0, 5) != 0 && acs_editor_node3d_component_prop_set(host, preserved, 1, 1, 0.7f, 0.0f, 0.0f, 0.0f) != 0 && acs_editor_prefab_instance3d_mark_root_component_property_override(host, preserved, 1, 1) != 0;
    if (ok) {
        acs_editor_node3d_set_enabled(host, preserved, 0);
        ok = acs_editor_prefab_instance3d_mark_root_override(host, preserved, kEnabled) != 0;
    }
    std::string before_invalid_selective2d;
    std::string before_invalid_selective3d;
    ok = ok && SnapshotSceneDocument(host, before_invalid_selective2d, before_invalid_selective3d) && acs_editor_prefab_instance3d_revert_root_component_property_override(host, preserved, kSource, kReorderedSubtree, 0, 3) == -1 && SceneDocumentEquals(host, before_invalid_selective2d, before_invalid_selective3d);
    const int component_reverted = ok ? acs_editor_prefab_instance3d_revert_root_component_property_override(host, preserved, kSource, kReorderedSubtree, 0, 4) : -1;
    float selectively_reverted_roughness = 0.0f;
    float preserved_normal_strength = 0.0f;
    float preserved_mesh_roughness = 0.0f;
    char component_reverted_scene[32768]{};
    const int component_reverted_written = component_reverted >= 0 ? acs_editor_scene3d_serialize(host, component_reverted_scene, static_cast<int>(sizeof(component_reverted_scene))) : 0;
    char retained_water_override_line[64]{};
    char retained_mesh_override_line[64]{};
    char removed_water_override_line[64]{};
    const int retained_water_override_written = std::snprintf(retained_water_override_line, sizeof(retained_water_override_line), "PCOVR3D %d 0 5\n", component_reverted);
    const int retained_mesh_override_written = std::snprintf(retained_mesh_override_line, sizeof(retained_mesh_override_line), "PCOVR3D %d 1 1\n", component_reverted);
    const int removed_water_override_written = std::snprintf(removed_water_override_line, sizeof(removed_water_override_line), "PCOVR3D %d 0 4\n", component_reverted);
    ok = ok && component_reverted >= 0 && acs_editor_node3d_component_prop_get(host, component_reverted, 0, 4, &selectively_reverted_roughness, nullptr, nullptr, nullptr) != 0 && selectively_reverted_roughness == 0.1f && acs_editor_node3d_component_prop_get(host, component_reverted, 0, 5, &preserved_normal_strength, nullptr, nullptr, nullptr) != 0 && preserved_normal_strength == 0.8f && acs_editor_node3d_component_prop_get(host, component_reverted, 1, 1, &preserved_mesh_roughness, nullptr, nullptr, nullptr) != 0 && preserved_mesh_roughness == 0.7f && acs_editor_prefab_instance3d_root_component_property_override_mask(host, component_reverted, 0) == kNormalStrength && acs_editor_prefab_instance3d_root_component_property_override_mask(host, component_reverted, 1) == kMeshRoughness && acs_editor_prefab_instance3d_root_override_mask(host, component_reverted) == kEnabled && acs_editor_node3d_get_enabled(host, component_reverted) == 0 && component_reverted_written > 0 && retained_water_override_written > 0 && retained_mesh_override_written > 0 && removed_water_override_written > 0 && std::strstr(component_reverted_scene, retained_water_override_line) != nullptr && std::strstr(component_reverted_scene, retained_mesh_override_line) != nullptr && std::strstr(component_reverted_scene, removed_water_override_line) == nullptr;
    std::string before_repeated_selective2d;
    std::string before_repeated_selective3d;
    ok = ok && SnapshotSceneDocument(host, before_repeated_selective2d, before_repeated_selective3d) && acs_editor_prefab_instance3d_revert_root_component_property_override(host, component_reverted, kSource, kReorderedSubtree, 0, 4) == -1 && SceneDocumentEquals(host, before_repeated_selective2d, before_repeated_selective3d);
    if (ok) completed_stage = 5u;

    std::string before_invalid_clear2d;
    std::string before_invalid_clear3d;
    ok = ok && SnapshotSceneDocument(host, before_invalid_clear2d, before_invalid_clear3d) && acs_editor_prefab_instance3d_clear_root_component_property_override(host, component_reverted, 0, 4) == 0 && SceneDocumentEquals(host, before_invalid_clear2d, before_invalid_clear3d) && acs_editor_prefab_instance3d_clear_root_component_property_override(host, component_reverted, 0, 5) != 0;
    char selectively_applied_scene[32768]{};
    const int selectively_applied_written = ok ? acs_editor_scene3d_serialize(host, selectively_applied_scene, static_cast<int>(sizeof(selectively_applied_scene))) : 0;
    char removed_normal_override_line[64]{};
    const int removed_normal_override_written = std::snprintf(removed_normal_override_line, sizeof(removed_normal_override_line), "PCOVR3D %d 0 5\n", component_reverted);
    ok = ok && acs_editor_prefab_instance3d_root_component_property_override_mask(host, component_reverted, 0) == 0u && acs_editor_prefab_instance3d_root_component_property_override_mask(host, component_reverted, 1) == kMeshRoughness && acs_editor_prefab_instance3d_root_override_mask(host, component_reverted) == kEnabled && selectively_applied_written > 0 && removed_normal_override_written > 0 && std::strstr(selectively_applied_scene, removed_normal_override_line) == nullptr && std::strstr(selectively_applied_scene, retained_mesh_override_line) != nullptr;
    std::string before_repeated_clear2d;
    std::string before_repeated_clear3d;
    ok = ok && SnapshotSceneDocument(host, before_repeated_clear2d, before_repeated_clear3d) && acs_editor_prefab_instance3d_clear_root_component_property_override(host, component_reverted, 0, 5) == 0 && SceneDocumentEquals(host, before_repeated_clear2d, before_repeated_clear3d) && acs_editor_prefab_instance3d_mark_root_component_property_override(host, component_reverted, 0, 5) != 0;
    if (ok) completed_stage = 6u;

    ok = ok && acs_editor_prefab_instance3d_clear_root_component_property_overrides(host, component_reverted) != 0 && acs_editor_prefab_instance3d_root_component_property_override_mask(host, component_reverted, 0) == 0u && acs_editor_prefab_instance3d_mark_root_component_property_override(host, component_reverted, 0, 4) != 0;
    const int reverted = ok ? acs_editor_prefab_instance3d_refresh(host, component_reverted, kSource, kReorderedSubtree) : -1;
    float reverted_roughness = 0.0f;
    char reverted_scene[32768]{};
    const int reverted_written = reverted >= 0 ? acs_editor_scene3d_serialize(host, reverted_scene, static_cast<int>(sizeof(reverted_scene))) : 0;
    ok = ok && reverted >= 0 && acs_editor_node3d_component_prop_get(host, reverted, 0, 4, &reverted_roughness, nullptr, nullptr, nullptr) != 0 && reverted_roughness == 0.1f && acs_editor_prefab_instance3d_root_component_property_override_mask(host, reverted, 0) == 0u && acs_editor_prefab_instance3d_root_override_mask(host, reverted) == 0u && acs_editor_node3d_get_enabled(host, reverted) != 0 && reverted_written > 0 && std::strstr(reverted_scene, "PCOVR3D ") == nullptr;
    if (!ok) {
        std::printf("3D Prefab root component property override contract failed after stage %u.\n", completed_stage);
    }
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
    float default_x = -1.0f;
    float default_y = -1.0f;
    float default_z = -1.0f;
    float default_w = -1.0f;
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
        acs_editor_component_prop_default_at(
            kType, 4,
            &default_x, &default_y, &default_z, &default_w) != 0 &&
        std::abs(default_x - 0.105f) < 1.0e-6f &&
        default_y == 0.0f &&
        default_z == 0.0f &&
        default_w == 0.0f &&
        acs_editor_component_prop_default_at(
            kType, 4, nullptr, nullptr, nullptr, nullptr) != 0 &&
        acs_editor_component_prop_default_at(
            kType, -1,
            &default_x, &default_y, &default_z, &default_w) == 0 &&
        std::abs(default_x - 0.105f) < 1.0e-6f &&
        default_y == 0.0f &&
        default_z == 0.0f &&
        default_w == 0.0f &&
        acs_editor_component_prop_default_at(
            kType, 20,
            &default_x, &default_y, &default_z, &default_w) == 0 &&
        std::abs(default_x - 0.105f) < 1.0e-6f &&
        default_y == 0.0f &&
        default_z == 0.0f &&
        default_w == 0.0f &&
        acs_editor_component_prop_default_at(
            nullptr, 4,
            &default_x, &default_y, &default_z, &default_w) == 0 &&
        std::abs(default_x - 0.105f) < 1.0e-6f &&
        default_y == 0.0f &&
        default_z == 0.0f &&
        default_w == 0.0f &&
        acs_editor_component_prop_default_at(
            "NoSuchComponent", 4,
            &default_x, &default_y, &default_z, &default_w) == 0 &&
        std::abs(default_x - 0.105f) < 1.0e-6f &&
        default_y == 0.0f &&
        default_z == 0.0f &&
        default_w == 0.0f &&
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

/** Play/Stop owns restoration; Scene/Game tabs are display-only. */
bool RunPlayCameraIsolation() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;

    // Establish independent 2D navigation before making 3D the active view.
    acs_editor_set_view3d(host, 0);
    acs_editor_camera_pan(host, 31.25f, -17.5f);
    acs_editor_camera_zoom(host, 1.75f, 240.0f, 180.0f);
    float original_pan_x = 0.0f, original_pan_y = 0.0f;
    float original_zoom = 0.0f;
    acs_editor_camera_get(
        host, &original_pan_x, &original_pan_y, &original_zoom);

    acs_editor_set_view3d(host, 1);
    acs_editor_set_ortho3d(host, 1);
    const bool original_set = acs_editor_camera3d_set(
        host, 0.42f, -0.31f, 27.5f,
        4.25f, 5.5f, -6.75f) != 0;
    float original_camera[6]{};
    const bool original_read = acs_editor_camera3d_get(
        host, &original_camera[0], &original_camera[1],
        &original_camera[2], &original_camera[3],
        &original_camera[4], &original_camera[5]) != 0;

    const bool started =
        acs_editor_play_start(host) != 0 &&
        acs_editor_play_state(host) == 1;
    acs_editor_set_ortho3d(host, 0);
    const bool play_camera_set = acs_editor_camera3d_set(
        host, -1.2f, 0.7f, 44.0f,
        -9.0f, 8.0f, 7.0f) != 0;
    float play_camera_before_tabs[6]{};
    acs_editor_camera3d_get(
        host, &play_camera_before_tabs[0],
        &play_camera_before_tabs[1],
        &play_camera_before_tabs[2],
        &play_camera_before_tabs[3],
        &play_camera_before_tabs[4],
        &play_camera_before_tabs[5]);

    acs_editor_set_game_view(host, 1);
    const bool game_selected =
        acs_editor_is_game_view(host) == 1 &&
        acs_editor_play_state(host) == 1;
    acs_editor_set_game_view(host, 0);
    float play_camera_after_tabs[6]{};
    acs_editor_camera3d_get(
        host, &play_camera_after_tabs[0],
        &play_camera_after_tabs[1],
        &play_camera_after_tabs[2],
        &play_camera_after_tabs[3],
        &play_camera_after_tabs[4],
        &play_camera_after_tabs[5]);
    const bool tabs_preserved =
        acs_editor_is_game_view(host) == 0 &&
        acs_editor_play_state(host) == 1 &&
        std::memcmp(
            play_camera_before_tabs, play_camera_after_tabs,
            sizeof(play_camera_before_tabs)) == 0;

    const bool stopped =
        acs_editor_play_stop(host) != 0 &&
        acs_editor_play_state(host) == 0;
    float restored_camera[6]{};
    acs_editor_camera3d_get(
        host, &restored_camera[0], &restored_camera[1],
        &restored_camera[2], &restored_camera[3],
        &restored_camera[4], &restored_camera[5]);
    float restored_pan_x = 0.0f, restored_pan_y = 0.0f;
    float restored_zoom = 0.0f;
    acs_editor_camera_get(
        host, &restored_pan_x, &restored_pan_y, &restored_zoom);
    const bool exact_restore =
        acs_editor_get_view3d(host) == 1 &&
        acs_editor_get_ortho3d(host) == 1 &&
        std::memcmp(
            original_camera, restored_camera,
            sizeof(original_camera)) == 0 &&
        original_pan_x == restored_pan_x &&
        original_pan_y == restored_pan_y &&
        original_zoom == restored_zoom;

    acs_editor_destroy(host);
    return original_set && original_read && started &&
           play_camera_set && game_selected && tabs_preserved &&
           stopped && exact_restore;
}

/** Legacy 2D Game View fallback is authored-bounds based, never editor-pose based. */
bool RunDeterministicGameCamera2D() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    const int left = acs_editor_add_node(host, "Node", -1);
    const int right = acs_editor_add_node(host, "Node", -1);
    if (left < 0 || right < 0) {
        acs_editor_destroy(host);
        return false;
    }
    acs_editor_node_set_transform(
        host, left, -120.0f, 40.0f, 0.35f, 2.0f, 1.0f);
    acs_editor_node_set_transform(
        host, right, 310.0f, 170.0f, -0.2f, 1.0f, 3.0f);
    float first[3]{};
    float second[3]{};
    const bool first_ok = acs_editor_game_camera2d_get(
        host, 1280u, 720u, &first[0], &first[1], &first[2]) != 0;
    acs_editor_camera_pan(host, 900.0f, -500.0f);
    acs_editor_camera_zoom(host, 3.0f, 13.0f, 17.0f);
    const bool second_ok = acs_editor_game_camera2d_get(
        host, 1280u, 720u, &second[0], &second[1], &second[2]) != 0;
    const bool independent =
        std::memcmp(first, second, sizeof(first)) == 0 &&
        std::isfinite(first[0]) && std::isfinite(first[1]) &&
        std::isfinite(first[2]) && first[2] > 0.0f;
    acs_editor_destroy(host);

    void* const empty = acs_editor_create();
    if (empty == nullptr) return false;
    acs_editor_camera_pan(empty, -123.0f, 456.0f);
    acs_editor_camera_zoom(empty, 2.0f, 1.0f, 1.0f);
    float empty_center_x = 1.0f;
    float empty_center_y = 1.0f;
    float empty_zoom = 0.0f;
    const bool empty_default =
        acs_editor_game_camera2d_get(
            empty, 1920u, 1080u, &empty_center_x,
            &empty_center_y, &empty_zoom) != 0 &&
        empty_center_x == 0.0f &&
        empty_center_y == 0.0f &&
        empty_zoom == 1.0f;
    acs_editor_destroy(empty);
    return first_ok && second_ok && independent && empty_default;
}

/** Game View resolves fallback/authored/preview cameras without editor mutation. */
bool RunGameCameraResolutionAndPreview() noexcept
{
    void* const host = acs_editor_create();
    if (host == nullptr) return false;
    acs_editor_set_view3d(host, 1);
    acs_editor_scene3d_new(host);
    const int mesh_a = acs_editor_add_node3d(host, 0, "BoundsA");
    const int mesh_b = acs_editor_add_node3d(host, 1, "BoundsB");
    const bool meshes_ready =
        mesh_a >= 0 && mesh_b >= 0 &&
        acs_editor_node3d_set_transform(
            host, mesh_a, -4.0f, 1.0f, 3.0f,
            0.0f, 0.0f, 0.0f, 2.0f, 1.0f, 1.0f) != 0 &&
        acs_editor_node3d_set_transform(
            host, mesh_b, 8.0f, 3.0f, -2.0f,
            0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f) != 0;

    int fallback_projection_a = -1, fallback_source_a = -9;
    int fallback_projection_b = -1, fallback_source_b = -9;
    float fallback_position_a[3]{}, fallback_forward_a[3]{};
    float fallback_up_a[3]{}, fallback_params_a[4]{};
    float fallback_position_b[3]{}, fallback_forward_b[3]{};
    float fallback_up_b[3]{}, fallback_params_b[4]{};
    const bool fallback_a = acs_editor_game_camera3d_get(
        host, 16.0f / 9.0f,
        &fallback_projection_a, &fallback_source_a,
        fallback_position_a, fallback_forward_a,
        fallback_up_a, fallback_params_a) != 0;
    acs_editor_camera3d_set(
        host, 2.3f, -0.8f, 190.0f,
        70.0f, -30.0f, 44.0f);
    const bool fallback_b = acs_editor_game_camera3d_get(
        host, 16.0f / 9.0f,
        &fallback_projection_b, &fallback_source_b,
        fallback_position_b, fallback_forward_b,
        fallback_up_b, fallback_params_b) != 0;
    const bool fallback_independent =
        fallback_source_a == -1 && fallback_source_b == -1 &&
        fallback_projection_a == 0 && fallback_projection_b == 0 &&
        std::memcmp(
            fallback_position_a, fallback_position_b,
            sizeof(fallback_position_a)) == 0 &&
        std::memcmp(
            fallback_forward_a, fallback_forward_b,
            sizeof(fallback_forward_a)) == 0 &&
        std::memcmp(
            fallback_up_a, fallback_up_b,
            sizeof(fallback_up_a)) == 0 &&
        std::memcmp(
            fallback_params_a, fallback_params_b,
            sizeof(fallback_params_a)) == 0;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const bool aspect_rejected =
        acs_editor_game_camera3d_get(
            host, nan, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr) == 0 &&
        acs_editor_game_camera3d_get(
            host, 0.0f, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr) == 0 &&
        acs_editor_game_camera3d_get(
            host, 1.0e20f, nullptr, nullptr, nullptr,
            nullptr, nullptr, nullptr) == 0;

    const int active_camera =
        acs_editor_add_camera3d(
            host, "ActiveCamera", "camera.active");
    const int preview_camera =
        acs_editor_add_camera3d(
            host, "PreviewCamera", "camera.preview");
    const bool authored =
        active_camera >= 0 && preview_camera >= 0 &&
        acs_editor_node3d_set_transform(
            host, active_camera, 2.0f, 4.0f, 6.0f,
            0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f) != 0 &&
        acs_editor_node3d_camera_set(
            host, active_camera, "camera.active",
            1, 100, 1, 55.0f, 18.0f, 0.25f, 900.0f) != 0 &&
        acs_editor_node3d_set_transform(
            host, preview_camera, -3.0f, 5.0f, 7.0f,
            0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f) != 0 &&
        acs_editor_node3d_camera_set(
            host, preview_camera, "camera.preview",
            0, 0, 0, 67.0f, 12.0f, 0.1f, 400.0f) != 0;

    int projection = -1, source = -1;
    float position[3]{}, forward[3]{}, up[3]{}, params[4]{};
    const bool active_resolved =
        acs_editor_game_camera3d_get(
            host, 2.0f, &projection, &source,
            position, forward, up, params) != 0 &&
        source == active_camera && projection == 1 &&
        position[0] == 2.0f && position[1] == 4.0f &&
        position[2] == 6.0f &&
        std::abs(forward[0]) < 1.0e-6f &&
        std::abs(forward[1]) < 1.0e-6f &&
        std::abs(forward[2] - 1.0f) < 1.0e-6f &&
        params[1] == 18.0f && params[2] == 0.25f &&
        params[3] == 900.0f;

    char before_preview[32768]{};
    char after_preview[32768]{};
    const int before_written = acs_editor_scene3d_serialize(
        host, before_preview,
        static_cast<int>(sizeof(before_preview)));
    int preview_id = -1;
    const bool preview_resolved =
        acs_editor_game_camera_preview_set(
            host, preview_camera) != 0 &&
        acs_editor_game_camera_preview_get(
            host, &preview_id) != 0 &&
        preview_id == preview_camera &&
        acs_editor_game_camera3d_get(
            host, 2.0f, &projection, &source,
            position, forward, up, params) != 0 &&
        source == preview_camera && projection == 0 &&
        position[0] == -3.0f && position[1] == 5.0f &&
        position[2] == 7.0f;
    const int after_written = acs_editor_scene3d_serialize(
        host, after_preview,
        static_cast<int>(sizeof(after_preview)));
    const bool preview_nonpersistent =
        before_written > 0 && before_written == after_written &&
        std::strcmp(before_preview, after_preview) == 0;

    acs_editor_node3d_set_enabled(
        host, preview_camera, 0);
    preview_id = 77;
    const bool invalid_preview_cleared =
        acs_editor_game_camera_preview_get(
            host, &preview_id) == 0 &&
        preview_id == -1 &&
        acs_editor_game_camera3d_get(
            host, 2.0f, &projection, &source,
            position, forward, up, params) != 0 &&
        source == active_camera;

    const int camera_count = acs_editor_camera3d_count(host);
    const bool enumeration =
        camera_count == 2 &&
        acs_editor_camera3d_node_id_at(host, 0) ==
            active_camera &&
        acs_editor_camera3d_node_id_at(host, 1) ==
            preview_camera &&
        acs_editor_camera3d_node_id_at(host, -1) == -1 &&
        acs_editor_camera3d_node_id_at(host, camera_count) == -1;
    const bool frustum_toggle =
        acs_editor_camera_frustum_get_visible(host) == 1 &&
        (acs_editor_camera_frustum_set_visible(host, 0), true) &&
        acs_editor_camera_frustum_get_visible(host) == 0 &&
        (acs_editor_camera_frustum_set_visible(host, 1), true) &&
        acs_editor_camera_frustum_get_visible(host) == 1;
    acs_editor_game_camera_preview_clear(host);
    preview_id = 55;
    const bool explicitly_cleared =
        acs_editor_game_camera_preview_get(
            host, &preview_id) == 0 &&
        preview_id == -1;

    acs_editor_destroy(host);
    return meshes_ready && fallback_a && fallback_b &&
           fallback_independent && aspect_rejected && authored &&
           active_resolved && preview_resolved &&
           preview_nonpersistent && invalid_preview_cleared &&
           enumeration && frustum_toggle && explicitly_cleared;
}

/** Logical Camera View requests stay bounded, isolated and non-persistent. */
bool RunCameraViewRequestContract() noexcept
{
    using namespace acs::editor_camera_view;
    static_assert(sizeof(FSnapshot) == 60u);
    static_assert(kMaximumRequests == 8u);

    CRegistry registry;
    std::uint64_t logical_a = 0u;
    std::uint64_t logical_b = 0u;
    const bool registry_created =
        registry.Create(10, "camera.logical-a", 1280, 720, logical_a) &&
        registry.Create(20, "camera.logical-b", 640, 360, logical_b) &&
        logical_a != 0u && logical_b != 0u && logical_a != logical_b;
    FSnapshot logical_snapshot{};
    const bool exclusive_presenter =
        registry_created &&
        registry.BindPresenter(logical_a) &&
        !registry.BindPresenter(logical_b) &&
        registry.Snapshot(logical_a, logical_snapshot) &&
        (logical_snapshot.flags & SnapshotPresenter) != 0u &&
        logical_snapshot.target_kind ==
            static_cast<std::uint32_t>(ETargetKind::SharedSwapchain);
    registry.MarkPresenterRendered(77u, 1280u, 720u);
    const bool latest_metadata =
        registry.Snapshot(logical_a, logical_snapshot) &&
        logical_snapshot.latest_frame_serial == 77u &&
        logical_snapshot.presented_width == 1280u &&
        logical_snapshot.presented_height == 720u &&
        (logical_snapshot.flags &
         (SnapshotTargetRecreatePending |
          SnapshotHistoryResetPending)) == 0u;
    const std::uint32_t history_before_stale =
        logical_snapshot.history_generation;
    registry.MarkAllCamerasStale();
    const bool stale_isolated =
        registry.Snapshot(logical_a, logical_snapshot) &&
        (logical_snapshot.flags & SnapshotCameraStale) != 0u &&
        (logical_snapshot.flags & SnapshotPresenter) == 0u &&
        registry.PresenterRequestId() == 0u &&
        registry.Update(
            logical_a, 10, "camera.logical-a", 1280, 720) &&
        registry.Snapshot(logical_a, logical_snapshot) &&
        (logical_snapshot.flags & SnapshotCameraStale) == 0u &&
        logical_snapshot.history_generation >
            history_before_stale;
    const std::uint64_t stale_logical_a = logical_a;
    const bool aba_safe =
        registry.Destroy(logical_a) &&
        registry.Create(30, "camera.logical-c", 320, 180, logical_a) &&
        logical_a != stale_logical_a &&
        !registry.Destroy(stale_logical_a);
    CRegistry bounded_registry;
    std::uint64_t bounded_ids[kMaximumRequests]{};
    bool bounded_capacity = true;
    for (std::uint32_t index = 0u;
         index < kMaximumRequests; ++index) {
        char stable_id[32]{};
        std::snprintf(
            stable_id, sizeof(stable_id),
            "camera.bounded-%u", index);
        bounded_capacity =
            bounded_capacity &&
            bounded_registry.Create(
                static_cast<std::int32_t>(index),
                stable_id, 320, 180,
                bounded_ids[index]);
    }
    std::uint64_t over_capacity = 99u;
    bounded_capacity =
        bounded_capacity &&
        !bounded_registry.Create(
            99, "camera.over-capacity",
            320, 180, over_capacity) &&
        over_capacity == 0u;

    void* host = acs_editor_create();
    if (host == nullptr) return false;
    const int camera_a =
        acs_editor_add_camera3d(
            host, "RequestCameraA", "camera.request-a");
    const int camera_b =
        acs_editor_add_camera3d(
            host, "RequestCameraB", "camera.request-b");
    char before[32768]{};
    char after[32768]{};
    const int before_written =
        acs_editor_scene3d_serialize(
            host, before, static_cast<int>(sizeof(before)));

    std::uint64_t request_a = 0u;
    std::uint64_t request_b = 0u;
    std::uint64_t rejected = 55u;
    const bool created =
        camera_a >= 0 && camera_b >= 0 &&
        acs_editor_camera_view_request_create(
            host, camera_a, "camera.request-a",
            1280, 720, &request_a) != 0 &&
        acs_editor_camera_view_request_create(
            host, camera_b, "camera.request-b",
            640, 360, &request_b) != 0 &&
        request_a != 0u && request_b != 0u &&
        acs_editor_camera_view_request_create(
            host, camera_a, "camera.request-a",
            8192, 8192, &rejected) == 0 &&
        rejected == 0u;

    FSnapshot snapshot_a{};
    const bool bound_exclusively =
        created &&
        acs_editor_camera_view_request_bind_presenter(
            host, request_a) != 0 &&
        acs_editor_camera_view_request_bind_presenter(
            host, request_b) == 0 &&
        acs_editor_game_camera_preview_set(host, camera_b) == 0 &&
        acs_editor_camera_view_request_get(
            host, request_a, &snapshot_a,
            static_cast<std::uint32_t>(sizeof(snapshot_a))) != 0 &&
        snapshot_a.request_id == request_a &&
        snapshot_a.camera_node_id == camera_a &&
        snapshot_a.target_kind ==
            static_cast<std::uint32_t>(
                ETargetKind::SharedSwapchain) &&
        (snapshot_a.flags & SnapshotPresenter) != 0u;

    const std::uint32_t history_before_update =
        snapshot_a.history_generation;
    const bool updated_independently =
        acs_editor_camera_view_request_update(
            host, request_a, camera_b, "camera.request-b",
            1920, 1080) != 0 &&
        acs_editor_camera_view_request_get(
            host, request_a, &snapshot_a,
            static_cast<std::uint32_t>(sizeof(snapshot_a))) != 0 &&
        snapshot_a.camera_node_id == camera_b &&
        snapshot_a.width == 1920u &&
        snapshot_a.height == 1080u &&
        snapshot_a.history_generation > history_before_update &&
        (snapshot_a.flags & SnapshotHistoryResetPending) != 0u;

    FSnapshot snapshot_b{};
    acs_editor_node3d_set_enabled(host, camera_b, 0);
    const bool stale_identity_fails_closed =
        acs_editor_camera_view_request_unbind_presenter(
            host, request_a) != 0 &&
        acs_editor_camera_view_request_bind_presenter(
            host, request_b) == 0 &&
        acs_editor_camera_view_request_get(
            host, request_b, &snapshot_b,
            static_cast<std::uint32_t>(sizeof(snapshot_b))) != 0 &&
        (snapshot_b.flags & SnapshotCameraStale) != 0u &&
        (snapshot_b.flags & SnapshotPresenter) == 0u;
    acs_editor_node3d_set_enabled(host, camera_b, 1);
    const bool revalidated =
        acs_editor_camera_view_request_update(
            host, request_b, camera_b, "camera.request-b",
            640, 360) != 0 &&
        acs_editor_camera_view_request_bind_presenter(
            host, request_b) != 0;

    const bool scene_replacement_stales_all =
        before_written > 0 &&
        acs_editor_scene3d_load_text(host, before) != 0 &&
        acs_editor_camera_view_request_get(
            host, request_a, &snapshot_a,
            static_cast<std::uint32_t>(sizeof(snapshot_a))) != 0 &&
        acs_editor_camera_view_request_get(
            host, request_b, &snapshot_b,
            static_cast<std::uint32_t>(sizeof(snapshot_b))) != 0 &&
        (snapshot_a.flags & SnapshotCameraStale) != 0u &&
        (snapshot_b.flags & SnapshotCameraStale) != 0u &&
        (snapshot_b.flags & SnapshotPresenter) == 0u &&
        acs_editor_camera_view_request_update(
            host, request_b, camera_b, "camera.request-b",
            640, 360) != 0;

    const int after_written =
        acs_editor_scene3d_serialize(
            host, after, static_cast<int>(sizeof(after)));
    const bool non_persistent =
        before_written > 0 &&
        after_written == before_written &&
        std::strcmp(before, after) == 0;
    const bool destroyed =
        acs_editor_camera_view_request_destroy(
            host, request_a) != 0 &&
        acs_editor_camera_view_request_destroy(
            host, request_b) != 0 &&
        acs_editor_camera_view_request_get(
            host, request_a, &snapshot_a,
            static_cast<std::uint32_t>(sizeof(snapshot_a))) == 0;

    acs_editor_destroy(host);
    return registry_created && exclusive_presenter &&
           latest_metadata && stale_isolated && aba_safe &&
           bounded_capacity &&
           created && bound_exclusively &&
           updated_independently &&
           stale_identity_fails_closed && revalidated &&
           scene_replacement_stales_all &&
           non_persistent && destroyed;
}

/** Row-vector D3D planes, conservative scale and fail-open draw policy stay exact. */
bool RunFrustumCullingContract() noexcept
{
    using namespace acs::editor_frustum_culling;
    const acs::FMat4 view = acs::FMat4::LookAtLH(
        acs::FVec3{0.0f, 0.0f, 0.0f},
        acs::FVec3{0.0f, 0.0f, 1.0f},
        acs::FVec3{0.0f, 1.0f, 0.0f});
    const acs::FMat4 projection =
        acs::FMat4::PerspectiveFovLH(
            90.0f * 3.14159265f / 180.0f,
            1.0f, 1.0f, 10.0f);
    FPlane planes[6];
    if (!ExtractPlanes(view * projection, planes))
        return false;

    const FNodeDecision inside = EvaluateSphere(
        planes, acs::FVec3{0.0f, 0.0f, 5.0f},
        0.25f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision behind_near = EvaluateSphere(
        planes, acs::FVec3{0.0f, 0.0f, 0.2f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision beyond_far = EvaluateSphere(
        planes, acs::FVec3{0.0f, 0.0f, 11.0f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision beyond_left = EvaluateSphere(
        planes, acs::FVec3{-8.0f, 0.0f, 5.0f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision beyond_right = EvaluateSphere(
        planes, acs::FVec3{8.0f, 0.0f, 5.0f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const bool plane_contract =
        inside.valid && inside.visible &&
        behind_near.valid && !behind_near.visible &&
        beyond_far.valid && !beyond_far.visible &&
        beyond_left.valid && !beyond_left.visible &&
        beyond_right.valid && !beyond_right.visible;

    const FNodeDecision uniform = EvaluateSphere(
        planes, acs::FVec3{8.0f, 0.0f, 5.0f},
        1.0f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision nonuniform = EvaluateSphere(
        planes, acs::FVec3{8.0f, 0.0f, 5.0f},
        1.0f, acs::FVec3{1.0f, 3.0f, 2.0f});
    const FNodeDecision displaced_surface = EvaluateSphere(
        planes, acs::FVec3{8.0f, 0.0f, 5.0f},
        1.0f, acs::FVec3{1.0f, 1.0f, 1.0f},
        4.0f);
    const bool scale_contract =
        uniform.valid && !uniform.visible &&
        uniform.world_radius == 1.0f &&
        nonuniform.valid && nonuniform.visible &&
        nonuniform.world_radius == 3.0f &&
        displaced_surface.valid &&
        displaced_surface.visible &&
        displaced_surface.world_radius == 5.0f;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const FNodeDecision invalid = EvaluateSphere(
        planes, acs::FVec3{0.0f, 0.0f, 5.0f},
        nan, acs::FVec3{1.0f, 1.0f, 1.0f});
    FFrameDecision invalid_frame{};
    invalid_frame.Apply(inside);
    invalid_frame.Apply(invalid);
    const bool fail_open =
        !invalid.valid && invalid.visible &&
        !invalid_frame.enabled &&
        invalid_frame.tested == 0u &&
        invalid_frame.visible == 0u &&
        invalid_frame.culled == 0u &&
        ShouldSubmitOpaque(
            invalid_frame.enabled, false);

    FFrameDecision culled_frame{};
    culled_frame.Apply(beyond_right);
    const bool culled_skips_opaque =
        culled_frame.enabled &&
        culled_frame.tested == 1u &&
        culled_frame.visible == 0u &&
        culled_frame.culled == 1u &&
        !ShouldSubmitOpaque(
            culled_frame.enabled,
            beyond_right.visible);

    const acs::FMat4 orthographic =
        acs::FMat4::OrthoLH(8.0f, 6.0f, 1.0f, 10.0f);
    FPlane orthographic_planes[6]{};
    const bool orthographic_planes_valid =
        ExtractPlanes(view * orthographic, orthographic_planes);
    const FNodeDecision orthographic_inside = EvaluateSphere(
        orthographic_planes, acs::FVec3{3.5f, 2.5f, 5.0f},
        0.25f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision orthographic_outside_x = EvaluateSphere(
        orthographic_planes, acs::FVec3{4.5f, 0.0f, 5.0f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const FNodeDecision orthographic_outside_y = EvaluateSphere(
        orthographic_planes, acs::FVec3{0.0f, 3.5f, 5.0f},
        0.1f, acs::FVec3{1.0f, 1.0f, 1.0f});
    const bool orthographic_contract =
        orthographic_planes_valid &&
        orthographic_inside.valid &&
        orthographic_inside.visible &&
        orthographic_outside_x.valid &&
        !orthographic_outside_x.visible &&
        orthographic_outside_y.valid &&
        !orthographic_outside_y.visible;

    const acs::u8 visibility[] = {1u, 0u, 1u};
    const FSubmissionMaskView enabled_mask{
        true, visibility,
        static_cast<acs::u32>(
            sizeof(visibility) / sizeof(visibility[0]))};
    acs::u32 submitted_indices = 0u;
    const acs::u32 submitted_count = ForEachSubmittedNode(
        enabled_mask, 3u,
        [&](acs::u32 index) noexcept {
            submitted_indices |= 1u << index;
        });
    const bool production_submission_contract =
        submitted_count == 2u &&
        submitted_indices == ((1u << 0u) | (1u << 2u)) &&
        !AnySubmittedNode(
            enabled_mask, 3u,
            [](acs::u32 index) noexcept { return index == 1u; }) &&
        AnySubmittedNode(
            enabled_mask, 3u,
            [](acs::u32 index) noexcept { return index == 2u; });

    constexpr acs::usize batch_count = 9u;
    acs::FVec3 centers[batch_count] = {{0.0f, 0.0f, 5.0f}, {8.0f, 0.0f, 5.0f}, {-8.0f, 0.0f, 5.0f}, {0.0f, 8.0f, 5.0f}, {0.0f, -8.0f, 5.0f}, {0.0f, 0.0f, 11.0f}, {0.0f, 0.0f, 0.2f}, {1.0f, 1.0f, 5.0f}, {nan, 0.0f, 5.0f}};
    acs::f32 radii[batch_count] = {0.25f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.1f, 0.5f, 1.0f};
    acs::FVec3 scales[batch_count] = {{1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {1,1,1}, {2,3,4}, {1,1,1}};
    acs::f32 paddings[batch_count] = {0, 0, 0, 0, 0, 0, 0, 0.5f, 0};
    FNodeDecision batch[batch_count]{};
    EvaluateSpheresBatch(planes, centers, radii, scales, paddings, batch_count, batch);
    bool batch_scalar_parity = true;
    for (acs::usize i = 0u; i < batch_count; ++i) {
        const FNodeDecision scalar = EvaluateSphere(planes, centers[i], radii[i], scales[i], paddings[i]);
        batch_scalar_parity =
            batch_scalar_parity &&
            batch[i].valid == scalar.valid &&
            batch[i].visible == scalar.visible &&
            batch[i].world_radius == scalar.world_radius;
    }

    return plane_contract && scale_contract &&
           fail_open && culled_skips_opaque &&
           orthographic_contract &&
           production_submission_contract &&
           batch_scalar_parity;
}

/**
 * Exercise the real DrawScene3D publication path when a DX12 adapter is
 * available. Headless/no-adapter environments still run the pure and fake-RHI
 * contracts above.
 */
bool RunRenderedFrustumProfilerContract() noexcept
{
    using namespace acs::editor_profiler;
    HWND const window = ::CreateWindowExW(
        0, L"STATIC", L"ACS editor ABI culling integration test",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 256, 256,
        nullptr, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr) return true;

    void* const host = acs_editor_create();
    if (host == nullptr) {
        ::DestroyWindow(window);
        return false;
    }
    if (acs_editor_attach(host, window, 256u, 256u) == 0) {
        acs_editor_destroy(host);
        ::DestroyWindow(window);
        return true;
    }

    // Keep the integration frame deterministic and inexpensive. Lowest still
    // initializes and exercises the complete opaque PBR camera path.
    const bool quality_applied =
        acs_editor_settings_set(
            host, "Rendering", "QualityLevel", "Lowest") != 0;
    unsigned completed = 0u;
    unsigned total = 0u;
    int startup_state =
        acs_editor_startup_status(host, &completed, &total);
    const ULONGLONG startup_deadline =
        ::GetTickCount64() + 30000u;
    while (startup_state == 0 &&
           ::GetTickCount64() < startup_deadline) {
        const unsigned previous_completed = completed;
        acs_editor_render(host, 1.0f / 60.0f);
        startup_state =
            acs_editor_startup_status(host, &completed, &total);
        if (startup_state == 0 &&
            completed == previous_completed) {
            ::Sleep(1u);
        }
    }

    bool ok = quality_applied && startup_state > 0 &&
              completed == total && total > 1u;
    if (ok) {
        // この描画検証が全ノードを所有するため、明示的な空グラフから開始する。
        acs_editor_scene3d_new(host);
    }
    const int inside = ok
        ? acs_editor_add_node3d(host, 0, "CullingInside")
        : -1;
    const int outside = ok
        ? acs_editor_add_node3d(host, 0, "CullingOutside")
        : -1;
    const int water_inside = ok
        ? acs_editor_add_node3d(host, 2, "WaterCullingInside")
        : -1;
    const int water_outside = ok
        ? acs_editor_add_node3d(host, 2, "WaterCullingOutside")
        : -1;
    const int camera = ok
        ? acs_editor_add_camera3d(
              host, "CullingCamera", "camera.culling.integration")
        : -1;
    ok = ok && inside >= 0 && outside >= 0 &&
         water_inside >= 0 && water_outside >= 0 && camera >= 0 &&
         acs_editor_node3d_add_component(
             host, water_inside,
             "AWaterSurface3DComponent") != 0 &&
         acs_editor_node3d_add_component(
             host, water_outside,
             "AWaterSurface3DComponent") != 0 &&
         acs_editor_node3d_set_transform(
             host, inside,
             0.0f, 0.0f, 5.0f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0 &&
         acs_editor_node3d_set_transform(
             host, outside,
             20.0f, 0.0f, 5.0f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0 &&
         acs_editor_node3d_set_transform(
             host, water_inside,
             0.0f, 0.0f, 5.0f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0 &&
         acs_editor_node3d_set_transform(
             host, water_outside,
             20.0f, 0.0f, 5.0f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0 &&
         acs_editor_node3d_set_transform(
             host, camera,
             0.0f, 0.0f, 0.0f,
             0.0f, 0.0f, 0.0f,
             1.0f, 1.0f, 1.0f) != 0 &&
         acs_editor_node3d_camera_set(
             host, camera, "camera.culling.integration",
             0, 100, 1, 90.0f, 8.0f, 1.0f, 10.0f) != 0;

    acs_editor_set_view3d(host, 1);
    acs_editor_set_game_view(host, 1);
    FSnapshot perspective{};
    bool perspective_published = false;
    for (unsigned frame = 0u;
         frame < 8u && ok && !perspective_published; ++frame) {
        acs_editor_render(host, 1.0f / 60.0f);
        perspective = FSnapshot{};
        perspective_published =
            acs_editor_profiler_get(
                host, &perspective,
                static_cast<unsigned>(sizeof(perspective))) != 0 &&
            (perspective.flags & FrustumCullingEnabled) != 0u;
    }
    const bool perspective_counts =
        perspective_published &&
        (perspective.flags &
         (View3D | GameView | RuntimeSceneCamera)) ==
            (View3D | GameView | RuntimeSceneCamera) &&
        perspective.frustum_tested == 4u &&
        perspective.frustum_visible == 2u &&
        perspective.frustum_culled == 2u &&
        perspective.active_camera_node_id == camera;

    ok = ok && acs_editor_node3d_camera_set(
        host, camera, "camera.culling.integration",
        1, 100, 1, 90.0f, 8.0f, 1.0f, 10.0f) != 0;
    FSnapshot orthographic{};
    bool orthographic_published = false;
    for (unsigned frame = 0u;
         frame < 8u && ok && !orthographic_published; ++frame) {
        acs_editor_render(host, 1.0f / 60.0f);
        orthographic = FSnapshot{};
        orthographic_published =
            acs_editor_profiler_get(
                host, &orthographic,
                static_cast<unsigned>(sizeof(orthographic))) != 0 &&
            (orthographic.flags & FrustumCullingEnabled) != 0u;
    }
    int projection = -1;
    int source = -1;
    float position[3]{};
    float forward[3]{};
    float up[3]{};
    float parameters[4]{};
    const bool orthographic_counts =
        orthographic_published &&
        orthographic.frustum_tested == 4u &&
        orthographic.frustum_visible == 2u &&
        orthographic.frustum_culled == 2u &&
        orthographic.active_camera_node_id == camera &&
        acs_editor_game_camera3d_get(
            host, 1.0f, &projection, &source,
            position, forward, up, parameters) != 0 &&
        projection == 1 && source == camera &&
        parameters[1] == 8.0f &&
        parameters[2] == 1.0f &&
        parameters[3] == 10.0f;

    // A published frame that exits before DrawScene3D must not leak the last
    // successful frame's camera or culling counters into profiler consumers.
    acs_editor_set_scene_presentation_suppressed(host, 1);
    acs_editor_render(host, 1.0f / 60.0f);
    FSnapshot suppressed{};
    const bool early_frame_zeroed =
        acs_editor_profiler_get(
            host, &suppressed,
            static_cast<unsigned>(sizeof(suppressed))) != 0 &&
        suppressed.frame_index > orthographic.frame_index &&
        (suppressed.flags & ScenePresentationSuppressed) != 0u &&
        (suppressed.flags &
         (FrustumCullingEnabled | RuntimeSceneCamera)) == 0u &&
        suppressed.frustum_tested == 0u &&
        suppressed.frustum_visible == 0u &&
        suppressed.frustum_culled == 0u &&
        suppressed.active_camera_node_id == -1;
    acs_editor_set_scene_presentation_suppressed(host, 0);
    acs_editor_render(host, 1.0f / 60.0f);
    FSnapshot resumed{};
    const bool resumed_frame_published =
        acs_editor_profiler_get(
            host, &resumed,
            static_cast<unsigned>(sizeof(resumed))) != 0 &&
        resumed.frame_index > suppressed.frame_index &&
        (resumed.flags & ScenePresentationSuppressed) == 0u;

    const bool result =
        ok && perspective_counts && orthographic_counts &&
        early_frame_zeroed && resumed_frame_published;
    if (!result) {
        std::printf(
            "Rendered frustum profiler contract failed: "
            "startup=%d progress=%u/%u persp=%d [%u,%u,%u flags=%u cam=%d] "
            "ortho=%d [%u,%u,%u flags=%u cam=%d projection=%d source=%d] "
            "early_zero=%d [%u,%u,%u flags=%u cam=%d] "
            "resumed=%d [frame=%llu flags=%u]\n",
            startup_state, completed, total,
            perspective_published ? 1 : 0,
            perspective.frustum_tested,
            perspective.frustum_visible,
            perspective.frustum_culled,
            perspective.flags,
            perspective.active_camera_node_id,
            orthographic_published ? 1 : 0,
            orthographic.frustum_tested,
            orthographic.frustum_visible,
            orthographic.frustum_culled,
            orthographic.flags,
            orthographic.active_camera_node_id,
            projection, source,
            early_frame_zeroed ? 1 : 0,
            suppressed.frustum_tested,
            suppressed.frustum_visible,
            suppressed.frustum_culled,
            suppressed.flags,
            suppressed.active_camera_node_id,
            resumed_frame_published ? 1 : 0,
            static_cast<unsigned long long>(resumed.frame_index),
            resumed.flags);
    }
    acs_editor_destroy(host);
    ::DestroyWindow(window);
    return result;
}

} // namespace

int main()
{
    acs_editor_destroy(nullptr); // null 破棄は常に no-op であることも通す。

    // OS とランタイムの初回遅延初期化を基準値から除外する。
    if (!RunAbiCapabilityContract()) return 18;
    if (!RunOptionalServiceDiagnosticContract()) return 26;
    if (!RunOneLifecycle()) return 1;
    if (!RunEmptyScene3DStartupContract()) return 28;
    if (!RunStartupStatusContract()) return 13;
    if (!RunDestroyDuringAsyncWarmup()) return 14;
    if (!RunProfilerSnapshotContract()) return 12;
    if (!RunCloudWorkloadSnapshotContract()) return 19;
    if (!RunZeroScaleSafety()) return 8;
    if (!RunMaskedTransformMutationContract()) return 27;
    if (!RunScene3DSerializationGrowth()) return 9;
    if (!RunSceneDocumentStrictPreflight()) return 17;
    if (!RunTransactionalScene3DSubtreePaste()) return 29;
    if (!RunPrefabInstance3DRefreshTransaction()) return 30;
    if (!RunPrefabInstance3DStableIdentity()) return 31;
    if (!RunPrefabInstance3DSourceNodeIdentity()) return 34;
    if (!RunPrefabInstance3DRootPropertyOverrides()) return 32;
    if (!RunPrefabInstance3DNodePropertyOverrides()) return 35;
    if (!RunPrefabInstance3DRootComponentPropertyOverrides()) return 33;
    if (!RunWater3DComponentRoundTrip()) return 15;
    if (!RunWater3DPointerOcclusion()) return 16;
    if (!RunCamera3DStateSafety()) return 10;
    if (!RunCamera3DFrameAll()) return 11;
    if (!RunPlayCameraIsolation()) return 20;
    if (!RunDeterministicGameCamera2D()) return 21;
    if (!RunGameCameraResolutionAndPreview()) return 22;
    if (!RunCameraViewRequestContract()) return 25;
    if (!RunFrustumCullingContract()) return 23;
    if (!RunRenderedFrustumProfilerContract()) return 24;
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
