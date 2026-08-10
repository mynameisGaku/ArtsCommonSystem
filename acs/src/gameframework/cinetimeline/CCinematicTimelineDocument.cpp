// SPDX-License-Identifier: Apache-2.0
#include "gameframework/cinetimeline/CCinematicTimelineDocument.h"

#include "gameframework/CinematicsDirector.h"

#include <cmath>

namespace acs::game::cinetimeline {

namespace {
// 実行時イベントへ変換する種別タグです。
constexpr u32 kFadeTag = 0x01000000u;
// 実行時の時間倍率タグです。
constexpr u32 kTimeScaleTag = 0x02000000u;
// 実行時の生成イベントタグです。
constexpr u32 kSpawnTag = 0x03000000u;
// 実行時の通知イベントタグです。
constexpr u32 kTriggerTag = 0x04000000u;
// イベント識別子へ渡す下位ビットです。
constexpr u32 kPayloadMask = 0x00FFFFFFu;

// 色を8ビットへ丸め、許容範囲へ収めます。
u32 QuantizeColor(f32 value) noexcept {
    const f32 clamped = value < 0.0f ? 0.0f : (value > 1.0f ? 1.0f : value);
    return static_cast<u32>(clamped * 255.0f + 0.5f);
}

// 色の3成分を実行時イベント値へ詰めます。
u32 EncodeColor(const FVec3& color) noexcept {
    return (QuantizeColor(color.x) << 16u) | (QuantizeColor(color.y) << 8u) | QuantizeColor(color.z);
}

// 時間倍率を固定小数点イベント値へ変換します。
u32 EncodeScale(f32 value) noexcept {
    const f32 clamped = value < 0.0f ? 0.0f : (value > 8.0f ? 8.0f : value);
    const u64 quantized = static_cast<u64>(clamped * 2097152.0f + 0.5f);
    return static_cast<u32>(quantized > kPayloadMask ? kPayloadMask : quantized);
}
}

CCinematicTimelineDocument::CCinematicTimelineDocument(IAllocator& allocator) noexcept
    : m_Keyframes(allocator) {
}

void CCinematicTimelineDocument::Reset() noexcept {
    m_Keyframes.Reset();
    m_DurationSec = kDefaultDurationSec;
}

bool CCinematicTimelineDocument::TrySetDurationSec(f32 duration_sec) noexcept {
    if (!std::isfinite(duration_sec)) return false;
    const f32 clamped_duration = duration_sec < kMinDurationSec ? kMinDurationSec : duration_sec;
    // 既存列を変更せず、新しい上限へ時刻を写します。
    TArray<FCinematicTimelineKeyframe> adjusted(*m_Keyframes.GetAllocator());
    if (!adjusted.TryReserve(m_Keyframes.Num())) return false;
    for (const FCinematicTimelineKeyframe& source : m_Keyframes) {
        // 縮小された長さを越える時刻だけ末尾へ合わせます。
        FCinematicTimelineKeyframe keyframe = source;
        if (keyframe.time_sec > clamped_duration) keyframe.time_sec = clamped_duration;
        if (!adjusted.TryAdd(keyframe)) return false;
    }
    for (usize i = 1; i < adjusted.Num(); ++i) {
        if (adjusted[i].time_sec < adjusted[i - 1u].time_sec) return false;
    }
    m_Keyframes = Move(adjusted);
    m_DurationSec = clamped_duration;
    return true;
}

bool CCinematicTimelineDocument::ValidateKeyframe(const FCinematicTimelineKeyframe& keyframe) const noexcept {
    if (!std::isfinite(keyframe.time_sec) || keyframe.time_sec < 0.0f || keyframe.time_sec > m_DurationSec) return false;
    if (static_cast<u32>(keyframe.kind) > static_cast<u32>(ETimelineKeyKind::TriggerCallback)) return false;
    switch (keyframe.kind) {
    case ETimelineKeyKind::CameraCut:
    case ETimelineKeyKind::SpawnEffect:
        {
            const bool finite_camera = std::isfinite(keyframe.camera_target.x) && std::isfinite(keyframe.camera_target.y) && std::isfinite(keyframe.camera_target.z);
            if (!finite_camera)
            {
                return false;
            }
        }
        break;
    case ETimelineKeyKind::FadeColor:
        {
            const bool finite_start = std::isfinite(keyframe.fade_start_color.x) && std::isfinite(keyframe.fade_start_color.y) && std::isfinite(keyframe.fade_start_color.z);
            const bool finite_end = std::isfinite(keyframe.fade_end_color.x) && std::isfinite(keyframe.fade_end_color.y) && std::isfinite(keyframe.fade_end_color.z);
            if (!finite_start || !finite_end)
            {
                return false;
            }
        }
        break;
    case ETimelineKeyKind::TimeScale:
        if (!std::isfinite(keyframe.time_scale) || keyframe.time_scale < 0.0f) return false;
        break;
    case ETimelineKeyKind::TriggerCallback:
        break;
    }
    return true;
}

bool CCinematicTimelineDocument::TryAdd(const FCinematicTimelineKeyframe& keyframe, u32* out_index) noexcept {
    if (!ValidateKeyframe(keyframe)) return false;
    usize insert_at = m_Keyframes.Num();
    for (usize i = 0; i < m_Keyframes.Num(); ++i) {
        if (keyframe.time_sec < m_Keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }
    if (!m_Keyframes.TryAdd(keyframe)) return false;
    for (usize i = m_Keyframes.Num() - 1u; i > insert_at; --i) m_Keyframes[i] = m_Keyframes[i - 1u];
    m_Keyframes[insert_at] = keyframe;
    if (out_index != nullptr) *out_index = static_cast<u32>(insert_at);
    return true;
}

bool CCinematicTimelineDocument::TryRemove(u32 index) noexcept {
    if (index >= KeyframeCount()) return false;
    m_Keyframes.RemoveAt(index);
    return true;
}

bool CCinematicTimelineDocument::TryReplace(u32 index, const FCinematicTimelineKeyframe& keyframe, u32* out_index) noexcept {
    if (index >= KeyframeCount() || !ValidateKeyframe(keyframe)) return false;
    // 並べ替えに失敗しても現在列を保持する作業列です。
    TArray<FCinematicTimelineKeyframe> replacement(*m_Keyframes.GetAllocator());
    if (!replacement.TryReserve(m_Keyframes.Num())) return false;
    if (keyframe.time_sec == m_Keyframes[index].time_sec) {
        for (usize i = 0; i < m_Keyframes.Num(); ++i) {
            if (!replacement.TryAdd(i == index ? keyframe : m_Keyframes[i])) return false;
        }
        m_Keyframes = Move(replacement);
        if (out_index != nullptr) *out_index = index;
        return true;
    }
    for (usize i = 0; i < m_Keyframes.Num(); ++i) {
        if (i != index && !replacement.TryAdd(m_Keyframes[i])) return false;
    }
    usize new_index = replacement.Num();
    for (usize i = 0; i < replacement.Num(); ++i) if (keyframe.time_sec < replacement[i].time_sec) { new_index = i; break; }
    if (!replacement.TryAdd(keyframe)) return false;
    for (usize i = replacement.Num() - 1u; i > new_index; --i) replacement[i] = replacement[i - 1u];
    replacement[new_index] = keyframe;
    m_Keyframes = Move(replacement);
    if (out_index != nullptr) *out_index = static_cast<u32>(new_index);
    return true;
}

bool CCinematicTimelineDocument::TryBakeTo(CCinematicsDirector& director) noexcept {
    // Director所有アロケータで実行時列を先に確保します。
    TArray<FTimelineKeyframe> staged(*director.m_Keyframes.GetAllocator());
    if (!staged.TryReserve(m_Keyframes.Num())) return false;
    for (const FCinematicTimelineKeyframe& source : m_Keyframes) {
        if (!ValidateKeyframe(source)) return false;
        // 各編集種別を既存Directorの実行時表現へ写します。
        FTimelineKeyframe runtime{};
        runtime.time_sec = source.time_sec;
        switch (source.kind) {
        case ETimelineKeyKind::CameraCut:
            runtime.kind = ETimelineTrackKind::MoveCamera;
            runtime.payload.camera.target_pos = FVec2{ source.camera_target.x, source.camera_target.y };
            runtime.payload.camera.zoom = 1.0f;
            runtime.payload.camera.duration = 0.0f;
            break;
        case ETimelineKeyKind::FadeColor:
            runtime.kind = ETimelineTrackKind::FireEvent;
            runtime.payload.event.event_id = kFadeTag | EncodeColor(source.fade_end_color);
            break;
        case ETimelineKeyKind::TimeScale:
            runtime.kind = ETimelineTrackKind::FireEvent;
            runtime.payload.event.event_id = kTimeScaleTag | EncodeScale(source.time_scale);
            break;
        case ETimelineKeyKind::SpawnEffect:
            runtime.kind = ETimelineTrackKind::FireEvent;
            runtime.payload.event.event_id = kSpawnTag | (source.event_id & kPayloadMask);
            break;
        case ETimelineKeyKind::TriggerCallback:
            runtime.kind = ETimelineTrackKind::FireEvent;
            runtime.payload.event.event_id = kTriggerTag | (source.event_id & kPayloadMask);
            break;
        }
        if (!staged.TryAdd(runtime)) return false;
    }
    director.TryResetKeyframes(Move(staged));
    return true;
}

}
