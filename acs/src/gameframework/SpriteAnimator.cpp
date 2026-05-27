// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FSpriteAnimator 実装 (Phase 3 完結)
//
// ・時間→frame index 計算: floor(m_Elapsed * m_Fps)
// ・Loop:     N で mod
// ・PingPong: 2*(N-1) で mod した値を、後半は折り返す
// ・Once:     N-1 にクランプし m_Finished を立てる
//
// m_Elapsed は Loop/PingPong では周期長を超えたら巻き戻し、Once では「末尾 frame の
// 終端時刻」を超えないようクランプ。長時間プレイでも f32 精度ロスを防ぐ。
#include "gameframework/SpriteAnimator.h"

namespace acs::game {

void FSpriteAnimator::Init(u32 frame_count, f32 fps, EPlayMode mode) noexcept {
    m_FrameCount   = frame_count == 0u ? 1u : frame_count;
    m_Fps           = fps > 0.0f ? fps : 1.0f;
    m_Mode          = mode;
    m_Elapsed       = 0.0f;
    m_CurrentFrame = 0;
    m_Playing       = false;
    m_Finished      = false;
}

void FSpriteAnimator::Play() noexcept {
    // Once が末尾で終わっているときに Play() されたら先頭に巻き戻して再生
    // (= 「もう一回」を Play 単独で実現する一般的な期待挙動)
    if (m_Finished && m_Mode == EPlayMode::Once) {
        m_Elapsed       = 0.0f;
        m_CurrentFrame = 0;
        m_Finished      = false;
    }
    m_Playing = true;
}

void FSpriteAnimator::Pause() noexcept {
    m_Playing = false;
}

void FSpriteAnimator::Stop() noexcept {
    m_Playing       = false;
    m_Elapsed       = 0.0f;
    m_CurrentFrame = 0;
    m_Finished      = false;
}

u32 FSpriteAnimator::ComputeFrame(f32 elapsed) const noexcept {
    // 1 frame の表示時間 (sec/frame)
    const f32 t = elapsed * m_Fps;          // = 経過 frame 数 (連続値)
    const f32 f = Floor(t);                // = 経過 frame 数 (整数)
    const u32 raw = static_cast<u32>(f < 0.0f ? 0.0f : f);

    if (m_FrameCount <= 1u) return 0;

    switch (m_Mode) {
    case EPlayMode::Loop: {
        return raw % m_FrameCount;
    }
    case EPlayMode::PingPong: {
        // 周期長: 0→N-1→0 で合計 2*(N-1) frame
        const u32 period = (m_FrameCount - 1u) * 2u;
        const u32 m      = raw % period;
        // 前半 [0, N-1] はそのまま、後半 [N, 2N-2] は折り返す
        return m < m_FrameCount ? m : (period - m);
    }
    case EPlayMode::Once: {
        return raw >= m_FrameCount ? (m_FrameCount - 1u) : raw;
    }
    }
    return 0;
}

void FSpriteAnimator::Tick(f32 dt) noexcept {
    if (!m_Playing || dt <= 0.0f) return;
    if (m_Fps <= 0.0f)            return; // 0 fps はフリーズ
    if (m_FrameCount == 0u)      return;

    const u32 prev = m_CurrentFrame;
    m_Elapsed += dt;

    // m_Elapsed を周期内に丸めて f32 精度ロスを防ぐ
    bool wrapped = false;
    if (m_Mode == EPlayMode::Loop && m_FrameCount > 0u) {
        const f32 period_sec = static_cast<f32>(m_FrameCount) / m_Fps;
        if (period_sec > 0.0f) {
            while (m_Elapsed >= period_sec) {
                m_Elapsed -= period_sec;
                wrapped = true;
            }
        }
    } else if (m_Mode == EPlayMode::PingPong && m_FrameCount > 1u) {
        const f32 period_sec = static_cast<f32>((m_FrameCount - 1u) * 2u) / m_Fps;
        if (period_sec > 0.0f) {
            while (m_Elapsed >= period_sec) {
                m_Elapsed -= period_sec;
                wrapped = true;
            }
        }
    } else if (m_Mode == EPlayMode::Once) {
        // 末尾 frame の終端時刻 = frame_count / fps でクランプ
        const f32 end_sec = static_cast<f32>(m_FrameCount) / m_Fps;
        if (m_Elapsed >= end_sec) {
            m_Elapsed  = end_sec;
            m_Finished = true;
            m_Playing  = false; // 自動停止 (= IsPlaying() == false)
        }
    }

    const u32 next = ComputeFrame(m_Elapsed);
    if (next != prev || wrapped) {
        FireEventsBetween(prev, next, wrapped);
        m_CurrentFrame = next;
    }
}

void FSpriteAnimator::FireEventsBetween(u32 prev, u32 next, bool wrapped) noexcept {
    // 同 frame で wrap (= 1 周期内に止まる超低 fps & 高 dt) を考慮。
    // Loop:     prev<next なら (prev, next] が新規進入、wrapped なら間の周回も発火。
    // PingPong: 端で折り返すので「次の frame に進入した」だけ判定すれば足る。
    // Once:     prev<next の単調増加のみ。
    // 簡潔さ優先で「next と一致する frame の event を全部発火」する。
    // (= 同 frame に止まり続けるケースでは Tick 内で next != prev のときだけ
    // ここに来るので二重発火しない)
    (void)prev;
    (void)wrapped;
    const u32 n = static_cast<u32>(m_Events.Size());
    for (u32 i = 0; i < n; ++i) {
        const FrameEvent& e = m_Events[i];
        if (e.cb != nullptr && e.frame == next) {
            e.cb(e.user);
        }
    }
}

f32 FSpriteAnimator::NormalizedTime() const noexcept {
    if (m_FrameCount == 0u || m_Fps <= 0.0f) return 0.0f;

    switch (m_Mode) {
    case EPlayMode::Loop: {
        const f32 period_sec = static_cast<f32>(m_FrameCount) / m_Fps;
        if (period_sec <= 0.0f) return 0.0f;
        return Saturate(m_Elapsed / period_sec);
    }
    case EPlayMode::PingPong: {
        if (m_FrameCount <= 1u) return 0.0f;
        const f32 period_sec = static_cast<f32>((m_FrameCount - 1u) * 2u) / m_Fps;
        if (period_sec <= 0.0f) return 0.0f;
        return Saturate(m_Elapsed / period_sec);
    }
    case EPlayMode::Once: {
        const f32 end_sec = static_cast<f32>(m_FrameCount) / m_Fps;
        if (end_sec <= 0.0f) return 0.0f;
        return Saturate(m_Elapsed / end_sec);
    }
    }
    return 0.0f;
}

void FSpriteAnimator::SetCurrentFrame(u32 i) noexcept {
    if (m_FrameCount == 0u) return;
    const u32 idx = i >= m_FrameCount ? (m_FrameCount - 1u) : i;
    m_CurrentFrame = idx;
    // m_Elapsed を idx の先頭時刻に合わせる (= 同 frame 内のサブ位置はリセット)
    m_Elapsed = m_Fps > 0.0f ? (static_cast<f32>(idx) / m_Fps) : 0.0f;
    // Once で末尾にシークした場合は finished にしない (Tick で初めて確定)
    m_Finished = false;
}

void FSpriteAnimator::SetFps(f32 fps) noexcept {
    m_Fps = fps > 0.0f ? fps : 0.0f;
}

void FSpriteAnimator::AddFrameEvent(u32 frame, FrameEventFn cb, void* user) noexcept {
    if (cb == nullptr)         return;
    if (frame >= m_FrameCount) return; // 範囲外は黙って無視
    m_Events.PushBack(FrameEvent{frame, cb, user});
}

} // namespace acs::game
