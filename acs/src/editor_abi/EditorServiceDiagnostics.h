// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

#include <cstddef>
#include <type_traits>

namespace acs::editor_service_diagnostics {

/**
 * Optional-service status and typed native error payload.
 *
 * Version 2 appends a typed error tail to the complete version-1 prefix.
 * Callers select the prefix they understand through the first two fields.
 */
inline constexpr u32 kLegacyDiagnosticVersion = 1u;
inline constexpr u32 kLegacyDiagnosticSize = 192u;
inline constexpr u32 kDiagnosticVersion = 2u;
inline constexpr u32 kDiagnosticSize = 256u;
inline constexpr u32 kMessageBytes = 160u;
inline constexpr u32 kStableCodeBytes = 48u;

enum class EService : u32 {
    Profiler = 1u,
    VolumetricCloudWorkload = 2u,
    CameraViewRequests = 3u,
};

enum class EState : u32 {
    Enabled = 1u,
    Disabled = 2u,
    Pending = 3u,
    Inactive = 4u,
    Failed = 5u,
};

enum class EReason : u32 {
    None = 0u,
    CapabilityNotAdvertised = 1u,
    InvalidHost = 2u,
    StartupPending = 3u,
    SceneFeatureInactive = 4u,
    UnknownService = 5u,
    StartupFailed = 6u,
};

enum EFlags : u32 {
    Callable = 1u << 0u,
    Retryable = 1u << 1u,
};

enum class EErrorDomain : u32 {
    None = 0u,
    EditorAbi = 1u,
    EditorHost = 2u,
    Renderer = 3u,
};

enum class EErrorCode : i32 {
    None = 0,
    CapabilityNotAdvertised = 1001,
    InvalidHost = 1002,
    StartupPending = 1003,
    SceneFeatureInactive = 1004,
    UnknownService = 1005,
    StartupFailed = 1006,
};

#pragma pack(push, 4)
struct FDiagnostic {
    // Version-1 readable prefix (192 bytes).
    u32 version = kDiagnosticVersion;
    u32 struct_size = kDiagnosticSize;
    u32 service = 0u;
    u32 state = static_cast<u32>(EState::Disabled);
    u32 reason = static_cast<u32>(EReason::UnknownService);
    u32 flags = 0u;
    u64 host_generation = 0u;
    char message_utf8[kMessageBytes] = {};

    // Version-2 typed tail (64 bytes).
    u32 error_domain = static_cast<u32>(EErrorDomain::None);
    i32 error_code = static_cast<i32>(EErrorCode::None);
    u64 diagnostic_generation = 0u;
    char stable_code_utf8[kStableCodeBytes] = {};
};
#pragma pack(pop)

static_assert(sizeof(FDiagnostic) == kDiagnosticSize);
static_assert(alignof(FDiagnostic) == 4u);
static_assert(std::is_standard_layout_v<FDiagnostic>);
static_assert(std::is_trivially_copyable_v<FDiagnostic>);
static_assert(offsetof(FDiagnostic, host_generation) == 24u);
static_assert(offsetof(FDiagnostic, message_utf8) == 32u);
static_assert(offsetof(FDiagnostic, error_domain) == kLegacyDiagnosticSize);
static_assert(offsetof(FDiagnostic, diagnostic_generation) == 200u);
static_assert(offsetof(FDiagnostic, stable_code_utf8) == 208u);

} // namespace acs::editor_service_diagnostics
