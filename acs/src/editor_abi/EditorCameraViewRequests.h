// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <cstring>

namespace acs::editor_camera_view {

inline constexpr std::uint32_t kSnapshotVersion = 1u;
inline constexpr std::uint32_t kMaximumRequests = 8u;
inline constexpr std::uint32_t kMaximumDimension = 8192u;
inline constexpr std::uint64_t kMaximumPixels = 33'554'432ull;
inline constexpr std::uint32_t kStableCameraIdBytes = 64u;

enum class ETargetKind : std::uint32_t {
    None = 0u,
    SharedSwapchain = 1u,
    DedicatedOffscreen = 2u,
};

enum ESnapshotFlags : std::uint32_t {
    SnapshotActive = 1u << 0u,
    SnapshotPresenter = 1u << 1u,
    SnapshotCameraStale = 1u << 2u,
    SnapshotTargetRecreatePending = 1u << 3u,
    SnapshotHistoryResetPending = 1u << 4u,
};

#pragma pack(push, 4)
/**
 * Latest metadata for one bounded Camera View render request.
 *
 * CameraViewRequestsV1 deliberately does not promise a dedicated render
 * target. `target_kind == SharedSwapchain` means this request currently owns
 * the editor's one presentation surface. A future async offscreen capability
 * may publish DedicatedOffscreen without changing request identity.
 */
struct FSnapshot {
    std::uint32_t version = kSnapshotVersion;
    std::uint32_t struct_size = sizeof(FSnapshot);
    std::uint64_t request_id = 0u;
    std::uint64_t latest_frame_serial = 0u;
    std::int32_t camera_node_id = -1;
    std::uint32_t width = 0u;
    std::uint32_t height = 0u;
    std::uint32_t presented_width = 0u;
    std::uint32_t presented_height = 0u;
    std::uint32_t target_generation = 0u;
    std::uint32_t history_generation = 0u;
    std::uint32_t flags = 0u;
    std::uint32_t target_kind =
        static_cast<std::uint32_t>(ETargetKind::None);
};
#pragma pack(pop)

static_assert(sizeof(FSnapshot) == 60u);

[[nodiscard]] constexpr bool IsValidExtent(
    std::uint32_t width, std::uint32_t height) noexcept {
    return width > 0u && height > 0u &&
           width <= kMaximumDimension &&
           height <= kMaximumDimension &&
           static_cast<std::uint64_t>(width) *
                   static_cast<std::uint64_t>(height) <=
               kMaximumPixels;
}

/**
 * Fixed-capacity, allocation-free request registry owned by one FEditorHost.
 *
 * IDs contain a slot generation, so a stale managed lease cannot mutate a
 * newly-created request after close/reopen (ABA). Exactly one request may bind
 * the existing swapchain. Other requests retain independent camera, extent,
 * target-generation and history-generation metadata for a later bounded
 * offscreen scheduler.
 */
class FRegistry {
public:
    [[nodiscard]] bool Create(
        std::int32_t camera_node_id,
        const char* stable_camera_id,
        std::uint32_t width,
        std::uint32_t height,
        std::uint64_t& output_request_id) noexcept {
        output_request_id = 0u;
        char stable_id_copy[kStableCameraIdBytes + 1u]{};
        if (camera_node_id < 0 ||
            !CopyStableCameraId(
                stable_id_copy, stable_camera_id) ||
            !IsValidExtent(width, height)) {
            return false;
        }

        for (std::uint32_t slot = 0u; slot < kMaximumRequests; ++slot) {
            FRecord& record = m_Records[slot];
            if (record.active) continue;
            record.generation = NextGeneration(record.generation);
            record.active = true;
            record.camera_stale = false;
            record.camera_node_id = camera_node_id;
            std::memcpy(
                record.stable_camera_id,
                stable_id_copy,
                sizeof(stable_id_copy));
            record.width = width;
            record.height = height;
            record.target_generation = 1u;
            record.history_generation = 1u;
            record.latest_frame_serial = 0u;
            record.presented_width = 0u;
            record.presented_height = 0u;
            record.target_recreate_pending = true;
            record.history_reset_pending = true;
            output_request_id = EncodeId(slot, record.generation);
            return true;
        }
        return false;
    }

