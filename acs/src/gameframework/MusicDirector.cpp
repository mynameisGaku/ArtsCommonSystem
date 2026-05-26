// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar H — MusicDirector 実装 (Phase 1: bridge スケルトン)
//
// state machine + transition / stinger pending 管理を完全実装。
// 実際の BGM / SFX 再生は AudioDirector 経由で Phase 2 に接続予定。
#include "gameframework/MusicDirector.h"
#include "foundation/Log.h"

namespace acs::game {

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

f32 MusicDirector::Clamp01(f32 v) noexcept {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

// 同一 TODO メッセージを毎フレーム吐かないよう、ファイル static の `done` flag
// で 1 度きりに絞る。AudioDirector::LogTodoOnce と同方針。
void MusicDirector::LogTodoOnce(const char* what) noexcept {
    static bool s_logged_set_state = false;
    static bool s_logged_stinger   = false;
    static bool s_logged_stop      = false;
    static bool s_logged_tick      = false;

    bool* slot = nullptr;
    if      (what == "SetState") slot = &s_logged_set_state;
    else if (what == "Stinger")  slot = &s_logged_stinger;
    else if (what == "Stop")     slot = &s_logged_stop;
    else if (what == "Tick")     slot = &s_logged_tick;

    if (slot != nullptr && *slot) return;
    if (slot != nullptr) *slot = true;
    ACS_LOG_INFO("MusicDirector: %s — Phase 2 で AudioDirector と接続予定 (現在は state のみ)", what);
}

// ----------------------------------------------------------------------------
// construction
// ----------------------------------------------------------------------------

MusicDirector::MusicDirector() noexcept {
    // 想定登録数で track 配列を事前確保 (拡張時の reallocation を抑える)。
    _tracks.Reserve(kTrackReserveHint);
    for (u32 i = 0; i < kStateCount; ++i) {
        _state_first[i] = 0;
        _state_count[i] = 0;
    }
}

// ----------------------------------------------------------------------------
// track 登録
// ----------------------------------------------------------------------------

void MusicDirector::RegisterTrack(EMusicState state, const MusicTrack& track) noexcept {
    if (track.asset_path == nullptr) {
        ACS_LOG_WARN("MusicDirector::RegisterTrack: asset_path=nullptr → ignored");
        return;
    }
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) {
        ACS_LOG_WARN("MusicDirector::RegisterTrack: invalid state=%u → ignored", state_idx);
        return;
    }

    // intensity range を [0, 1] にクランプし、min <= max を保証する。
    MusicTrack normalized = track;
    normalized.intensity_min = Clamp01(track.intensity_min);
    normalized.intensity_max = Clamp01(track.intensity_max);
    if (normalized.intensity_min > normalized.intensity_max) {
        ACS_LOG_WARN("MusicDirector::RegisterTrack: intensity_min(%.3f) > max(%.3f) → swapped",
                     normalized.intensity_min, normalized.intensity_max);
        const f32 tmp = normalized.intensity_min;
        normalized.intensity_min = normalized.intensity_max;
        normalized.intensity_max = tmp;
    }

    // 該当 state の末尾位置に挿入するため、それより後ろの track を 1 つずつ後方シフト。
    // (state ごとに連続区間を維持する SoA 戦略)
    const u32 insert_at = _state_first[state_idx] + _state_count[state_idx];
    _tracks.PushBack(MusicTrack{});  // 末尾に空き枠を確保
    for (usize i = _tracks.Size() - 1; i > insert_at; --i) {
        _tracks[i] = _tracks[i - 1];
    }
    _tracks[insert_at] = normalized;
    _state_count[state_idx] += 1;

    // 自身より後ろの state は first を 1 つずつ繰り下げる。
    for (u32 s = state_idx + 1; s < kStateCount; ++s) {
        _state_first[s] += 1;
    }
}

void MusicDirector::RebuildStateIndex() noexcept {
    // 現状は RegisterTrack 内で逐次更新しており、明示再構築は不要。
    // 将来 UnregisterTrack を導入する際の hook 用に空関数として残す。
}

usize MusicDirector::FindTrackForState(EMusicState state, f32 intensity) const noexcept {
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) return _tracks.Size();
    const u32 first = _state_first[state_idx];
    const u32 count = _state_count[state_idx];
    if (count == 0) return _tracks.Size();

    // 1) intensity 範囲ヒットを線形探索 (count は通常 1..4 個程度)。
    usize fallback = _tracks.Size();
    f32   fallback_max = -1.0f;
    for (u32 i = 0; i < count; ++i) {
        const usize idx = static_cast<usize>(first + i);
        const MusicTrack& t = _tracks[idx];
        if (intensity >= t.intensity_min && intensity <= t.intensity_max) {
            return idx;
        }
        // 範囲外でも「最も上限の高い track」を fallback として記憶。
        if (t.intensity_max > fallback_max) {
            fallback_max = t.intensity_max;
            fallback     = idx;
        }
    }
    // 2) どれも範囲ヒットせず → fallback (= 最強 intensity 帯の track) を返す。
    return fallback;
}

// ----------------------------------------------------------------------------
// 状態遷移
// ----------------------------------------------------------------------------

