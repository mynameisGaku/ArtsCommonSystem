// SPDX-License-Identifier: Apache-2.0
#include "gameframework/cinematics/FCinematicDirectorBridge.h"

#include <cmath>

namespace acs::game {

namespace {

// assetの値がDirectorのkeyframeへ正確に対応するかを確認します。
bool IsRepresentable(const asset::FCinematicEvent& event) noexcept
{
    if (!std::isfinite(event.time_sec) || event.time_sec < 0.0f) return false;
    if (event.kind == asset::ECinematicEventKind::MoveCamera)
        return std::isfinite(event.target_pos.x) && std::isfinite(event.target_pos.y) &&
               std::isfinite(event.camera_zoom) && event.camera_zoom > 0.0f && std::isfinite(event.camera_duration) &&
               event.camera_duration >= 0.0f;
    if (event.kind == asset::ECinematicEventKind::Music)
        return std::isfinite(event.music_fade) && event.music_fade >= 0.0f;
    return true;
}

// assetのイベントを所有しないDirectorの値へ写します。
FTimelineKeyframe ToKeyframe(const asset::FCinematicEvent& event) noexcept
{
    FTimelineKeyframe keyframe{};
    keyframe.time_sec = event.time_sec;
    switch (event.kind) {
    case asset::ECinematicEventKind::Wait:
        keyframe.kind = ETimelineTrackKind::Wait;
        break;
    case asset::ECinematicEventKind::MoveCamera:
        keyframe.kind = ETimelineTrackKind::MoveCamera;
        keyframe.payload.camera.target_pos = event.target_pos;
        keyframe.payload.camera.zoom = event.camera_zoom;
        keyframe.payload.camera.duration = event.camera_duration;
        break;
    case asset::ECinematicEventKind::Dialogue:
        keyframe.kind = ETimelineTrackKind::ShowDialogue;
        keyframe.payload.dialogue.line_id = event.text.Data();
        break;
    case asset::ECinematicEventKind::Music:
        keyframe.kind = ETimelineTrackKind::PlayMusic;
        keyframe.payload.music.music_id = event.text.Data();
        keyframe.payload.music.fade = event.music_fade;
        break;
    case asset::ECinematicEventKind::FireEvent:
        keyframe.kind = ETimelineTrackKind::FireEvent;
        keyframe.payload.event.event_id = event.event_id;
        break;
    }
    return keyframe;
}

} // namespace

TResult<TArray<FTimelineKeyframe>> FCinematicDirectorBridge::BuildKeyframes(
    const asset::ACinematicAsset& asset) noexcept
{
    if (!std::isfinite(asset.DurationSec()) || asset.DurationSec() < 0.0f)
        return ACS_ERR(Asset, 910, "FCinematicDirectorBridge: invalid asset");
    // 全keyframeを一時配列へ確保し、成功時だけDirectorへ渡します。
    TArray<FTimelineKeyframe> out;
    if (!out.TryReserve(asset.EventCount() + 1u))
        return ACS_ERR(Memory, 910, "FCinematicDirectorBridge: allocation failed");
    for (const asset::FCinematicEvent& event : asset.Events()) {
        if (!IsRepresentable(event)) return ACS_ERR(Asset, 911, "FCinematicDirectorBridge: unsupported value");
        if (!out.TryAdd(ToKeyframe(event))) return ACS_ERR(Memory, 911, "FCinematicDirectorBridge: allocation failed");
    }
    if ((asset.EventCount() == 0u && asset.DurationSec() > 0.0f) ||
        (asset.EventCount() > 0u && asset.DurationSec() > asset.Events().Last().time_sec)) {
        // 明示durationまで再生できる終端Waitを追加します。
        FTimelineKeyframe tail{};
        tail.time_sec = asset.DurationSec();
        tail.kind = ETimelineTrackKind::Wait;
        if (!out.TryAdd(tail)) return ACS_ERR(Memory, 912, "FCinematicDirectorBridge: allocation failed");
    }
    return TResult<TArray<FTimelineKeyframe>>(OkInit, Move(out));
}

} // namespace acs::game