    [[nodiscard]] bool Update(
        std::uint64_t request_id,
        std::int32_t camera_node_id,
        const char* stable_camera_id,
        std::uint32_t width,
        std::uint32_t height) noexcept {
        FRecord* record = Find(request_id);
        char stable_id_copy[kStableCameraIdBytes + 1u]{};
        if (record == nullptr || camera_node_id < 0 ||
            !CopyStableCameraId(
                stable_id_copy, stable_camera_id) ||
            !IsValidExtent(width, height)) {
            return false;
        }

        const bool camera_changed =
            record->camera_stale ||
            record->camera_node_id != camera_node_id ||
            std::strcmp(
                record->stable_camera_id,
                stable_id_copy) != 0;
        const bool extent_changed =
            record->width != width || record->height != height;
        if (camera_changed) {
            record->camera_node_id = camera_node_id;
            std::memcpy(
                record->stable_camera_id,
                stable_id_copy,
                sizeof(stable_id_copy));
            record->camera_stale = false;
            record->history_generation =
                NextGeneration(record->history_generation);
            record->history_reset_pending = true;
            record->latest_frame_serial = 0u;
            record->presented_width = 0u;
            record->presented_height = 0u;
        }
        if (extent_changed) {
            record->width = width;
            record->height = height;
            record->target_generation =
                NextGeneration(record->target_generation);
            record->history_generation =
                NextGeneration(record->history_generation);
            record->target_recreate_pending = true;
            record->history_reset_pending = true;
            record->latest_frame_serial = 0u;
            record->presented_width = 0u;
            record->presented_height = 0u;
        }
        return true;
    }

    [[nodiscard]] bool Destroy(std::uint64_t request_id) noexcept {
        FRecord* record = Find(request_id);
        if (record == nullptr) return false;
        if (m_PresenterRequestId == request_id)
            m_PresenterRequestId = 0u;
        record->active = false;
        record->camera_stale = false;
        record->camera_node_id = -1;
        record->stable_camera_id[0] = '\0';
        record->width = 0u;
        record->height = 0u;
        record->target_recreate_pending = false;
        record->history_reset_pending = false;
        record->latest_frame_serial = 0u;
        record->presented_width = 0u;
        record->presented_height = 0u;
        return true;
    }

    [[nodiscard]] bool BindPresenter(
        std::uint64_t request_id) noexcept {
        FRecord* next = Find(request_id);
        if (next == nullptr || next->camera_stale) return false;
        if (m_PresenterRequestId == request_id) return true;
        // Presenter transfer is a two-phase managed transaction: first return
        // the HWND and explicitly unbind the old request, then bind the new
        // owner. Silently stealing here could leave native camera identity and
        // Win32 surface ownership describing different windows.
        if (m_PresenterRequestId != 0u) return false;
        m_PresenterRequestId = request_id;
        next->target_generation =
            NextGeneration(next->target_generation);
        next->history_generation =
            NextGeneration(next->history_generation);
        next->target_recreate_pending = true;
        next->history_reset_pending = true;
        next->latest_frame_serial = 0u;
        next->presented_width = 0u;
        next->presented_height = 0u;
        return true;
    }

    [[nodiscard]] bool UnbindPresenter(
        std::uint64_t request_id) noexcept {
        if (request_id == 0u ||
            m_PresenterRequestId != request_id ||
            Find(request_id) == nullptr) {
            return false;
        }
        if (FRecord* record = Find(request_id)) {
            record->target_generation =
                NextGeneration(record->target_generation);
            record->history_generation =
                NextGeneration(record->history_generation);
            record->target_recreate_pending = true;
            record->history_reset_pending = true;
            record->latest_frame_serial = 0u;
            record->presented_width = 0u;
            record->presented_height = 0u;
        }
        m_PresenterRequestId = 0u;
        return true;
    }

    void MarkPresenterCameraStale() noexcept {
        MarkCameraStale(m_PresenterRequestId);
    }

