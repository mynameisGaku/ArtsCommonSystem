// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — FCinematicsDirector 実装
//
// 状態遷移:
//   Stopped (m_Playing=false, m_Time=0)
//     -> Play() -> Playing
//     Playing -> Pause() -> Paused (m_Playing=false, m_Time 維持)
//                Paused -> Play() -> Playing (Resume)
//     Playing -> Stop() -> Stopped
//     Playing -> Tick で全 keyframe 発火 -> m_LastFiredIndex == size (Finished)
//
// 発火順序:
//   AddKeyframe で time_sec 昇順を維持しているので、Tick / Skip では
//   m_LastFiredIndex から線形に進めるだけで自然に時刻順発火になる。
//   同時刻 (= 同じ time_sec) の keyframe は登録順に発火する (stable insertion)。
#include "gameframework/CinematicsDirector.h"

namespace acs::game {

void FCinematicsDirector::AddKeyframe(const FTimelineKeyframe& kf) noexcept {
    // 負の time_sec は 0 に clamp して受け入れる (= タイムライン頭打ち発火)。
    FTimelineKeyframe entry = kf;
    if (entry.time_sec < 0.0f) entry.time_sec = 0.0f;

    // time_sec 昇順 / 同時刻は登録順 (stable) を維持する挿入位置を線形探索。
    // 典型 N < 100 なので O(N) で実用上問題なし。strict less-than 比較で
    // 「同時刻のものは後ろ」になり stable insertion になる。
    const usize n = m_Keyframes.Size();
    usize insert_at = n;
    for (usize i = 0; i < n; ++i) {
        if (entry.time_sec < m_Keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }

    if (insert_at == n) {
        // 末尾に追加 (時刻が現在の最大以上)
        m_Keyframes.PushBack(entry);
    } else {
        // [insert_at..n-1] を 1 つ後ろにずらして空きを作る。
        // 末尾要素を必ずローカルへ退避してから PushBack する。PushBack(m_Keyframes[n-1])
        // と書くと、Grow 再確保で旧バッファが Free された後に引数参照を読み UAF になる。
        const FTimelineKeyframe tail = m_Keyframes[n - 1];
        m_Keyframes.PushBack(tail);
        for (usize i = n - 1; i > insert_at; --i) {
            m_Keyframes[i] = m_Keyframes[i - 1];
        }
        m_Keyframes[insert_at] = entry;
    }

    // 挿入位置が m_LastFiredIndex 以下だと「過去 keyframe が増えた」状態に
    // なるが、再生中の場合は時間がそこを既に過ぎているため次 Tick で即発火
    // される。設計上は「キーフレーム追加は基本セットアップ段階のみ行う」前提
    // なので m_LastFiredIndex の補正はしない (Play() 前なら m_LastFired=0)。
}

void FCinematicsDirector::Clear() noexcept {
    m_Keyframes.Clear();
    m_Time             = 0.0f;
    m_LastFiredIndex = 0u;
    m_Playing          = false;
}

void FCinematicsDirector::Play() noexcept {
    m_Playing = true;
}

void FCinematicsDirector::Pause() noexcept {
    m_Playing = false;
}

void FCinematicsDirector::Stop() noexcept {
    m_Playing          = false;
    m_Time             = 0.0f;
    m_LastFiredIndex = 0u;
}

void FCinematicsDirector::Skip() noexcept {
    // 残り全 keyframe を時刻昇順に発火。m_Time は TotalDuration() に進める。
    // Skip は Play 中でなくても呼べる (= 即座に "終わった状態" に飛ばす操作)。
    const f32 total = TotalDuration();
    FireUpTo(total);
    m_Time             = total;
    m_LastFiredIndex = static_cast<u32>(m_Keyframes.Size());
    // m_Playing は維持 (= caller が Stop / Pause を別途呼ぶ責務)。Skip 後に
    // IsFinished() が true になるので、Tick が呼ばれ続けても無害。
}

bool FCinematicsDirector::IsFinished() const noexcept {
    return m_LastFiredIndex >= static_cast<u32>(m_Keyframes.Size());
}

void FCinematicsDirector::Tick(f32 dt) noexcept {
    if (!m_Playing) return;
    if (dt <= 0.0f) return;

    m_Time += dt;
    FireUpTo(m_Time);
}

f32 FCinematicsDirector::TotalDuration() const noexcept {
    // 昇順を維持しているので末尾 (Back) の time_sec が最大。
    if (m_Keyframes.Size() == 0) return 0.0f;
    return m_Keyframes[m_Keyframes.Size() - 1].time_sec;
}

void FCinematicsDirector::SetCameraCallback(CameraCallbackFn cb, void* user) noexcept {
    m_CameraCb   = cb;
    m_CameraUser = user;
}

void FCinematicsDirector::SetDialogueCallback(DialogueCallbackFn cb, void* user) noexcept {
    m_DialogueCb   = cb;
    m_DialogueUser = user;
}

void FCinematicsDirector::SetMusicCallback(MusicCallbackFn cb, void* user) noexcept {
    m_MusicCb   = cb;
    m_MusicUser = user;
}

void FCinematicsDirector::SetEventCallback(EventCallbackFn cb, void* user) noexcept {
    m_EventCb   = cb;
    m_EventUser = user;
}

void FCinematicsDirector::FireUpTo(f32 up_to_time) noexcept {
    const u32 n = static_cast<u32>(m_Keyframes.Size());
    while (m_LastFiredIndex < n) {
        const FTimelineKeyframe& kf = m_Keyframes[m_LastFiredIndex];
        if (kf.time_sec > up_to_time) break;   // 未来の keyframe はまだ発火しない
        FireOne(kf);
        ++m_LastFiredIndex;
    }
}

void FCinematicsDirector::FireOne(const FTimelineKeyframe& kf) noexcept {
    switch (kf.kind) {
    case ETimelineTrackKind::Wait:
        // 何もしない (時間進行マーカー)。
        break;

    case ETimelineTrackKind::MoveCamera:
        if (m_CameraCb != nullptr) {
            m_CameraCb(m_CameraUser,
                       kf.payload.camera.target_pos,
                       kf.payload.camera.zoom,
                       kf.payload.camera.duration);
        }
        break;

    case ETimelineTrackKind::ShowDialogue:
        if (m_DialogueCb != nullptr) {
            m_DialogueCb(m_DialogueUser, kf.payload.dialogue.line_id);
        }
        break;

    case ETimelineTrackKind::PlayMusic:
        if (m_MusicCb != nullptr) {
            m_MusicCb(m_MusicUser,
                      kf.payload.music.music_id,
                      kf.payload.music.fade);
        }
        break;

    case ETimelineTrackKind::FireEvent:
        if (m_EventCb != nullptr) {
            m_EventCb(m_EventUser, kf.payload.event.event_id);
        }
        break;
    }
}

} // namespace acs::game
