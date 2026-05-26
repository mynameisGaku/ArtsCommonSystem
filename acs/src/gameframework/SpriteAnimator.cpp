// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FSpriteAnimator 実装 (Phase 3 完結)
//
// ・時間→frame index 計算: floor(_elapsed * _fps)
// ・Loop:     N で mod
// ・PingPong: 2*(N-1) で mod した値を、後半は折り返す
// ・Once:     N-1 にクランプし _finished を立てる
//
// _elapsed は Loop/PingPong では周期長を超えたら巻き戻し、Once では「末尾 frame の
// 終端時刻」を超えないようクランプ。長時間プレイでも f32 精度ロスを防ぐ。
#include "gameframework/SpriteAnimator.h"

namespace acs::game {

void FSpriteAnimator::Init(u32 frame_count, f32 fps, EPlayMode mode) noexcept {
    _frame_count   = frame_count == 0u ? 1u : frame_count;
    _fps           = fps > 0.0f ? fps : 1.0f;
    _mode          = mode;
    _elapsed       = 0.0f;
    _current_frame = 0;
    _playing       = false;
    _finished      = false;
}

void FSpriteAnimator::Play() noexcept {
    // Once が末尾で終わっているときに Play() されたら先頭に巻き戻して再生
    // (= 「もう一回」を Play 単独で実現する一般的な期待挙動)
    if (_finished && _mode == EPlayMode::Once) {
        _elapsed       = 0.0f;
        _current_frame = 0;
        _finished      = false;
    }
    _playing = true;
}

void FSpriteAnimator::Pause() noexcept {
    _playing = false;
}

void FSpriteAnimator::Stop() noexcept {
    _playing       = false;
    _elapsed       = 0.0f;
    _current_frame = 0;
    _finished      = false;
}

u32 FSpriteAnimator::ComputeFrame(f32 elapsed) const noexcept {
    // 1 frame の表示時間 (sec/frame)
    const f32 t = elapsed * _fps;          // = 経過 frame 数 (連続値)
    const f32 f = Floor(t);                // = 経過 frame 数 (整数)
    const u32 raw = static_cast<u32>(f < 0.0f ? 0.0f : f);

    if (_frame_count <= 1u) return 0;

    switch (_mode) {
    case EPlayMode::Loop: {
        return raw % _frame_count;
    }
    case EPlayMode::PingPong: {
        // 周期長: 0→N-1→0 で合計 2*(N-1) frame
        const u32 period = (_frame_count - 1u) * 2u;
        const u32 m      = raw % period;
        // 前半 [0, N-1] はそのまま、後半 [N, 2N-2] は折り返す
        return m < _frame_count ? m : (period - m);
    }
    case EPlayMode::Once: {
        return raw >= _frame_count ? (_frame_count - 1u) : raw;
    }
    }
    return 0;
}

void FSpriteAnimator::Tick(f32 dt) noexcept {
    if (!_playing || dt <= 0.0f) return;
    if (_fps <= 0.0f)            return; // 0 fps はフリーズ
    if (_frame_count == 0u)      return;

    const u32 prev = _current_frame;
    _elapsed += dt;

    // _elapsed を周期内に丸めて f32 精度ロスを防ぐ
    bool wrapped = false;
    if (_mode == EPlayMode::Loop && _frame_count > 0u) {
        const f32 period_sec = static_cast<f32>(_frame_count) / _fps;
        if (period_sec > 0.0f) {
            while (_elapsed >= period_sec) {
                _elapsed -= period_sec;
                wrapped = true;
            }
        }
    } else if (_mode == EPlayMode::PingPong && _frame_count > 1u) {
        const f32 period_sec = static_cast<f32>((_frame_count - 1u) * 2u) / _fps;
        if (period_sec > 0.0f) {
            while (_elapsed >= period_sec) {
                _elapsed -= period_sec;
                wrapped = true;
            }
        }
    } else if (_mode == EPlayMode::Once) {
        // 末尾 frame の終端時刻 = frame_count / fps でクランプ
        const f32 end_sec = static_cast<f32>(_frame_count) / _fps;
        if (_elapsed >= end_sec) {
            _elapsed  = end_sec;
            _finished = true;
            _playing  = false; // 自動停止 (= IsPlaying() == false)
        }
    }

    const u32 next = ComputeFrame(_elapsed);
    if (next != prev || wrapped) {
        FireEventsBetween(prev, next, wrapped);
        _current_frame = next;
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
    const u32 n = static_cast<u32>(_events.Size());
    for (u32 i = 0; i < n; ++i) {
        const FFrameEvent& e = _events[i];
        if (e.cb != nullptr && e.frame == next) {
            e.cb(e.user);
        }
    }
}

f32 FSpriteAnimator::NormalizedTime() const noexcept {
    if (_frame_count == 0u || _fps <= 0.0f) return 0.0f;

    switch (_mode) {
    case EPlayMode::Loop: {
        const f32 period_sec = static_cast<f32>(_frame_count) / _fps;
        if (period_sec <= 0.0f) return 0.0f;
        return Saturate(_elapsed / period_sec);
    }
    case EPlayMode::PingPong: {
        if (_frame_count <= 1u) return 0.0f;
        const f32 period_sec = static_cast<f32>((_frame_count - 1u) * 2u) / _fps;
        if (period_sec <= 0.0f) return 0.0f;
        return Saturate(_elapsed / period_sec);
    }
    case EPlayMode::Once: {
        const f32 end_sec = static_cast<f32>(_frame_count) / _fps;
        if (end_sec <= 0.0f) return 0.0f;
        return Saturate(_elapsed / end_sec);
    }
    }
    return 0.0f;
}

void FSpriteAnimator::SetCurrentFrame(u32 i) noexcept {
    if (_frame_count == 0u) return;
    const u32 idx = i >= _frame_count ? (_frame_count - 1u) : i;
    _current_frame = idx;
    // _elapsed を idx の先頭時刻に合わせる (= 同 frame 内のサブ位置はリセット)
    _elapsed = _fps > 0.0f ? (static_cast<f32>(idx) / _fps) : 0.0f;
    // Once で末尾にシークした場合は finished にしない (Tick で初めて確定)
    _finished = false;
}

void FSpriteAnimator::SetFps(f32 fps) noexcept {
    _fps = fps > 0.0f ? fps : 0.0f;
}

void FSpriteAnimator::AddFrameEvent(u32 frame, FrameEventFn cb, void* user) noexcept {
    if (cb == nullptr)         return;
    if (frame >= _frame_count) return; // 範囲外は黙って無視
    _events.PushBack(FFrameEvent{frame, cb, user});
}

} // namespace acs::game
