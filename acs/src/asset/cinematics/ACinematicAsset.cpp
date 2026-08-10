// SPDX-License-Identifier: Apache-2.0
#include "asset/cinematics/ACinematicAsset.h"

#include <cmath>
#include <cstring>

namespace acs::asset {

namespace {

// 期間と時刻が有限で0以上かを確認します。
bool IsFiniteNonNegative(f32 value) noexcept
{
    return std::isfinite(value) && value >= 0.0f;
}

// 文字列内の埋め込みNULを検出します。
bool HasEmbeddedNul(const FString& value) noexcept
{
    return std::strlen(value.Data()) != value.Size();
}

// 文字列が厳密なUTF-8として読めるかを確認します。
bool IsUtf8(const FString& value) noexcept
{
    const u8* data = reinterpret_cast<const u8*>(value.Data());
    const usize size = value.Size();
    usize index = 0u;
    while (index < size) {
        const u8 lead = data[index];
        usize width = 1u;
        u32 codepoint = lead;
        if (lead >= 0xc2u && lead <= 0xdfu) {
            width = 2u;
            codepoint = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            width = 3u;
            codepoint = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            width = 4u;
            codepoint = lead & 0x07u;
        } else if (lead >= 0x80u)
            return false;
        if (width > size - index) return false;
        for (usize offset = 1u; offset < width; ++offset) {
            const u8 tail = data[index + offset];
            if ((tail & 0xc0u) != 0x80u) return false;
            codepoint = (codepoint << 6u) | (tail & 0x3fu);
        }
        if ((width == 2u && codepoint < 0x80u) || (width == 3u && codepoint < 0x800u) ||
            (width == 4u && codepoint < 0x10000u) || codepoint > 0x10ffffu ||
            (codepoint >= 0xd800u && codepoint <= 0xdfffu))
            return false;
        index += width;
    }
    return true;
}

// イベント種別ごとの値と時刻の不変条件を確認します。
bool ValidateEvent(const FCinematicEvent& event, f32 previous_time) noexcept
{
    if (!IsFiniteNonNegative(event.time_sec) || event.time_sec < previous_time) return false;
    switch (event.kind) {
    case ECinematicEventKind::Wait:
        return event.target_pos.x == 0.0f && event.target_pos.y == 0.0f && event.camera_zoom == 1.0f &&
               event.camera_duration == 0.0f && event.text.IsEmpty() && event.music_fade == 0.0f &&
               event.event_id == 0u;
    case ECinematicEventKind::MoveCamera:
        return std::isfinite(event.target_pos.x) && std::isfinite(event.target_pos.y) &&
               std::isfinite(event.camera_zoom) && event.camera_zoom > 0.0f && std::isfinite(event.camera_duration) &&
               event.camera_duration >= 0.0f && event.text.IsEmpty() && event.music_fade == 0.0f &&
               event.event_id == 0u;
    case ECinematicEventKind::Dialogue:
        return !HasEmbeddedNul(event.text) && IsUtf8(event.text) && event.text.Size() <= ACinematicAsset::kMaxTextBytes &&
               event.target_pos.x == 0.0f && event.target_pos.y == 0.0f && event.camera_zoom == 1.0f &&
               event.camera_duration == 0.0f && event.music_fade == 0.0f && event.event_id == 0u;
    case ECinematicEventKind::Music:
        return std::isfinite(event.music_fade) && event.music_fade >= 0.0f && !HasEmbeddedNul(event.text) &&
               IsUtf8(event.text) && event.text.Size() <= ACinematicAsset::kMaxTextBytes && event.target_pos.x == 0.0f &&
               event.target_pos.y == 0.0f && event.camera_zoom == 1.0f && event.camera_duration == 0.0f &&
               event.event_id == 0u;
    case ECinematicEventKind::FireEvent:
        return event.target_pos.x == 0.0f && event.target_pos.y == 0.0f && event.camera_zoom == 1.0f &&
               event.camera_duration == 0.0f && event.text.IsEmpty() && event.music_fade == 0.0f;
    }
    return false;
}

} // namespace

ACinematicAsset::ACinematicAsset(TArray<FCinematicEvent>&& events, f32 duration_sec) noexcept
    : m_Events(Move(events)), m_DurationSec(duration_sec)
{
}

bool ACinematicAsset::Validate(const TArray<FCinematicEvent>& events, f32 duration_sec) noexcept
{
    if (events.Num() > kMaxEvents) return false;
    if (!IsFiniteNonNegative(duration_sec)) return false;
    f32 previous_time = 0.0f;
    for (const FCinematicEvent& event : events) {
        if (!ValidateEvent(event, previous_time)) return false;
        previous_time = event.time_sec;
    }
    return events.Num() == 0 || duration_sec >= previous_time;
}

TResult<TSharedPtr<ACinematicAsset>> ACinematicAsset::TryCreate(TArray<FCinematicEvent>&& events,
                                                                f32 duration_sec) noexcept
{
    if (!Validate(events, duration_sec)) return ACS_ERR(Asset, 901, "ACinematicAsset: invalid values");
    TSharedPtr<ACinematicAsset> asset = MakeShared<ACinematicAsset>(Move(events), duration_sec);
    if (!asset) return ACS_ERR(Memory, 901, "ACinematicAsset: allocation failed");
    return TResult<TSharedPtr<ACinematicAsset>>(OkInit, Move(asset));
}

} // namespace acs::asset
