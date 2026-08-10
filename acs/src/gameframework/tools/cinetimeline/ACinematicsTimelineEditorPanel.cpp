// SPDX-License-Identifier: Apache-2.0
#include "gameframework/tools/cinetimeline/ACinematicsTimelineEditorPanel.h"

#include "gameframework/CinematicsDirector.h"

#include <imgui.h>

#include <cmath>
#include <cstdio>

namespace acs::game::cinetimeline {

namespace {
// 表示値を指定範囲へ収め、入力範囲外の値を安全に扱います。
f32 ClampF(f32 value, f32 low, f32 high) noexcept {
    return value < low ? low : (value > high ? high : value);
}

// 編集種別の表示名を返し、未定義値はUnknownとして扱います。
const char* KindName(ETimelineKeyKind kind) noexcept {
    switch (kind) {
    case ETimelineKeyKind::CameraCut: return "CameraCut";
    case ETimelineKeyKind::FadeColor: return "FadeColor";
    case ETimelineKeyKind::TimeScale: return "TimeScale";
    case ETimelineKeyKind::SpawnEffect: return "SpawnEffect";
    case ETimelineKeyKind::TriggerCallback: return "TriggerCallback";
    }
    return "Unknown";
}

// 編集種別に対応するマーカー色を返します。
ImU32 KindColor(ETimelineKeyKind kind) noexcept {
    switch (kind) {
    case ETimelineKeyKind::CameraCut: return IM_COL32(80, 200, 255, 255);
    case ETimelineKeyKind::FadeColor: return IM_COL32(255, 220, 100, 255);
    case ETimelineKeyKind::TimeScale: return IM_COL32(220, 100, 220, 255);
    case ETimelineKeyKind::SpawnEffect: return IM_COL32(100, 220, 120, 255);
    case ETimelineKeyKind::TriggerCallback: return IM_COL32(255, 150, 80, 255);
    }
    return IM_COL32(120, 120, 120, 255);
}

// 時刻をキャンバス座標へ変換し、継続時間外の値を端点へ収めます。
f32 TimeToX(f32 time_sec, f32 duration_sec, f32 origin_x, f32 width) noexcept {
    if (duration_sec <= 0.0f || width <= 0.0f) return origin_x;
    return origin_x + ClampF(time_sec / duration_sec, 0.0f, 1.0f) * width;
}

// キャンバス座標を有効な時刻へ変換し、左右の端点でクランプします。
f32 XToTime(f32 x, f32 duration_sec, f32 origin_x, f32 width) noexcept {
    if (width <= 0.0f) return 0.0f;
    return ClampF((x - origin_x) / width, 0.0f, 1.0f) * duration_sec;
}
}

TSpan<const FEditorKeyframe> ACinematicsTimelineEditorPanel::EditorKeyframes() const noexcept {
    return m_Document.Keyframes();
}

void ACinematicsTimelineEditorPanel::Init() noexcept {
    m_Director = nullptr;
    m_Document.Reset();
    m_SelectedIdx = kNoKeySelected;
    m_CurrentTime = 0.0f;
    m_Playing = false;
    m_bDraggingMarker = false;
    m_DragIdx = kNoKeySelected;
    m_AddKind = ETimelineKeyKind::CameraCut;
}

void ACinematicsTimelineEditorPanel::Shutdown() noexcept {
    m_Director = nullptr;
    m_Document.Reset();
    m_SelectedIdx = kNoKeySelected;
    m_CurrentTime = 0.0f;
    m_Playing = false;
    m_bDraggingMarker = false;
    m_DragIdx = kNoKeySelected;
}

void ACinematicsTimelineEditorPanel::SetCinematicsDirector(CCinematicsDirector* director) noexcept {
    m_Director = director;
    m_SelectedIdx = kNoKeySelected;
    m_CurrentTime = 0.0f;
    m_Playing = false;
    (void)BakeToDirector();
}

CCinematicsDirector* ACinematicsTimelineEditorPanel::CurrentDirector() const noexcept {
    return m_Director;
}

void ACinematicsTimelineEditorPanel::Play() noexcept {
    if (m_Director != nullptr && !BakeToDirector()) return;
    m_CurrentTime = 0.0f;
    m_Playing = true;
    if (m_Director != nullptr) m_Director->Play();
}

void ACinematicsTimelineEditorPanel::Pause() noexcept {
    m_Playing = false;
    if (m_Director != nullptr) m_Director->Pause();
}

void ACinematicsTimelineEditorPanel::Stop() noexcept {
    m_Playing = false;
    m_CurrentTime = 0.0f;
    if (m_Director != nullptr) m_Director->Stop();
}

void ACinematicsTimelineEditorPanel::Step(f32 dt) noexcept {
    if (!m_Playing) return;
    StepOnce(dt);
}

void ACinematicsTimelineEditorPanel::StepOnce(f32 dt) noexcept {
    if (!std::isfinite(dt) || dt <= 0.0f) return;
    if (m_Director != nullptr) {
        const bool director_was_playing = m_Director->IsPlaying();
        const f32 next_time = m_Director->CurrentTime() + dt;
        if (!std::isfinite(next_time)) return;
        if (!director_was_playing) m_Director->Play();
        m_Director->Tick(dt);
        if (!director_was_playing) m_Director->Pause();
        m_CurrentTime = m_Director->CurrentTime();
    } else {
        const f32 next_time = m_CurrentTime + dt;
        if (!std::isfinite(next_time)) return;
        m_CurrentTime = next_time;
    }
    if (m_CurrentTime >= DurationSec()) {
        m_CurrentTime = DurationSec();
        if (m_Director != nullptr) m_Director->Pause();
        m_Playing = false;
    }
}

bool ACinematicsTimelineEditorPanel::IsPlaying() const noexcept {
    return m_Playing;
}

f32 ACinematicsTimelineEditorPanel::CurrentTimeSec() const noexcept {
    return m_CurrentTime;
}

void ACinematicsTimelineEditorPanel::SetCurrentTimeSec(f32 time_sec) noexcept {
    if (!std::isfinite(time_sec)) return;
    m_CurrentTime = ClampF(time_sec, 0.0f, DurationSec());
}

f32 ACinematicsTimelineEditorPanel::DurationSec() const noexcept {
    return m_Document.DurationSec();
}

void ACinematicsTimelineEditorPanel::SetDurationSec(f32 duration_sec) noexcept {
    if (!m_Document.TrySetDurationSec(duration_sec)) return;
    m_CurrentTime = ClampF(m_CurrentTime, 0.0f, DurationSec());
    (void)BakeToDirector();
}

i32 ACinematicsTimelineEditorPanel::SelectedKeyframeIndex() const noexcept {
    return m_SelectedIdx;
}

void ACinematicsTimelineEditorPanel::SelectKeyframe(i32 index) noexcept {
    m_SelectedIdx = index >= 0 && static_cast<u32>(index) < m_Document.KeyframeCount() ? index : kNoKeySelected;
}

void ACinematicsTimelineEditorPanel::AddKeyframe(ETimelineKeyKind kind, f32 time_sec) noexcept {
    if (!std::isfinite(time_sec)) return;
    FEditorKeyframe keyframe;
    keyframe.kind = kind;
    keyframe.time_sec = ClampF(time_sec, 0.0f, DurationSec());
    u32 index = 0u;
    if (!m_Document.TryAdd(keyframe, &index)) return;
    m_SelectedIdx = static_cast<i32>(index);
    (void)BakeToDirector();
}

void ACinematicsTimelineEditorPanel::RemoveSelectedKeyframe() noexcept {
    if (m_SelectedIdx < 0 || !m_Document.TryRemove(static_cast<u32>(m_SelectedIdx))) return;
    m_SelectedIdx = kNoKeySelected;
    (void)BakeToDirector();
}

bool ACinematicsTimelineEditorPanel::BakeToDirector() noexcept {
    return m_Director == nullptr || m_Document.TryBakeTo(*m_Director);
}

void ACinematicsTimelineEditorPanel::DrawUI() noexcept {
    if (!IsVisible()) return;
    if (!ImGui::Begin(Title(), &m_Visible))
    {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Play")) Play();
    ImGui::SameLine();
    if (ImGui::Button("Pause")) Pause();
    ImGui::SameLine();
    if (ImGui::Button("Stop")) Stop();
    ImGui::SameLine();
    if (ImGui::Button("Step+0.1s")) StepOnce(0.1f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220.0f);
    float cursor = m_CurrentTime;
    if (ImGui::SliderFloat("##time_slider", &cursor, 0.0f, DurationSec(), "t=%.2fs")) SetCurrentTimeSec(cursor);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    float duration = DurationSec();
    if (ImGui::DragFloat("##duration", &duration, 0.1f, kMinDurationSec, 600.0f, "dur=%.1fs"))
    {
        SetDurationSec(duration);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    if (ImGui::BeginCombo("##add_kind", KindName(m_AddKind)))
    {
        for (u32 value = 0u; value < kTrackCount; ++value)
        {
            const ETimelineKeyKind kind = static_cast<ETimelineKeyKind>(value);
            if (ImGui::Selectable(KindName(kind), kind == m_AddKind))
            {
                m_AddKind = kind;
            }
            if (kind == m_AddKind)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("+Add")) AddKeyframe(m_AddKind, m_CurrentTime);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_SelectedIdx < 0);
    if (ImGui::Button("-Del")) RemoveSelectedKeyframe();
    ImGui::EndDisabled();
    ImGui::Separator();

    // 右側インスペクターを残した描画幅です。
    const f32 inspector_width = 260.0f;
    const f32 available_width = ImGui::GetContentRegionAvail().x;
    const f32 left_pane_width = available_width > inspector_width + 50.0f ? available_width - inspector_width - 8.0f : available_width;
    ImGui::BeginChild("##timeline_left", ImVec2(left_pane_width, 0.0f), false);
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const f32 canvas_width = ImGui::GetContentRegionAvail().x - 4.0f;
    const f32 ruler_height = 22.0f;
    const f32 track_gap = 2.0f;
    const f32 canvas_padding = 4.0f;
    const f32 track_height = kTrackRowHeightPx * static_cast<f32>(kTrackCount);
    const f32 canvas_height = ruler_height + track_gap + track_height + canvas_padding;
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImVec2 canvas_end(origin.x + canvas_width, origin.y + canvas_height);
    draw->AddRectFilled(origin, canvas_end, IM_COL32(30, 30, 35, 255));
    draw->AddRect(origin, canvas_end, IM_COL32(80, 80, 90, 255));
    draw->AddRectFilled(origin, ImVec2(canvas_end.x, origin.y + ruler_height), IM_COL32(40, 40, 48, 255));
    // 継続時間が長くても目盛り数を安全な上限に制限します。
    const i32 tick_count = static_cast<i32>(ClampF(DurationSec(), 0.0f, 600.0f) + 0.5f);
    for (i32 tick = 0; tick <= tick_count; ++tick)
    {
        const f32 x = TimeToX(static_cast<f32>(tick), DurationSec(), origin.x, canvas_width);
        draw->AddLine(ImVec2(x, origin.y), ImVec2(x, origin.y + ruler_height), IM_COL32(160, 160, 160, 220));
        char label[16];
        std::snprintf(label, sizeof(label), "%ds", tick);
        draw->AddText(ImVec2(x + 2.0f, origin.y + 2.0f), IM_COL32(200, 200, 200, 255), label);
    }
    for (u32 track = 0u; track < kTrackCount; ++track)
    {
        const f32 y0 = origin.y + ruler_height + track_gap + kTrackRowHeightPx * static_cast<f32>(track);
        const ImU32 stripe = (track & 1u) == 0u ? IM_COL32(32, 32, 38, 255) : IM_COL32(38, 38, 44, 255);
        draw->AddRectFilled(ImVec2(origin.x, y0), ImVec2(canvas_end.x, y0 + kTrackRowHeightPx), stripe);
        draw->AddText(ImVec2(origin.x + 4.0f, y0 + 6.0f), IM_COL32(170, 170, 170, 255), KindName(static_cast<ETimelineKeyKind>(track)));
    }
    draw->AddLine(ImVec2(origin.x, origin.y + ruler_height + track_gap + track_height), ImVec2(canvas_end.x, origin.y + ruler_height + track_gap + track_height), IM_COL32(80, 80, 90, 255));
    ImGui::InvisibleButton("##timeline_canvas", ImVec2(canvas_width, canvas_height), ImGuiButtonFlags_MouseButtonLeft);
    // マウス入力はキャンバス外へ出てもドラッグ中は受け取ります。
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
    {
        const f32 tracks_origin_y = origin.y + ruler_height + track_gap;
        const f32 local_y = io.MousePos.y - tracks_origin_y;
        const u32 track = local_y >= 0.0f ? static_cast<u32>(local_y / kTrackRowHeightPx) : kTrackCount;
        i32 hit = kNoKeySelected;
        const TSpan<const FEditorKeyframe> keyframes = EditorKeyframes();
        for (usize i = 0; i < keyframes.Num(); ++i)
        {
            const FEditorKeyframe& keyframe = keyframes[i];
            const f32 x = TimeToX(keyframe.time_sec, DurationSec(), origin.x, canvas_width);
            const f32 marker_y = tracks_origin_y + kTrackRowHeightPx * static_cast<f32>(track);
            const bool same_track = track < kTrackCount && static_cast<u32>(keyframe.kind) == track;
            const bool near_marker = std::fabs(io.MousePos.x - x) <= kMarkerWidthPx * 0.5f + kMarkerHitSlackPx;
            const bool in_row = io.MousePos.y >= marker_y && io.MousePos.y <= marker_y + kTrackRowHeightPx;
            if (same_track && near_marker && in_row)
            {
                hit = static_cast<i32>(i);
                break;
            }
        }
        if (hit >= 0)
        {
            SelectKeyframe(hit);
            m_bDraggingMarker = true;
            m_DragIdx = hit;
        }
        else if (io.MousePos.y >= origin.y && io.MousePos.y <= origin.y + ruler_height)
        {
            SetCurrentTimeSec(XToTime(io.MousePos.x, DurationSec(), origin.x, canvas_width));
        }
        else
        {
            SelectKeyframe(kNoKeySelected);
        }
    }
    if (m_bDraggingMarker && m_DragIdx >= 0 && static_cast<u32>(m_DragIdx) < m_Document.KeyframeCount())
    {
        FEditorKeyframe edited = EditorKeyframes()[static_cast<u32>(m_DragIdx)];
        edited.time_sec = XToTime(io.MousePos.x, DurationSec(), origin.x, canvas_width);
        u32 new_index = 0u;
        if (m_Document.TryReplace(static_cast<u32>(m_DragIdx), edited, &new_index))
        {
            m_DragIdx = static_cast<i32>(new_index);
            m_SelectedIdx = m_DragIdx;
        }
    }
    if (m_bDraggingMarker && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
    {
        (void)BakeToDirector();
        m_bDraggingMarker = false;
        m_DragIdx = kNoKeySelected;
    }
    const TSpan<const FEditorKeyframe> keyframes = EditorKeyframes();
    for (usize i = 0; i < keyframes.Num(); ++i)
    {
        const FEditorKeyframe& keyframe = keyframes[i];
        const f32 x = TimeToX(keyframe.time_sec, DurationSec(), origin.x, canvas_width);
        const f32 y = origin.y + ruler_height + track_gap + kTrackRowHeightPx * static_cast<f32>(static_cast<u32>(keyframe.kind));
        const ImVec2 marker_min(x - kMarkerWidthPx * 0.5f, y + 4.0f);
        const ImVec2 marker_max(x + kMarkerWidthPx * 0.5f, y + kTrackRowHeightPx - 4.0f);
        const bool selected = m_SelectedIdx == static_cast<i32>(i);
        const ImU32 outline = selected ? IM_COL32(255, 255, 255, 255) : IM_COL32(10, 10, 10, 220);
        draw->AddRectFilled(marker_min, marker_max, KindColor(keyframe.kind), 2.0f);
        draw->AddRect(marker_min, marker_max, outline, 2.0f, 0, selected ? 2.0f : 1.0f);
    }
    // 現在時刻の線を最後に描き、同時刻のマーカーより前面へ置きます。
    const f32 current_x = TimeToX(m_CurrentTime, DurationSec(), origin.x, canvas_width);
    draw->AddLine(ImVec2(current_x, origin.y), ImVec2(current_x, origin.y + canvas_height), IM_COL32(255, 80, 80, 220), 2.0f);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##timeline_right", ImVec2(0.0f, 0.0f), true);
    ImGui::TextUnformatted("Inspector");
    ImGui::Separator();
    if (m_SelectedIdx < 0 || static_cast<u32>(m_SelectedIdx) >= m_Document.KeyframeCount())
    {
        ImGui::TextDisabled("(no keyframe selected)");
        ImGui::TextDisabled("Click a marker in the timeline,");
        ImGui::TextDisabled("or use +Add to create one.");
    }
    else
    {
        FEditorKeyframe edited = EditorKeyframes()[static_cast<u32>(m_SelectedIdx)];
        ImGui::Text("Kind: %s", KindName(edited.kind));
        // 編集中の時刻を文書へ反映するための作業値です。
        float time = edited.time_sec;
        // 入力が変化したフレームだけ文書を置き換えます。
        bool edited_changed = false;
        if (ImGui::DragFloat("time (s)", &time, 0.05f, 0.0f, DurationSec(), "%.3f"))
        {
            edited.time_sec = ClampF(time, 0.0f, DurationSec());
            edited_changed = true;
        }
        if (edited.kind == ETimelineKeyKind::CameraCut)
        {
            float target[3] = { edited.camera_target.x, edited.camera_target.y, edited.camera_target.z };
            if (ImGui::DragFloat3("target", target, 0.1f))
            {
                edited.camera_target = FVec3{ target[0], target[1], target[2] };
                edited_changed = true;
            }
        }
        if (edited.kind == ETimelineKeyKind::FadeColor)
        {
            float start_color[3] = { edited.fade_start_color.x, edited.fade_start_color.y, edited.fade_start_color.z };
            float end_color[3] = { edited.fade_end_color.x, edited.fade_end_color.y, edited.fade_end_color.z };
            if (ImGui::ColorEdit3("start", start_color))
            {
                edited.fade_start_color = FVec3{ start_color[0], start_color[1], start_color[2] };
                edited_changed = true;
            }
            if (ImGui::ColorEdit3("end", end_color))
            {
                edited.fade_end_color = FVec3{ end_color[0], end_color[1], end_color[2] };
                edited_changed = true;
            }
        }
        if (edited.kind == ETimelineKeyKind::TimeScale)
        {
            float scale = edited.time_scale;
            if (ImGui::DragFloat("scale", &scale, 0.01f, 0.0f, 8.0f, "%.3f"))
            {
                edited.time_scale = scale;
                edited_changed = true;
            }
        }
        if (edited.kind == ETimelineKeyKind::SpawnEffect || edited.kind == ETimelineKeyKind::TriggerCallback)
        {
            int event_id = static_cast<int>(edited.event_id);
            const char* const event_label = edited.kind == ETimelineKeyKind::SpawnEffect ? "effect_id" : "event_id";
            if (ImGui::DragInt(event_label, &event_id, 1.0f, 0, 65535))
            {
                edited.event_id = static_cast<u32>(event_id < 0 ? 0 : event_id);
                edited_changed = true;
            }
        }
        if (edited.kind == ETimelineKeyKind::SpawnEffect)
        {
            float position[3] = { edited.camera_target.x, edited.camera_target.y, edited.camera_target.z };
            if (ImGui::DragFloat3("position", position, 0.1f))
            {
                edited.camera_target = FVec3{ position[0], position[1], position[2] };
                edited_changed = true;
            }
        }
        if (edited_changed)
        {
            u32 new_index = 0u;
            if (m_Document.TryReplace(static_cast<u32>(m_SelectedIdx), edited, &new_index))
            {
                m_SelectedIdx = static_cast<i32>(new_index);
                (void)BakeToDirector();
            }
        }
    }
    ImGui::Separator();
    ImGui::Text("Count: %u", static_cast<unsigned>(m_Document.KeyframeCount()));
    ImGui::Text("Director: %s", m_Director != nullptr ? "bound" : "(none)");
    ImGui::Text("Playing: %s", m_Playing ? "yes" : "no");
    ImGui::EndChild();
    ImGui::End();
}

}
