// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar (ジャンルキット: Racing) — FLapTimer 実装
//
// slot+gen で racer を管理し、Tick で race clock を進めつつ、collider 側からの
// NotifyCheckpointPassed / NotifyLapCompleted で進捗を蓄積する。順位算出は
// 要求時に IsBetterRank 比較で線形走査する (racer 数 << 32 想定なので十分)。
#include "gameframework/LapTimer.h"

namespace acs::game {

u32 FLapTimer::AcquireSlot() noexcept {
    // index 0 を invalid 予約として残す (dummy slot)。
    for (u32 i = 1; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    if (m_Slots.IsEmpty()) {
        m_Slots.PushBack({});   // dummy at index 0
    }
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

FLapTimer::Slot* FLapTimer::FindSlot(RacerId id) noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

const FLapTimer::Slot* FLapTimer::FindSlot(RacerId id) const noexcept {
    if (!id.IsValid()) return nullptr;
    const u32 idx = id.Index();
    if (idx == 0 || idx >= m_Slots.Size()) return nullptr;
    const Slot& s = m_Slots[idx];
    if (!s.active) return nullptr;
    if (s.gen != id.Generation()) return nullptr;
    return &s;
}

bool FLapTimer::IsBetterRank(const RacerStats& lhs, const RacerStats& rhs) const noexcept {
    // 1. 残りラップ少ない方が上 (= current_lap 多い方が上)。
    if (lhs.current_lap != rhs.current_lap) {
        return lhs.current_lap > rhs.current_lap;
    }
    // 2. 同 lap なら checkpoint 多い方が上。
    if (lhs.checkpoints_passed != rhs.checkpoints_passed) {
        return lhs.checkpoints_passed > rhs.checkpoints_passed;
    }
    // 3. 同 lap + 同 checkpoint なら経過時間が短い方が上 (先に到達した)。
    return lhs.total_time_sec < rhs.total_time_sec;
}

void FLapTimer::Init(u32 total_laps, u32 checkpoints_per_lap) noexcept {
    // 0 周 / 0 checkpoint は意味を持たないので 1 にクランプ。
    m_TotalLaps          = total_laps          == 0 ? 1u : total_laps;
    m_CheckpointsPerLap = checkpoints_per_lap == 0 ? 1u : checkpoints_per_lap;
    _state               = State::Stopped;
    m_RaceTimeSec       = 0.0f;
    m_FinishedCount      = 0;

    // racer 一覧は維持。各 stats の進捗だけリセットする。
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        s.stats.current_lap        = 0;
        s.stats.checkpoints_passed = 0;
        s.stats.total_time_sec     = 0.0f;
        s.stats.best_lap_time_sec  = 0.0f;
        s.stats.racer_position     = 0;
        s.records.Clear();
        s.expected_checkpoint      = 0;
        s.lap_start_time           = 0.0f;
        s.finished                 = false;
    }
}

RacerId FLapTimer::AddRacer(const char* display_name) noexcept {
    const u32 idx = AcquireSlot();
    Slot& s = m_Slots[idx];

    // generation を進める (0 は invalid 扱いなので回避)。
    s.gen = static_cast<u8>(s.gen + 1u);
    if (s.gen == 0) s.gen = 1;

    s.active                   = true;
    s.finished                 = false;
    s.expected_checkpoint      = 0;
    s.lap_start_time           = 0.0f;
    s.records.Clear();

    s.stats.display_name       = display_name;
    s.stats.current_lap        = 0;
    s.stats.checkpoints_passed = 0;
    s.stats.total_time_sec     = 0.0f;
    s.stats.best_lap_time_sec  = 0.0f;
    s.stats.racer_position     = 0;

    ++m_RacerCount;
    return RacerId{idx, s.gen};
}

void FLapTimer::RemoveRacer(RacerId id) noexcept {
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    // gen はそのまま。次の AddRacer で gen が +1 されて古い handle を無効化する。
    s->active                   = false;
    s->finished                 = false;
    s->expected_checkpoint      = 0;
    s->lap_start_time           = 0.0f;
    s->records.Clear();
    s->stats.display_name       = nullptr;
    s->stats.current_lap        = 0;
    s->stats.checkpoints_passed = 0;
    s->stats.total_time_sec     = 0.0f;
    s->stats.best_lap_time_sec  = 0.0f;
    s->stats.racer_position     = 0;
    if (m_RacerCount > 0) --m_RacerCount;
}

void FLapTimer::Start() noexcept {
    _state          = State::Running;
    m_RaceTimeSec  = 0.0f;
    m_FinishedCount = 0;

    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        s.stats.current_lap        = 0;
        s.stats.checkpoints_passed = 0;
        s.stats.total_time_sec     = 0.0f;
        s.stats.best_lap_time_sec  = 0.0f;
        s.stats.racer_position     = 0;
        s.records.Clear();
        s.expected_checkpoint      = 0;
        s.lap_start_time           = 0.0f;
        s.finished                 = false;
    }
}

void FLapTimer::Stop() noexcept {
    // Stopped: Tick が止まる。値は据置 (リザルト表示用)。
    _state = State::Stopped;
}

void FLapTimer::Pause() noexcept {
    // Running 中のみ受理 (Stopped を Paused にすると Resume で誤って動き出すため)。
    if (_state == State::Running) {
        _state = State::Paused;
    }
}

void FLapTimer::Resume() noexcept {
    if (_state == State::Paused) {
        _state = State::Running;
    }
}

bool FLapTimer::IsRunning() const noexcept {
    return _state == State::Running;
}

void FLapTimer::NotifyCheckpointPassed(RacerId id, u32 checkpoint_index) noexcept {
    if (_state != State::Running) return;     // 開始前 / 停止中 / Pause 中は無視
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (s->finished) return;                  // フィニッシュ済 racer は no-op

    // 順序検証: expected_checkpoint と一致した場合のみ受理。
    // 違う index は黙って棄却 (ショートカット / 重複通知に強い)。
    if (checkpoint_index != s->expected_checkpoint) return;

    ++s->expected_checkpoint;
    s->stats.checkpoints_passed = s->expected_checkpoint;
    // expected_checkpoint == m_CheckpointsPerLap になった時点で
    // 「全 checkpoint 完了」状態。次の NotifyLapCompleted で受理される。
}

void FLapTimer::NotifyLapCompleted(RacerId id) noexcept {
    if (_state != State::Running) return;
    Slot* s = FindSlot(id);
    if (s == nullptr) return;
    if (s->finished) return;

    // 全 checkpoint を踏み切っていなければ棄却 (= 逆走防止)。
    if (s->expected_checkpoint < m_CheckpointsPerLap) return;

    // ラップ時間 = 現在 race time - ラップ開始時刻。
    const f32 lap_time   = m_RaceTimeSec - s->lap_start_time;
    const u32 lap_number = s->stats.current_lap + 1u;   // 1-origin

    // ベスト判定。
    // 初回ラップ (best_lap_time_sec == 0.0f) は常に PB として記録。
    bool is_pb = false;
    if (s->stats.best_lap_time_sec <= 0.0f || lap_time < s->stats.best_lap_time_sec) {
        s->stats.best_lap_time_sec = lap_time;
        is_pb = true;
    }

    // LapRecord に追加。
    LapRecord rec;
    rec.lap_index        = lap_number;
    rec.lap_time_sec     = lap_time;
    rec.split_time_sec   = m_RaceTimeSec;
    rec.is_personal_best = is_pb;
    s->records.PushBack(rec);

    // 次ラップ準備。
    s->stats.current_lap        = lap_number;
    s->stats.checkpoints_passed = 0;
    s->expected_checkpoint      = 0;
    s->lap_start_time           = m_RaceTimeSec;

    // callback 発火 (state 確定後)。
    if (m_OnLap != nullptr) {
        m_OnLap(m_OnLapUser, id, lap_number, lap_time, is_pb);
    }

    // フィニッシュ判定: total_laps 到達でゴール確定。
    if (lap_number >= m_TotalLaps) {
        s->finished = true;
        ++m_FinishedCount;
        s->stats.racer_position = m_FinishedCount;   // 1, 2, 3, ... 順
        if (m_OnFinish != nullptr) {
            m_OnFinish(m_OnFinishUser, id, m_RaceTimeSec, m_FinishedCount);
        }
    }
}

void FLapTimer::Tick(f32 dt) noexcept {
    if (_state != State::Running) return;
    if (dt <= 0.0f) return;

    m_RaceTimeSec += dt;

    // フィニッシュ未確定の racer のみ total_time を進める。
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (s.finished) continue;
        s.stats.total_time_sec += dt;
    }
}

