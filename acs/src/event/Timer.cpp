// SPDX-License-Identifier: Apache-2.0
// FTimerManager 実装
#include "event/Timer.h"

namespace acs {

FTimerHandle FTimerManager::SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || delay_seconds < 0.0f) return kInvalidTimer;

    u32 idx;
    if (_free_indices.Size() > 0) {
        idx = _free_indices[_free_indices.Size() - 1];
        _free_indices.PopBack();
    } else {
        idx = static_cast<u32>(_slots.Size());
        _slots.PushBack(Slot{});
    }

    Slot& s = _slots[idx];
    if (s.id == 0) s.id = _next_id++;
    s.generation++;        // 再利用の度に generation を進める
    s.active    = true;
    s.repeating = false;
    s.remaining = delay_seconds;
    s.period    = 0.0f;
    s.cb        = cb;
    s.user      = user;
    return FTimerHandle{ s.id, s.generation };
}

FTimerHandle FTimerManager::SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || period_seconds <= 0.0f) return kInvalidTimer;

    u32 idx;
    if (_free_indices.Size() > 0) {
        idx = _free_indices[_free_indices.Size() - 1];
        _free_indices.PopBack();
    } else {
        idx = static_cast<u32>(_slots.Size());
        _slots.PushBack(Slot{});
    }

    Slot& s = _slots[idx];
    if (s.id == 0) s.id = _next_id++;
    s.generation++;
    s.active    = true;
    s.repeating = true;
    s.remaining = period_seconds;
    s.period    = period_seconds;
    s.cb        = cb;
    s.user      = user;
    return FTimerHandle{ s.id, s.generation };
}

bool FTimerManager::Cancel(FTimerHandle h) noexcept {
    if (!h.IsValid()) return false;
    // id は 1-based、_slots の中を線形検索（タイマ数は通常少ないので OK）
    for (usize i = 0; i < _slots.Size(); ++i) {
        Slot& s = _slots[i];
        if (s.id == h.id && s.generation == h.generation && s.active) {
            s.active = false;
            s.cb     = nullptr;
            s.user   = nullptr;
            _free_indices.PushBack(static_cast<u32>(i));
            return true;
        }
    }
    return false;
}

void FTimerManager::Tick(f32 dt) noexcept {
    // コールバック中にタイマを追加 / Cancel される可能性があるので、
    // ループ中の _slots.Size() を毎回読み直す。新規追加は末尾のため安全。
    // 既存スロットを active=false にされても次のループで checked される。
    const usize initial_count = _slots.Size();
    for (usize i = 0; i < initial_count; ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;
        s.remaining -= dt;
        if (s.remaining > 0.0f) continue;

        // 発火 — コールバック中に Cancel される可能性あり
        TimerCallback cb   = s.cb;
        void*         user = s.user;
        bool          repeating = s.repeating;
        f32           period    = s.period;
        if (!repeating) {
            // 1 回限りはコールバック呼ぶ前にスロット解放
            // (コールバック中に同じハンドルを Cancel しても false が返るだけで安全)
            s.active = false;
            s.cb     = nullptr;
            s.user   = nullptr;
            _free_indices.PushBack(static_cast<u32>(i));
        } else {
            // 周期は次の発火までセット (drift しないよう period 加算、複数回分追いつき)
            s.remaining += period;
            if (s.remaining < 0.0f) s.remaining = period;  // 大きく overshoot した場合の保護
        }

        cb(user);
    }
}

u32 FTimerManager::ActiveCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < _slots.Size(); ++i) if (_slots[i].active) ++n;
    return n;
}

} // namespace acs
