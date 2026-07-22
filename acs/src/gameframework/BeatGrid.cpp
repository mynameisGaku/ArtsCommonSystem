// SPDX-License-Identifier: Apache-2.0
#include "gameframework/BeatGrid.h"

#include <cfloat>
#include <cmath>

namespace acs::game {
namespace {

bool IsValidLane(EBeatLane lane) noexcept {
    return static_cast<u32>(lane) < kBeatLaneCount;
}

f32 Abs(f32 value) noexcept {
    return value < 0.0f ? -value : value;
}

f32 Clamp(f32 value, f32 low, f32 high) noexcept {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

EJudgement WorseJudgement(EJudgement a, EJudgement b) noexcept {
    return static_cast<u8>(a) >= static_cast<u8>(b) ? a : b;
}

} // namespace

const char* BeatChartLoadResultName(EBeatChartLoadResult result) noexcept {
    switch (result) {
        case EBeatChartLoadResult::Success: return "Success";
        case EBeatChartLoadResult::NullNotes: return "NullNotes";
        case EBeatChartLoadResult::TooManyNotes: return "TooManyNotes";
        case EBeatChartLoadResult::InvalidBpm: return "InvalidBpm";
        case EBeatChartLoadResult::InvalidNoteTime: return "InvalidNoteTime";
        case EBeatChartLoadResult::InvalidLane: return "InvalidLane";
        case EBeatChartLoadResult::InvalidHoldDuration: return "InvalidHoldDuration";
        case EBeatChartLoadResult::OutOfMemory: return "OutOfMemory";
    }
    return "Unknown";
}

FBeatGrid::FBeatGrid(FAllocator& allocator) noexcept
    : m_Notes(allocator), m_Judged(allocator), m_Holds(allocator) {
}

void FBeatGrid::BumpRevision() noexcept {
    ++m_StateRevision;
    if (m_StateRevision == 0u) m_StateRevision = 1u;
}

void FBeatGrid::ClearActiveHolds() noexcept {
    for (usize i = 0; i < m_Holds.Size(); ++i) {
        m_Holds[i].active = false;
        m_Holds[i].head_judgement = EJudgement::Miss;
    }
}

void FBeatGrid::ResetRunState() noexcept {
    m_CurrentTime = 0.0f;
    m_Playing = false;
    m_Paused = false;
    m_bEndedFired = false;
    m_PerfectCount = 0u;
    m_GreatCount = 0u;
    m_GoodCount = 0u;
    m_MissCount = 0u;
    m_CurrentCombo = 0u;
    m_MaxCombo = 0u;

    for (usize i = 0; i < m_Judged.Size(); ++i) m_Judged[i] = false;
    ClearActiveHolds();
}

void FBeatGrid::Init() noexcept {
    BumpRevision();
    ResetRunState();
}

EBeatChartLoadResult FBeatGrid::TryLoadChart(
    const FBeatNote* notes, u32 count, f32 bpm) noexcept {
    if (count > kMaxBeatChartNotes) return EBeatChartLoadResult::TooManyNotes;
    if (count != 0u && notes == nullptr) return EBeatChartLoadResult::NullNotes;
    if (!std::isfinite(bpm) || bpm < 0.0f || bpm > kMaxBeatBpm) {
        return EBeatChartLoadResult::InvalidBpm;
    }

    for (u32 i = 0u; i < count; ++i) {
        const FBeatNote& note = notes[i];
        if (!std::isfinite(note.time_sec) || note.time_sec < 0.0f
            || note.time_sec > kMaxBeatChartTimeSec) {
            return EBeatChartLoadResult::InvalidNoteTime;
        }
        if (!IsValidLane(note.lane)) return EBeatChartLoadResult::InvalidLane;
        if (!std::isfinite(note.hold_duration_sec)
            || note.hold_duration_sec < 0.0f
            || note.hold_duration_sec > kMaxBeatHoldDurationSec) {
            return EBeatChartLoadResult::InvalidHoldDuration;
        }
        if (note.is_hold) {
            if (note.hold_duration_sec <= 0.0f
                || note.hold_duration_sec
                    > kMaxBeatChartTimeSec - note.time_sec) {
                return EBeatChartLoadResult::InvalidHoldDuration;
            }
        }
    }

    FAllocator& allocator = *m_Notes.GetAllocator();
    TArray<FBeatNote> staged_notes(allocator);
    TArray<bool> staged_judged(allocator);
    TArray<FHoldState> staged_holds(allocator);
    const usize staged_count = static_cast<usize>(count);

    if (!staged_notes.TryReserve(staged_count)
        || !staged_judged.TryReserve(staged_count)
        || !staged_holds.TryReserve(staged_count)) {
        return EBeatChartLoadResult::OutOfMemory;
    }

    for (u32 i = 0u; i < count; ++i) {
        FBeatNote note = notes[i];
        if (!note.is_hold) note.hold_duration_sec = 0.0f;
        const FHoldState hold{};
        if (!staged_notes.TryPushBack(note)
            || !staged_judged.TryPushBack(false)
            || !staged_holds.TryPushBack(hold)) {
            return EBeatChartLoadResult::OutOfMemory;
        }
    }

    m_Notes = Move(staged_notes);
    m_Judged = Move(staged_judged);
    m_Holds = Move(staged_holds);
    m_Bpm = bpm;
    m_TotalNotes = count;
    BumpRevision();
    ResetRunState();
    return EBeatChartLoadResult::Success;
}

void FBeatGrid::LoadChart(
    const FBeatNote* notes, u32 count, f32 bpm) noexcept {
    (void)TryLoadChart(notes, count, bpm);
}

bool FBeatGrid::TrySetTimingWindows(
    f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept {
    if (!std::isfinite(perfect_ms) || !std::isfinite(great_ms)
        || !std::isfinite(good_ms) || perfect_ms < 0.0f
        || perfect_ms > kMaxBeatTimingWindowMs
        || great_ms < perfect_ms || great_ms > kMaxBeatTimingWindowMs
        || good_ms < great_ms || good_ms > kMaxBeatTimingWindowMs) {
        return false;
    }

    m_PerfectWindowSec = perfect_ms * 0.001f;
    m_GreatWindowSec = great_ms * 0.001f;
    m_GoodWindowSec = good_ms * 0.001f;
    BumpRevision();
    return true;
}

void FBeatGrid::SetTimingWindows(
    f32 perfect_ms, f32 great_ms, f32 good_ms) noexcept {
    if (!std::isfinite(perfect_ms) || !std::isfinite(great_ms)
        || !std::isfinite(good_ms)) {
        return;
    }

    const f32 perfect =
        Clamp(perfect_ms, 0.0f, kMaxBeatTimingWindowMs);
    const f32 great =
        Clamp(great_ms, perfect, kMaxBeatTimingWindowMs);
    const f32 good =
        Clamp(good_ms, great, kMaxBeatTimingWindowMs);
    m_PerfectWindowSec = perfect * 0.001f;
    m_GreatWindowSec = great * 0.001f;
    m_GoodWindowSec = good * 0.001f;
    BumpRevision();
}

void FBeatGrid::Start() noexcept {
    BumpRevision();
    ResetRunState();
    m_Playing = true;
}

void FBeatGrid::Stop() noexcept {
    BumpRevision();
    m_Playing = false;
    m_Paused = false;
    m_CurrentTime = 0.0f;
    ClearActiveHolds();
}

void FBeatGrid::Pause() noexcept {
    if (m_Playing && !m_Paused) {
        m_Paused = true;
        BumpRevision();
    }
}

void FBeatGrid::Resume() noexcept {
    if (m_Playing && m_Paused) {
        m_Paused = false;
        BumpRevision();
    }
}

EJudgement FBeatGrid::ClassifyDelta(f32 abs_delta_sec) const noexcept {
    if (abs_delta_sec <= m_PerfectWindowSec) return EJudgement::Perfect;
    if (abs_delta_sec <= m_GreatWindowSec) return EJudgement::Great;
    if (abs_delta_sec <= m_GoodWindowSec) return EJudgement::Good;
    return EJudgement::Miss;
}

usize FBeatGrid::FindNearestNote(EBeatLane lane) const noexcept {
    const usize count = m_Notes.Size();
    usize best_index = count;
    f32 best_delta = 0.0f;

    for (usize i = 0; i < count; ++i) {
        if (m_Judged[i] || m_Holds[i].active || m_Notes[i].lane != lane) {
            continue;
        }
        const f32 delta = Abs(m_Notes[i].time_sec - m_CurrentTime);
        if (delta > m_GoodWindowSec) continue;
        if (best_index == count || delta < best_delta) {
            best_index = i;
            best_delta = delta;
        }
    }
    return best_index;
}

usize FBeatGrid::FindNearestActiveHold(EBeatLane lane) const noexcept {
    const usize count = m_Notes.Size();
    usize best_index = count;
    f32 best_delta = 0.0f;

    for (usize i = 0; i < count; ++i) {
        if (!m_Holds[i].active || m_Notes[i].lane != lane) continue;
        const f32 tail_time =
            m_Notes[i].time_sec + m_Notes[i].hold_duration_sec;
        const f32 delta = Abs(tail_time - m_CurrentTime);
        if (best_index == count || delta < best_delta) {
            best_index = i;
            best_delta = delta;
        }
    }
    return best_index;
}

bool FBeatGrid::ApplyJudgement(
    EBeatLane lane, EJudgement judgement) noexcept {
    switch (judgement) {
        case EJudgement::Perfect:
            ++m_PerfectCount;
            ++m_CurrentCombo;
            break;
        case EJudgement::Great:
            ++m_GreatCount;
            ++m_CurrentCombo;
            break;
        case EJudgement::Good:
            ++m_GoodCount;
            ++m_CurrentCombo;
            break;
        case EJudgement::Miss:
            ++m_MissCount;
            m_CurrentCombo = 0u;
            break;
    }
    if (m_CurrentCombo > m_MaxCombo) m_MaxCombo = m_CurrentCombo;

    const u64 revision = m_StateRevision;
    if (m_JudgeCb != nullptr) {
        m_JudgeCb(m_JudgeUser, lane, judgement, m_CurrentCombo);
    }
    return revision == m_StateRevision;
}

bool FBeatGrid::FireEndIfComplete() noexcept {
    if (m_bEndedFired) return true;
    for (usize i = 0; i < m_Judged.Size(); ++i) {
        if (!m_Judged[i]) return true;
    }

    m_bEndedFired = true;
    const u64 revision = m_StateRevision;
    if (m_EndCb != nullptr) {
        m_EndCb(m_EndUser, HitNotes(), m_MissCount, Accuracy());
    }
    return revision == m_StateRevision;
}

EJudgement FBeatGrid::PressLane(EBeatLane lane) noexcept {
    if (!m_Playing || !IsValidLane(lane)) return EJudgement::Miss;

    const usize index = FindNearestNote(lane);
    if (index >= m_Notes.Size()) return EJudgement::Miss;

    const EJudgement judgement =
        ClassifyDelta(Abs(m_Notes[index].time_sec - m_CurrentTime));
    if (judgement == EJudgement::Miss) return judgement;

    if (m_Notes[index].is_hold) {
        m_Holds[index].active = true;
        m_Holds[index].head_judgement = judgement;
        return judgement;
    }

    m_Judged[index] = true;
    (void)ApplyJudgement(lane, judgement);
    return judgement;
}

EJudgement FBeatGrid::ReleaseLane(EBeatLane lane) noexcept {
    if (!m_Playing || !IsValidLane(lane)) return EJudgement::Miss;

    const usize index = FindNearestActiveHold(lane);
    if (index >= m_Notes.Size()) return EJudgement::Miss;

    const FBeatNote& note = m_Notes[index];
    const f32 tail_time = note.time_sec + note.hold_duration_sec;
    const EJudgement tail_judgement =
        ClassifyDelta(Abs(tail_time - m_CurrentTime));
    const EJudgement final_judgement =
        WorseJudgement(m_Holds[index].head_judgement, tail_judgement);

    m_Holds[index].active = false;
    m_Holds[index].head_judgement = EJudgement::Miss;
    m_Judged[index] = true;
    (void)ApplyJudgement(lane, final_judgement);
    return final_judgement;
}

bool FBeatGrid::IsLaneHolding(EBeatLane lane) const noexcept {
    if (!IsValidLane(lane)) return false;
    for (usize i = 0; i < m_Holds.Size(); ++i) {
        if (m_Holds[i].active && m_Notes[i].lane == lane) return true;
    }
    return false;
}

u32 FBeatGrid::ActiveHoldCount() const noexcept {
    u32 count = 0u;
    for (usize i = 0; i < m_Holds.Size(); ++i) {
        if (m_Holds[i].active) ++count;
    }
    return count;
}

void FBeatGrid::Tick(f32 dt) noexcept {
    if (!m_Playing || m_Paused || !std::isfinite(dt) || dt <= 0.0f) {
        return;
    }
    if (m_CurrentTime > FLT_MAX - dt) return;

    m_CurrentTime += dt;
    const usize count = m_Notes.Size();
    const u64 revision = m_StateRevision;

    for (usize i = 0; i < count; ++i) {
        if (m_Judged[i]) continue;

        bool missed = false;
        if (m_Holds[i].active) {
            const f32 tail_time =
                m_Notes[i].time_sec + m_Notes[i].hold_duration_sec;
            missed = m_CurrentTime > tail_time + m_GoodWindowSec;
        } else {
            missed = m_CurrentTime
                > m_Notes[i].time_sec + m_GoodWindowSec;
        }
        if (!missed) continue;

        const EBeatLane lane = m_Notes[i].lane;
        m_Holds[i].active = false;
        m_Holds[i].head_judgement = EJudgement::Miss;
        m_Judged[i] = true;
        if (!ApplyJudgement(lane, EJudgement::Miss)
            || revision != m_StateRevision) {
            return;
        }
    }

    if (revision == m_StateRevision) (void)FireEndIfComplete();
}

f32 FBeatGrid::Accuracy() const noexcept {
    if (m_TotalNotes == 0u) return 1.0f;
    const f32 weighted =
        static_cast<f32>(m_PerfectCount)
        + static_cast<f32>(m_GreatCount) * 0.8f
        + static_cast<f32>(m_GoodCount) * 0.5f;
    return weighted / static_cast<f32>(m_TotalNotes);
}

void FBeatGrid::ClearAll() noexcept {
    BumpRevision();
    m_Notes.Clear();
    m_Judged.Clear();
    m_Holds.Clear();
    m_Bpm = 0.0f;
    m_PerfectWindowSec = 0.025f;
    m_GreatWindowSec = 0.050f;
    m_GoodWindowSec = 0.100f;
    m_CurrentTime = 0.0f;
    m_Playing = false;
    m_Paused = false;
    m_bEndedFired = false;
    m_TotalNotes = 0u;
    m_PerfectCount = 0u;
    m_GreatCount = 0u;
    m_GoodCount = 0u;
    m_MissCount = 0u;
    m_CurrentCombo = 0u;
    m_MaxCombo = 0u;
    m_JudgeCb = nullptr;
    m_JudgeUser = nullptr;
    m_EndCb = nullptr;
    m_EndUser = nullptr;
}

} // namespace acs::game