f32 FLapTimer::RaceTimeSec() const noexcept {
    return m_RaceTimeSec;
}

const RacerStats* FLapTimer::GetStats(RacerId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return nullptr;
    return &s->stats;
}

RacerId FLapTimer::GetLeader() const noexcept {
    // フィニッシュ済 racer が居れば、その中で racer_position == 1 を返す。
    // 未フィニッシュ集合は IsBetterRank で best を選ぶ。
    const usize n = m_Slots.Size();

    // 既ゴール組から 1 位を探す。
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active || !s.finished) continue;
        if (s.stats.racer_position == 1u) {
            return RacerId{static_cast<u32>(i), s.gen};
        }
    }

    // ゴール組が居ない or 1 位がまだ確定していない場合、走行中ベストを返す。
    u32         best_idx = 0;
    const Slot* best_s   = nullptr;
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (s.finished) continue;   // ゴール組は別計上
        if (best_s == nullptr || IsBetterRank(s.stats, best_s->stats)) {
            best_idx = static_cast<u32>(i);
            best_s   = &s;
        }
    }

    if (best_s == nullptr) return RacerId{};
    return RacerId{best_idx, best_s->gen};
}

u32 FLapTimer::PositionOf(RacerId id) const noexcept {
    const Slot* target = FindSlot(id);
    if (target == nullptr) return 0;

    // フィニッシュ済なら確定値を返す。
    if (target->finished) return target->stats.racer_position;

    // 走行中: 自分より上位の racer 数 + 1 (フィニッシュ済 + 走行中ベター両方)。
    // フィニッシュ済はすべて走行中より上位とみなす (ゴール順位確定済のため)。
    u32 better = 0;
    const usize n = m_Slots.Size();
    for (usize i = 1; i < n; ++i) {
        const Slot& s = m_Slots[i];
        if (!s.active) continue;
        if (&s == target) continue;
        if (s.finished) {
            ++better;
            continue;
        }
        // 走行中同士は IsBetterRank で比較。
        if (IsBetterRank(s.stats, target->stats)) {
            ++better;
        }
    }
    return better + 1u;
}

u32 FLapTimer::LapRecordCount(RacerId id) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return 0;
    return static_cast<u32>(s->records.Size());
}

const LapRecord* FLapTimer::GetLapRecord(RacerId id, u32 lap_index) const noexcept {
    const Slot* s = FindSlot(id);
    if (s == nullptr) return nullptr;
    if (static_cast<usize>(lap_index) >= s->records.Size()) return nullptr;
    return &s->records[lap_index];
}

void FLapTimer::SetOnLapCallback(LapCallback cb, void* user) noexcept {
    m_OnLap      = cb;
    m_OnLapUser = user;
}

void FLapTimer::SetOnFinishCallback(FinishCallback cb, void* user) noexcept {
    m_OnFinish      = cb;
    m_OnFinishUser = user;
}

void FLapTimer::ClearAll() noexcept {
    m_Slots.Clear();
    m_RacerCount    = 0;
    _state          = State::Stopped;
    m_RaceTimeSec  = 0.0f;
    m_FinishedCount = 0;
    // total_laps / checkpoints_per_lap / callback は保持 (再 Start で同レース継続可能)。
}

} // namespace acs::game
