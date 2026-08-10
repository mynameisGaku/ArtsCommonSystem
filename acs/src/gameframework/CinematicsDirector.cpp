// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar R — CCinematicsDirector 実装
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

#include <cmath>

namespace acs::game {

void CCinematicsDirector::AddKeyframe(const FTimelineKeyframe& kf) noexcept {
    (void)TryAddKeyframe(kf);
}

bool CCinematicsDirector::TryAddKeyframe(const FTimelineKeyframe& kf) noexcept {
    if (!std::isfinite(kf.time_sec) || m_Playing || m_Time != 0.0f || m_LastFiredIndex != 0u) {
        return false;
    }

    // 負の time_sec は 0 に clamp して受け入れる (= タイムライン頭打ち発火)。
    FTimelineKeyframe entry = kf;
    if (entry.time_sec < 0.0f) entry.time_sec = 0.0f;

    // time_sec 昇順 / 同時刻は登録順 (stable) を維持する挿入位置を線形探索。
    // 典型 N < 100 なので O(N) で実用上問題なし。strict less-than 比較で
    // 「同時刻のものは後ろ」になり stable insertion になる。
    const usize n = m_Keyframes.Num();
    usize insert_at = n;
    for (usize i = 0; i < n; ++i) {
        if (entry.time_sec < m_Keyframes[i].time_sec) {
            insert_at = i;
            break;
        }
    }

    if (insert_at == n) {
        // 末尾に追加 (時刻が現在の最大以上)
        return m_Keyframes.TryAdd(entry);
    } else {
        // [insert_at..n-1] を 1 つ後ろにずらして空きを作る。
        // 末尾要素をローカルへ退避してから TryAdd する。TArray は再確保前に
        // 追加元をコピーするため、配列内要素を参照したままでも値が保たれる。
        const FTimelineKeyframe tail = m_Keyframes[n - 1];
        if (!m_Keyframes.TryAdd(tail)) return false;
        for (usize i = n - 1; i > insert_at; --i) {
            m_Keyframes[i] = m_Keyframes[i - 1];
        }
        m_Keyframes[insert_at] = entry;
    }

    // 初期停止状態だけを受け付けるため、登録後も発火位置は 0 のまま保たれる。
    return true;
}

void CCinematicsDirector::Clear() noexcept {
    m_Keyframes.Reset();
    m_Time             = 0.0f;
    m_LastFiredIndex = 0u;
    m_Playing          = false;
}

void CCinematicsDirector::Play() noexcept {
    m_Playing = true;
}

void CCinematicsDirector::Pause() noexcept {
    m_Playing = false;
}

void CCinematicsDirector::Stop() noexcept {
    m_Playing          = false;
    m_Time             = 0.0f;
    m_LastFiredIndex = 0u;
}

void CCinematicsDirector::Skip() noexcept {
    // 残り全 keyframe を時刻昇順に発火。m_Time は TotalDuration() に進める。
    // Skip は Play 中でなくても呼べる (= 即座に "終わった状態" に飛ばす操作)。
    const f32 total = TotalDuration();
    FireUpTo(total);
    m_Time             = total;
    m_LastFiredIndex = static_cast<u32>(m_Keyframes.Num());
    // m_Playing は維持 (= caller が Stop / Pause を別途呼ぶ責務)。Skip 後に
    // IsFinished() が true になるので、Tick が呼ばれ続けても無害。
}

bool CCinematicsDirector::IsFinished() const noexcept {
    return m_LastFiredIndex >= static_cast<u32>(m_Keyframes.Num());
}

void CCinematicsDirector::Tick(f32 dt) noexcept {
    if (!m_Playing) return;
    if (!std::isfinite(dt) || dt <= 0.0f) return;

    const f32 next_time = m_Time + dt;
    if (!std::isfinite(next_time)) return;
    m_Time = next_time;
    FireUpTo(m_Time);
}

f32 CCinematicsDirector::TotalDuration() const noexcept {
    // 昇順を維持しているので末尾 (Back) の time_sec が最大。
    if (m_Keyframes.Num() == 0) return 0.0f;
    return m_Keyframes[m_Keyframes.Num() - 1].time_sec;
}

void CCinematicsDirector::SetCameraCallback(CameraCallbackFn cb, void* user) noexcept {
    m_CameraCb   = cb;
    m_CameraUser = user;
}

void CCinematicsDirector::SetDialogueCallback(DialogueCallbackFn cb, void* user) noexcept {
    m_DialogueCb   = cb;
    m_DialogueUser = user;
}

void CCinematicsDirector::SetMusicCallback(MusicCallbackFn cb, void* user) noexcept {
    m_MusicCb   = cb;
    m_MusicUser = user;
}

void CCinematicsDirector::SetEventCallback(EventCallbackFn cb, void* user) noexcept {
    m_EventCb   = cb;
    m_EventUser = user;
}

void CCinematicsDirector::FireUpTo(f32 up_to_time) noexcept {
    // 発火予定の keyframe を «値で» 退避してから、走査を終えた後にまとめて発火する。
    // keyframe callback が同 director の AddKeyframe / Clear / Stop を呼んで
    // m_Keyframes を変更 (再確保・空化) しても、走査中の範囲外アクセスや realloc による
    // dangling を起こさないようにする (BuffSystem::Tick と同じ「配列操作を済ませてから
    // 発火」規約)。退避は値コピー — payload は POD union なので所有参照を持たない。
    // 走査ループ中は callback を呼ばないので、m_Keyframes は不変で index は常に in-bounds。
    TArray<FTimelineKeyframe> to_fire;
    while (m_LastFiredIndex < static_cast<u32>(m_Keyframes.Num())) {
        const FTimelineKeyframe& kf = m_Keyframes[m_LastFiredIndex];
        if (kf.time_sec > up_to_time) break;   // 未来の keyframe はまだ発火しない
        (void)to_fire.TryAdd(kf);
        ++m_LastFiredIndex;
    }
    for (usize i = 0; i < to_fire.Num(); ++i) {
        FireOne(to_fire[i]);
    }
}

void CCinematicsDirector::FireOne(const FTimelineKeyframe& kf) noexcept {
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