    void MarkAllCamerasStale() noexcept {
        m_PresenterRequestId = 0u;
        for (FRecord& record : m_Records) {
            if (!record.active) continue;
            record.camera_stale = true;
            record.history_generation =
                NextGeneration(record.history_generation);
            record.history_reset_pending = true;
            record.latest_frame_serial = 0u;
            record.presented_width = 0u;
            record.presented_height = 0u;
        }
    }

    void MarkCameraStale(std::uint64_t request_id) noexcept {
        FRecord* record = Find(request_id);
        if (record == nullptr) {
            if (m_PresenterRequestId == request_id)
                m_PresenterRequestId = 0u;
            return;
        }
        record->camera_stale = true;
        record->history_generation =
            NextGeneration(record->history_generation);
        record->history_reset_pending = true;
        record->latest_frame_serial = 0u;
        record->presented_width = 0u;
        record->presented_height = 0u;
        if (m_PresenterRequestId == request_id)
            m_PresenterRequestId = 0u;
    }

    void MarkPresenterRendered(
        std::uint64_t frame_serial,
        std::uint32_t presented_width,
        std::uint32_t presented_height) noexcept {
        FRecord* record = Find(m_PresenterRequestId);
        if (record == nullptr || record->camera_stale) return;
        record->latest_frame_serial = frame_serial;
        record->presented_width = presented_width;
        record->presented_height = presented_height;
        const bool requested_extent_presented =
            record->width == presented_width &&
            record->height == presented_height;
        record->target_recreate_pending =
            !requested_extent_presented;
        record->history_reset_pending =
            !requested_extent_presented;
    }

    [[nodiscard]] bool Snapshot(
        std::uint64_t request_id,
        FSnapshot& output) const noexcept {
        const FRecord* record = Find(request_id);
        output = FSnapshot{};
        if (record == nullptr) return false;
        output.request_id = request_id;
        output.latest_frame_serial = record->latest_frame_serial;
        output.camera_node_id = record->camera_node_id;
        output.width = record->width;
        output.height = record->height;
        output.presented_width = record->presented_width;
        output.presented_height = record->presented_height;
        output.target_generation = record->target_generation;
        output.history_generation = record->history_generation;
        output.flags = SnapshotActive;
        if (request_id == m_PresenterRequestId) {
            output.flags |= SnapshotPresenter;
            output.target_kind =
                static_cast<std::uint32_t>(
                    ETargetKind::SharedSwapchain);
        }
        if (record->camera_stale)
            output.flags |= SnapshotCameraStale;
        if (record->target_recreate_pending)
            output.flags |= SnapshotTargetRecreatePending;
        if (record->history_reset_pending)
            output.flags |= SnapshotHistoryResetPending;
        return true;
    }

    [[nodiscard]] bool PresenterIdentity(
        std::uint64_t& output_request_id,
        std::int32_t& output_camera_node_id,
        const char*& output_stable_camera_id,
        std::uint32_t& output_history_generation) const noexcept {
        output_request_id = 0u;
        output_camera_node_id = -1;
        output_stable_camera_id = nullptr;
        output_history_generation = 0u;
        const FRecord* record = Find(m_PresenterRequestId);
        if (record == nullptr || record->camera_stale) return false;
        output_request_id = m_PresenterRequestId;
        output_camera_node_id = record->camera_node_id;
        output_stable_camera_id = record->stable_camera_id;
        output_history_generation = record->history_generation;
        return true;
    }

    [[nodiscard]] bool RequestIdentity(
        std::uint64_t request_id,
        std::int32_t& output_camera_node_id,
        const char*& output_stable_camera_id) const noexcept {
        output_camera_node_id = -1;
        output_stable_camera_id = nullptr;
        const FRecord* record = Find(request_id);
        if (record == nullptr || record->camera_stale) return false;
        output_camera_node_id = record->camera_node_id;
        output_stable_camera_id = record->stable_camera_id;
        return true;
    }

