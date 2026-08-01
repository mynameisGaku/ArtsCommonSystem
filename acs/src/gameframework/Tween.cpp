// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — FTween / CTweenManager 実装
#include "gameframework/Tween.h"

#include <cmath>

namespace acs::game {

namespace {

bool IsFiniteTweenValue(f32 value) noexcept {
    return std::isfinite(value);
}

bool IsFiniteTweenValue(FVec2 value) noexcept {
    return IsFiniteTweenValue(value.x) && IsFiniteTweenValue(value.y);
}

bool IsFiniteTweenValue(FVec3 value) noexcept {
    return IsFiniteTweenValue(value.x)
        && IsFiniteTweenValue(value.y)
        && IsFiniteTweenValue(value.z);
}

} // namespace

u32 CTweenManager::AcquireSlot() noexcept {
    // 既存の inactive slot を再利用 (= 一定 Tick 後の FTween 群はキャッシュ局所性高い)
    for (u32 i = 0; i < m_Slots.Size(); ++i) {
        if (!m_Slots[i].active) {
            return i;
        }
    }
    // 全 slot 使用中 → 末尾に追加
    m_Slots.PushBack({});
    return static_cast<u32>(m_Slots.Size()) - 1u;
}

void CTweenManager::FillCommon(FSlot& s, void* target, f32 duration,
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

FTweenHandle CTweenManager::Tween(f32* target, f32 from, f32 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return {};
    }
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind   = EKind::F32;
    s.from_f = from;
    s.to_f   = to;
    FillCommon(s, target, duration, ease);
    ++m_ActiveCount;
    return FTweenHandle{idx, s.generation};
}

FTweenHandle CTweenManager::Tween(f32* target, f32 from, f32 to, f32 duration,
                                 Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

FTweenHandle CTweenManager::Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return {};
    }
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind    = EKind::Vec2;
    s.from_v2 = from;
    s.to_v2   = to;
    FillCommon(s, target, duration, ease);
    ++m_ActiveCount;
    return FTweenHandle{idx, s.generation};
}

FTweenHandle CTweenManager::Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                                 Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

FTweenHandle CTweenManager::Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                                 Easing::EasingFn ease) noexcept {
    if (target == nullptr) return {};
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return {};
    }
    if (duration <= 0.0f) {
        *target = to;
        return {};
    }
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    s.kind    = EKind::Vec3;
    s.from_v3 = from;
    s.to_v3   = to;
    FillCommon(s, target, duration, ease);
    ++m_ActiveCount;
    return FTweenHandle{idx, s.generation};
}

FTweenHandle CTweenManager::Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                                 Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

void CTweenManager::Cancel(FTweenHandle h) noexcept {
    if (!h.IsValid() || h.index >= m_Slots.Size()) return;
    FSlot& s = m_Slots[h.index];
    if (s.generation != h.generation || !s.active) return;
    s.active = false;
    s.kind   = EKind::None;
    s.target = nullptr;
    if (m_ActiveCount > 0) --m_ActiveCount;
}

void CTweenManager::CompleteAll() noexcept {
    for (u32 i = 0; i < m_Slots.Size(); ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active) continue;
        switch (s.kind) {
        case EKind::F32:  *static_cast<f32*>(s.target)  = s.to_f;  break;
        case EKind::Vec2: *static_cast<FVec2*>(s.target) = s.to_v2; break;
        case EKind::Vec3: *static_cast<FVec3*>(s.target) = s.to_v3; break;
        default: break;
        }
        s.active = false;
        s.kind   = EKind::None;
        s.target = nullptr;
    }
    m_ActiveCount = 0;
}

void CTweenManager::CancelAll() noexcept {
    for (u32 i = 0; i < m_Slots.Size(); ++i) {
        m_Slots[i].active = false;
        m_Slots[i].kind   = EKind::None;
        m_Slots[i].target = nullptr;
    }
    m_ActiveCount = 0;
}

bool CTweenManager::IsActive(FTweenHandle h) const noexcept {
    if (!h.IsValid() || h.index >= m_Slots.Size()) return false;
    const FSlot& s = m_Slots[h.index];
    return s.active && s.generation == h.generation;
}

u32 CTweenManager::ActiveCount() const noexcept {
    return m_ActiveCount;
}

void CTweenManager::Tick(f32 dt) noexcept {
    if (m_ActiveCount == 0 || dt <= 0.0f || !std::isfinite(dt)) return;
    for (u32 i = 0; i < m_Slots.Size(); ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active) continue;

        s.elapsed += dt;
        if (!std::isfinite(s.elapsed)) s.elapsed = s.duration;
        f32 t = s.elapsed / s.duration;
        const bool finished = (t >= 1.0f);
        if (finished) t = 1.0f;
        const f32 eased = s.ease(t);
        const f32 e = std::isfinite(eased) ? eased : t;

        switch (s.kind) {
        case EKind::F32: {
            f32* p = static_cast<f32*>(s.target);
            if (finished) {
                *p = s.to_f;
            } else {
                const f32 staged =
                    s.from_f + (s.to_f - s.from_f) * e;
                if (IsFiniteTweenValue(staged)) *p = staged;
            }
            break;
        }
        case EKind::Vec2: {
            FVec2* p = static_cast<FVec2*>(s.target);
            if (finished) {
                *p = s.to_v2;
            } else {
                const FVec2 staged{
                    s.from_v2.x + (s.to_v2.x - s.from_v2.x) * e,
                    s.from_v2.y + (s.to_v2.y - s.from_v2.y) * e,
                };
                if (IsFiniteTweenValue(staged)) *p = staged;
            }
            break;
        }
        case EKind::Vec3: {
            FVec3* p = static_cast<FVec3*>(s.target);
            if (finished) {
                *p = s.to_v3;
            } else {
                const FVec3 staged{
                    s.from_v3.x + (s.to_v3.x - s.from_v3.x) * e,
                    s.from_v3.y + (s.to_v3.y - s.from_v3.y) * e,
                    s.from_v3.z + (s.to_v3.z - s.from_v3.z) * e,
                };
                if (IsFiniteTweenValue(staged)) *p = staged;
            }
            break;
        }
        default: break;
        }

        if (finished) {
            s.active = false;
            s.kind   = EKind::None;
            s.target = nullptr;
            if (m_ActiveCount > 0) --m_ActiveCount;
        }
    }
}

} // namespace acs::game
