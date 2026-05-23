// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R Phase 2 — CinematicsDirector 実装
//
// 状態遷移:
//   Stopped (_playing=false, _time=0)
//     -> Play() -> Playing
//     Playing -> Pause() -> Paused (_playing=false, _time 維持)
//                Paused -> Play() -> Playing (Resume)
//     Playing -> Stop() -> Stopped
//     Playing -> Tick で全 keyframe 発火 -> _last_fired_index == size (Finished)
//
// 発火順序:
//   AddKeyframe で time_sec 昇順を維持しているので、Tick / Skip では
//   _last_fired_index から線形に進めるだけで自然に時刻順発火になる。
//   同時刻 (= 同じ time_sec) の keyframe は登録順に発火する (stable insertion)。
#include "gameframework/CinematicsDirector.h"

namespace acs::game {

void CinematicsDirector::AddKeyframe(const TimelineKeyframe& kf) noexcept {
    // 負の time_sec は 0 に clamp して受け入れる (= タイムライン頭打ち発火)。
    TimelineKeyframe entry = kf;
    if (entry.time_sec < 0.0f) entry.time_sec = 0.0f;

    // time_sec 昇順 / 同時刻は登録順 (stable) を維持する挿入位置を線形探索。
    // 典型 N < 100 なので O(N) で実用上問題なし。strict less-than 比較で
    // 「同時刻のものは後ろ」になり stable insertion になる。
    const usize n = _keyframes.Size();
    usize insert_at = n;
    for (usize i = 0; i < n; ++i) {
        if (entry.time_sec < _keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }

    if (insert_at == n) {
        // 末尾に追加 (時刻が現在の最大以上)
        _keyframes.PushBack(entry);
    } else {
        // [insert_at..n-1] を 1 つ後ろにずらして空きを作る
        _keyframes.PushBack(_keyframes[n - 1]);  // 末尾を増やす (旧末尾のコピー)
        for (usize i = n - 1; i > insert_at; --i) {
            _keyframes[i] = _keyframes[i - 1];
        }
        _keyframes[insert_at] = entry;
    }

    // 挿入位置が _last_fired_index 以下だと「過去 keyframe が増えた」状態に
    // なるが、再生中の場合は時間がそこを既に過ぎているため次 Tick で即発火
    // される。設計上は「キーフレーム追加は基本セットアップ段階のみ行う」前提
    // なので _last_fired_index の補正はしない (Play() 前なら _last_fired=0)。
}

void CinematicsDirector::Clear() noexcept {
    _keyframes.Clear();
    _time             = 0.0f;
    _last_fired_index = 0u;
    _playing          = false;
}

void CinematicsDirector::Play() noexcept {
    _playing = true;
}

void CinematicsDirector::Pause() noexcept {
    _playing = false;
}

void CinematicsDirector::Stop() noexcept {
    _playing          = false;
    _time             = 0.0f;
    _last_fired_index = 0u;
}

void CinematicsDirector::Skip() noexcept {
    // 残り全 keyframe を時刻昇順に発火。_time は TotalDuration() に進める。
    // Skip は Play 中でなくても呼べる (= 即座に "終わった状態" に飛ばす操作)。
    const f32 total = TotalDuration();
    FireUpTo(total);
    _time             = total;
    _last_fired_index = static_cast<u32>(_keyframes.Size());
    // _playing は維持 (= caller が Stop / Pause を別途呼ぶ責務)。Skip 後に
    // IsFinished() が true になるので、Tick が呼ばれ続けても無害。
}

bool CinematicsDirector::IsFinished() const noexcept {
    return _last_fired_index >= static_cast<u32>(_keyframes.Size());
}

void CinematicsDirector::Tick(f32 dt) noexcept {
    if (!_playing) return;
    if (dt <= 0.0f) return;

    _time += dt;
    FireUpTo(_time);
}

f32 CinematicsDirector::TotalDuration() const noexcept {
    // 昇順を維持しているので末尾 (Back) の time_sec が最大。
    if (_keyframes.Size() == 0) return 0.0f;
    return _keyframes[_keyframes.Size() - 1].time_sec;
}

void CinematicsDirector::SetCameraCallback(CameraCallbackFn cb, void* user) noexcept {
    _camera_cb   = cb;
    _camera_user = user;
}

void CinematicsDirector::SetDialogueCallback(DialogueCallbackFn cb, void* user) noexcept {
    _dialogue_cb   = cb;
    _dialogue_user = user;
}

void CinematicsDirector::SetMusicCallback(MusicCallbackFn cb, void* user) noexcept {
    _music_cb   = cb;
    _music_user = user;
}

void CinematicsDirector::SetEventCallback(EventCallbackFn cb, void* user) noexcept {
    _event_cb   = cb;
    _event_user = user;
}

// ===== private =====

void CinematicsDirector::FireUpTo(f32 up_to_time) noexcept {
    const u32 n = static_cast<u32>(_keyframes.Size());
    while (_last_fired_index < n) {
        const TimelineKeyframe& kf = _keyframes[_last_fired_index];
        if (kf.time_sec > up_to_time) break;   // 未来の keyframe はまだ発火しない
        FireOne(kf);
        ++_last_fired_index;
    }
}

void CinematicsDirector::FireOne(const TimelineKeyframe& kf) noexcept {
    switch (kf.kind) {
    case ETimelineTrackKind::Wait:
        // 何もしない (時間進行マーカー)。
        break;

    case ETimelineTrackKind::MoveCamera:
        if (_camera_cb != nullptr) {
            _camera_cb(_camera_user,
                       kf.payload.camera.target_pos,
                       kf.payload.camera.zoom,
                       kf.payload.camera.duration);
        }
        break;

    case ETimelineTrackKind::ShowDialogue:
        if (_dialogue_cb != nullptr) {
            _dialogue_cb(_dialogue_user, kf.payload.dialogue.line_id);
        }
        break;

    case ETimelineTrackKind::PlayMusic:
        if (_music_cb != nullptr) {
            _music_cb(_music_user,
                      kf.payload.music.music_id,
                      kf.payload.music.fade);
        }
        break;

    case ETimelineTrackKind::FireEvent:
        if (_event_cb != nullptr) {
            _event_cb(_event_user, kf.payload.event.event_id);
        }
        break;
    }
}

} // namespace acs::game