    void Clear() noexcept {
        m_PresenterRequestId = 0u;
        for (FRecord& record : m_Records) {
            record.active = false;
            record.camera_stale = false;
            record.camera_node_id = -1;
            record.stable_camera_id[0] = '\0';
            record.width = 0u;
            record.height = 0u;
            record.target_recreate_pending = false;
            record.history_reset_pending = false;
            record.latest_frame_serial = 0u;
            record.presented_width = 0u;
            record.presented_height = 0u;
        }
    }

    [[nodiscard]] std::uint64_t PresenterRequestId() const noexcept {
        return Find(m_PresenterRequestId) != nullptr
            ? m_PresenterRequestId : 0u;
    }

private:
    struct FRecord {
        std::uint32_t generation = 0u;
        bool active = false;
        bool camera_stale = false;
        std::int32_t camera_node_id = -1;
        char stable_camera_id[kStableCameraIdBytes + 1u]{};
        std::uint32_t width = 0u;
        std::uint32_t height = 0u;
        std::uint32_t target_generation = 0u;
        std::uint32_t history_generation = 0u;
        std::uint64_t latest_frame_serial = 0u;
        std::uint32_t presented_width = 0u;
        std::uint32_t presented_height = 0u;
        bool target_recreate_pending = false;
        bool history_reset_pending = false;
    };

    [[nodiscard]] static constexpr std::uint32_t NextGeneration(
        std::uint32_t generation) noexcept {
        ++generation;
        return generation == 0u ? 1u : generation;
    }

    [[nodiscard]] static constexpr std::uint64_t EncodeId(
        std::uint32_t slot, std::uint32_t generation) noexcept {
        return (static_cast<std::uint64_t>(generation) << 32u) |
               static_cast<std::uint64_t>(slot + 1u);
    }

    [[nodiscard]] static bool DecodeId(
        std::uint64_t request_id,
        std::uint32_t& output_slot,
        std::uint32_t& output_generation) noexcept {
        const std::uint32_t slot_code =
            static_cast<std::uint32_t>(request_id & 0xffffffffull);
        output_generation =
            static_cast<std::uint32_t>(request_id >> 32u);
        if (slot_code == 0u ||
            slot_code > kMaximumRequests ||
            output_generation == 0u) {
            output_slot = 0u;
            output_generation = 0u;
            return false;
        }
        output_slot = slot_code - 1u;
        return true;
    }

    [[nodiscard]] FRecord* Find(
        std::uint64_t request_id) noexcept {
        std::uint32_t slot = 0u;
        std::uint32_t generation = 0u;
        if (!DecodeId(request_id, slot, generation)) return nullptr;
        FRecord& record = m_Records[slot];
        return record.active && record.generation == generation
            ? &record : nullptr;
    }

    [[nodiscard]] const FRecord* Find(
        std::uint64_t request_id) const noexcept {
        std::uint32_t slot = 0u;
        std::uint32_t generation = 0u;
        if (!DecodeId(request_id, slot, generation)) return nullptr;
        const FRecord& record = m_Records[slot];
        return record.active && record.generation == generation
            ? &record : nullptr;
    }

    /**
     * Validate and optionally copy a canonical camera ID without unbounded
     * strlen. Passing destination=nullptr performs validation only.
     */
    [[nodiscard]] static bool CopyStableCameraId(
        char* destination, const char* source) noexcept {
        if (source == nullptr || source[0] == '\0') return false;
        std::uint32_t length = 0u;
        for (; length <= kStableCameraIdBytes; ++length) {
            const char value = source[length];
            if (value == '\0') break;
            const bool alpha =
                (value >= 'A' && value <= 'Z') ||
                (value >= 'a' && value <= 'z');
            const bool digit = value >= '0' && value <= '9';
            if (!alpha && !digit &&
                (length == 0u ||
                 (value != '_' && value != '.' && value != '-'))) {
                return false;
            }
        }
        if (length == 0u || length > kStableCameraIdBytes)
            return false;
        if (destination != nullptr) {
            std::memcpy(destination, source, length);
            destination[length] = '\0';
        }
        return true;
    }

    FRecord m_Records[kMaximumRequests]{};
    std::uint64_t m_PresenterRequestId = 0u;
};

} // namespace acs::editor_camera_view