void MusicDirector::SetState(EMusicState state, f32 transition_sec) noexcept {
    const u32 state_idx = static_cast<u32>(state);
    if (state_idx >= kStateCount) {
        ACS_LOG_WARN("MusicDirector::SetState: invalid state=%u → ignored", state_idx);
        return;
    }
    LogTodoOnce("SetState");

    // 同一 state 再要求: no-op (現行を継続)。遷移中ならそれも継続。
    if (state == _target_state && !IsTransitioning()) {
        return;
    }

    if (transition_sec <= 0.0f) {
        // 即時切替: current / target ともに即合わせ、progress=1 にスナップ。
        _current_state        = state;
        _target_state         = state;
        _transition_duration  = 0.0f;
        _transition_elapsed   = 0.0f;
        _transition_progress  = 1.0f;
        return;
    }

    // クロスフェード開始: current は据え置き、target を新 state に。
    // 既に遷移中だった場合は「現在の current を起点に、新 target へ」再遷移。
    // (= 中断して新方向に切り替える。前の target は破棄される)
    _target_state         = state;
    _transition_duration  = transition_sec;
    _transition_elapsed   = 0.0f;
    _transition_progress  = 0.0f;
}

bool MusicDirector::IsTransitioning() const noexcept {
    return _current_state != _target_state && _transition_progress < 1.0f;
}

// ----------------------------------------------------------------------------
// intensity
// ----------------------------------------------------------------------------

void MusicDirector::SetIntensity(f32 intensity_0_to_1) noexcept {
    const f32 c = Clamp01(intensity_0_to_1);
    if (c != intensity_0_to_1) {
        ACS_LOG_WARN("MusicDirector::SetIntensity: out-of-range %.3f → clamped to %.3f",
                     intensity_0_to_1, c);
    }
    _intensity = c;
}

// ----------------------------------------------------------------------------
// Stinger
// ----------------------------------------------------------------------------

void MusicDirector::PlayStinger(const char* asset_path, f32 volume) noexcept {
    if (asset_path == nullptr) {
        ACS_LOG_WARN("MusicDirector::PlayStinger: asset_path=nullptr → ignored");
        return;
    }
    if (volume <= 0.0f) {
        ACS_LOG_WARN("MusicDirector::PlayStinger: volume=%.3f <= 0 → ignored", volume);
        return;
    }
    if (_stinger_pending) {
        // 既存 pending を上書き (最新採用ポリシー、スタックしない)。
        ACS_LOG_WARN("MusicDirector::PlayStinger: pending stinger overwritten (%s → %s)",
                     _stinger_path, asset_path);
    }
    LogTodoOnce("Stinger");
    _stinger_path    = asset_path;
    _stinger_volume  = volume;
    _stinger_pending = true;
}

const char* MusicDirector::ConsumeStinger(f32& out_volume) noexcept {
    if (!_stinger_pending) {
        out_volume = 0.0f;
        return nullptr;
    }
    const char* path = _stinger_path;
    out_volume       = _stinger_volume;
    _stinger_pending = false;
    _stinger_path    = nullptr;
    _stinger_volume  = 0.0f;
    return path;
}

// ----------------------------------------------------------------------------
// driver
// ----------------------------------------------------------------------------

void MusicDirector::Tick(f32 dt) noexcept {
    if (dt < 0.0f) dt = 0.0f;
    LogTodoOnce("Tick");

    // クロスフェード進行。current == target なら何もしない (定常状態)。
    if (_current_state != _target_state) {
        if (_transition_duration <= 0.0f) {
            // 不正状態 (duration=0 で current != target) → 即 snap して防御。
            _current_state       = _target_state;
            _transition_progress = 1.0f;
            return;
        }
        _transition_elapsed += dt;
        const f32 t = _transition_elapsed / _transition_duration;
        if (t >= 1.0f) {
            // 遷移完了: current を target に snap。
            _current_state       = _target_state;
            _transition_progress = 1.0f;
            _transition_elapsed  = 0.0f;
            _transition_duration = 0.0f;
        } else {
            _transition_progress = t;
        }
    }
}

// ----------------------------------------------------------------------------
// Stop
// ----------------------------------------------------------------------------

void MusicDirector::Stop() noexcept {
    LogTodoOnce("Stop");
    _current_state        = EMusicState::Silent;
    _target_state         = EMusicState::Silent;
    _transition_duration  = 0.0f;
    _transition_elapsed   = 0.0f;
    _transition_progress  = 1.0f;
    // stinger pending も破棄 (一律停止のため)。
    _stinger_pending = false;
    _stinger_path    = nullptr;
    _stinger_volume  = 0.0f;
    // intensity は保持 (next state 開始時の体験を断絶させない)。
}

// ----------------------------------------------------------------------------
// 派生情報
// ----------------------------------------------------------------------------

const MusicTrack* MusicDirector::CurrentTrack() const noexcept {
    const usize idx = FindTrackForState(_current_state, _intensity);
    if (idx >= _tracks.Size()) return nullptr;
    return &_tracks[idx];
}

const MusicTrack* MusicDirector::TargetTrack() const noexcept {
    if (_current_state == _target_state) return nullptr;
    const usize idx = FindTrackForState(_target_state, _intensity);
    if (idx >= _tracks.Size()) return nullptr;
    return &_tracks[idx];
}

} // namespace acs::game
