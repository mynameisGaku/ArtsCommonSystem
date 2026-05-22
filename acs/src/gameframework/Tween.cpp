// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Tween / TweenManager 実装 (Phase 3)
#include "gameframework/Tween.h"

namespace acs::game {

u32 TweenManager::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用 (= 一定 Tick 後の Tween 群はキャッシュ局所性高い)
    for (u32 i = 0; i < _slots.Size(); ++i) {
        if (!_slots[i].active) {
            return i;
        }
    }
    // 全 slot 使用中 → 末尾に追加
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

void TweenManager::FillCommon(Slot& s, void* target, f32 duration,
                               Easing::EasingFn ease) noexcept {
    s.target   = target;
    s.elapsed  = 0.0f;
    s.duration = duration;
    s.ease     = ease != nullptr ? ease : Easing::Linear;
    s.active   = true;
    // generation を 1 進める (0 = invalid なので必ず非 0 を保つ)
    s.generation = s.generation + 1u;
    if (s.generation == 0u) s.generation = 1u;
}

TweenHandle TweenManager::Tween(f32* target, f32 from, f32 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.kind   = Kind::F32;
    s.from_f = from;
    s.to_f   = to;
    FillCommon(s, target, duration, ease);
    ++_active_count;
    return TweenHandle{idx, s.generation};
}

TweenHandle TweenManager::Tween(Vec2* target, Vec2 from, Vec2 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.kind    = Kind::Vec2;
    s.from_v2 = from;
    s.to_v2   = to;
    FillCommon(s, target, duration, ease);
    ++_active_count;
    return TweenHandle{idx, s.generation};
}

TweenHandle TweenManager::Tween(Vec3* target, Vec3 from, Vec3 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    s.kind    = Kind::Vec3;
    s.from_v3 = from;
    s.to_v3   = to;
    FillCommon(s, target, duration, ease);
    ++_active_count;
    return TweenHandle{idx, s.generation};
}

void TweenManager::Cancel(TweenHandle h) noexcept {
    if (!h.IsValid() || h.index >= _slots.Size()) return;
    Slot& s = _slots[h.index];
    if (s.generation != h.generation || !s.active) return;
    s.active = false;
    s.kind   = Kind::None;
    s.target = nullptr;
    if (_active_count > 0) --_active_count;
}

void TweenManager::CompleteAll() noexcept {
    for (u32 i = 0; i < _slots.Size(); ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;
        switch (s.kind) {
        case Kind::F32:  *static_cast<f32*>(s.target)  = s.to_f;  break;
        case Kind::Vec2: *static_cast<Vec2*>(s.target) = s.to_v2; break;
        case Kind::Vec3: *static_cast<Vec3*>(s.target) = s.to_v3; break;
        default: break;
        }
        s.active = false;
        s.kind   = Kind::None;
        s.target = nullptr;
    }
    _active_count = 0;
}

void TweenManager::CancelAll() noexcept {
    for (u32 i = 0; i < _slots.Size(); ++i) {
        _slots[i].active = false;
        _slots[i].kind   = Kind::None;
        _slots[i].target = nullptr;
    }
    _active_count = 0;
}

bool TweenManager::IsActive(TweenHandle h) const noexcept {
    if (!h.IsValid() || h.index >= _slots.Size()) return false;
    const Slot& s = _slots[h.index];
    return s.active && s.generation == h.generation;
}

u32 TweenManager::ActiveCount() const noexcept {
    return _active_count;
}

void TweenManager::Tick(f32 dt) noexcept {
    if (_active_count == 0 || dt <= 0.0f) return;
    for (u32 i = 0; i < _slots.Size(); ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;

        s.elapsed += dt;
        f32 t = s.elapsed / s.duration;
        const bool finished = (t >= 1.0f);
        if (finished) t = 1.0f;
        const f32 e = s.ease(t);

        switch (s.kind) {
        case Kind::F32: {
            f32* p = static_cast<f32*>(s.target);
            *p = finished ? s.to_f
                          : s.from_f + (s.to_f - s.from_f) * e;
            break;
        }
        case Kind::Vec2: {
            Vec2* p = static_cast<Vec2*>(s.target);
            if (finished) {
                *p = s.to_v2;
            } else {
                p->x = s.from_v2.x + (s.to_v2.x - s.from_v2.x) * e;
                p->y = s.from_v2.y + (s.to_v2.y - s.from_v2.y) * e;
            }
            break;
        }
        case Kind::Vec3: {
            Vec3* p = static_cast<Vec3*>(s.target);
            if (finished) {
                *p = s.to_v3;
            } else {
                p->x = s.from_v3.x + (s.to_v3.x - s.from_v3.x) * e;
                p->y = s.from_v3.y + (s.to_v3.y - s.from_v3.y) * e;
                p->z = s.from_v3.z + (s.to_v3.z - s.from_v3.z) * e;
            }
            break;
        }
        default: break;
        }

        if (finished) {
            s.active = false;
            s.kind   = Kind::None;
            s.target = nullptr;
            if (_active_count > 0) --_active_count;
        }
    }
}

} // namespace acs::game
