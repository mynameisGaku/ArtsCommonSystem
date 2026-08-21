// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

#include <cstddef>
#include <type_traits>

namespace acs::editor_cloud_workload {

/**
 * ボリューム雲が投入した処理量を公開する、追加可能な任意診断契約。
 * 一般プロファイラーとは版を分け、古い利用側へ整合する接頭部を維持する。
 */
inline constexpr u32 kSnapshotVersionV1 = 1u;
inline constexpr u32 kSnapshotSizeV1 = 168u;
inline constexpr u32 kSnapshotVersionV2 = 2u;
inline constexpr u32 kSnapshotSizeV2 = 200u;

// 既存の C++ 利用側が参照する名前は、互換用の v1 を指し続ける。
inline constexpr u32 kSnapshotVersion = kSnapshotVersionV1;
inline constexpr u32 kSnapshotSize = kSnapshotSizeV1;

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
    u32 version = kSnapshotVersionV1;
    u32 struct_size = kSnapshotSizeV1;
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

/**
 * 地表と物体へ投影するワールド雲影の処理量を追加した v2 契約。
 * base は v1 と同じバイト配置であり、旧利用側へ安全に変換できる。
 */
struct FSnapshotV2 {
    /** 既定構築だけで v2 問い合わせに使える要求ヘッダーを設定する。 */
    FSnapshotV2() noexcept
    {
        base.version = kSnapshotVersionV2;
        base.struct_size = kSnapshotSizeV2;
    }

    FSnapshot base{};
    u32 world_shadow_dispatches = 0u;
    u32 reserved1 = 0u;
    u64 world_shadow_logical_invocations = 0u;
    u64 world_shadow_launched_threads = 0u;
    u64 maximum_world_shadow_samples = 0u;
};
#pragma pack(pop)

static_assert(sizeof(FSnapshot) == kSnapshotSizeV1, "Cloud workload ABI must match EditorCloudWorkloadSnapshot");
static_assert(offsetof(FSnapshot, profiler_frame_index) == 16u);
static_assert(offsetof(FSnapshot, trace_width) == 32u);
static_assert(offsetof(FSnapshot, trace_logical_invocations) == 72u);
static_assert(offsetof(FSnapshot, maximum_light_samples) == 160u);
static_assert(sizeof(FSnapshotV2) == kSnapshotSizeV2, "Cloud workload v2 ABI must match EditorCloudWorkloadSnapshot");
static_assert(offsetof(FSnapshotV2, base) == 0u);
static_assert(offsetof(FSnapshotV2, world_shadow_dispatches) == 168u);
static_assert(offsetof(FSnapshotV2, world_shadow_logical_invocations) == 176u);
static_assert(offsetof(FSnapshotV2, maximum_world_shadow_samples) == 192u);
static_assert(std::is_trivially_copyable_v<FSnapshotV2>);

} // namespace acs::editor_cloud_workload
