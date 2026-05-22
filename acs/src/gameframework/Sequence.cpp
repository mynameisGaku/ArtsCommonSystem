// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar C — Sequence / SequenceRunner 実装 (Phase 4)
#include "gameframework/Sequence.h"
#include "foundation/Move.h"

namespace acs::game {

Sequence& Sequence::Wait(f32 seconds) noexcept {
    SeqAction a;
    a.kind     = SeqAction::Kind::Wait;
    a.duration = seconds < 0.0f ? 0.0f : seconds;
    _actions.PushBack(Move(a));
    return *this;
}

Sequence& Sequence::Call(void(*fn)(void*) noexcept, void* user) noexcept {
    SeqAction a;
    a.kind      = SeqAction::Kind::Call;
    a.duration  = 0.0f;
    a.call_fn   = fn;
    a.call_user = user;
    _actions.PushBack(Move(a));
    return *this;
}

Sequence& Sequence::Tween(f32* target, f32 from, f32 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    SeqAction a;
    a.kind           = SeqAction::Kind::TweenF;
    a.duration       = duration < 0.0f ? 0.0f : duration;
    a.tween_f_target = target;
    a.tween_f_from   = from;
    a.tween_f_to     = to;
    a.ease           = ease != nullptr ? ease : Easing::Linear;
    _actions.PushBack(Move(a));
    return *this;
}

Sequence& Sequence::Tween(Vec2* target, Vec2 from, Vec2 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    SeqAction a;
    a.kind            = SeqAction::Kind::TweenV2;
    a.duration        = duration < 0.0f ? 0.0f : duration;
    a.tween_v2_target = target;
    a.tween_v2_from   = from;
    a.tween_v2_to     = to;
    a.ease            = ease != nullptr ? ease : Easing::Linear;
    _actions.PushBack(Move(a));
    return *this;
}

Sequence& Sequence::Tween(Vec3* target, Vec3 from, Vec3 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    SeqAction a;
    a.kind            = SeqAction::Kind::TweenV3;
    a.duration        = duration < 0.0f ? 0.0f : duration;
    a.tween_v3_target = target;
    a.tween_v3_from   = from;
    a.tween_v3_to     = to;
    a.ease            = ease != nullptr ? ease : Easing::Linear;
    _actions.PushBack(Move(a));
    return *this;
}

// =================== Runner ===================

u32 SequenceRunner::AcquireSlot() noexcept {
    for (u32 i = 0; i < _slots.Size(); ++i) {
        if (!_slots[i].active) return i;
    }
    _slots.PushBack({});
    return static_cast<u32>(_slots.Size()) - 1u;
}

SeqHandle SequenceRunner::Start(Sequence seq) noexcept {
    if (seq.Actions().Size() == 0) return {};
    const u32 idx = AcquireSlot();
    Slot& s = _slots[idx];
    // Slot は再利用なので明示的にリセット (seq は新しいものを Move 代入)
    s.seq            = Move(seq);
    s.action_idx     = 0;
    s.action_elapsed = 0.0f;
    s.loops_done     = 0;
    s.call_fired     = false;
    s.active         = true;
    s.generation     = s.generation + 1u;
    if (s.generation == 0u) s.generation = 1u;
    ++_active_count;
    return SeqHandle{idx, s.generation};
}

void SequenceRunner::Cancel(SeqHandle h) noexcept {
    if (!h.IsValid() || h.index >= _slots.Size()) return;
    Slot& s = _slots[h.index];
    if (s.generation != h.generation || !s.active) return;
    s.active = false;
    if (_active_count > 0) --_active_count;
}

void SequenceRunner::CancelAll() noexcept {
    for (u32 i = 0; i < _slots.Size(); ++i) {
        _slots[i].active = false;
    }
    _active_count = 0;
}

bool SequenceRunner::IsActive(SeqHandle h) const noexcept {
    if (!h.IsValid() || h.index >= _slots.Size()) return false;
    const Slot& s = _slots[h.index];
    return s.active && s.generation == h.generation;
}

void SequenceRunner::AdvanceToNext(Slot& s) noexcept {
    ++s.action_idx;
    s.action_elapsed = 0.0f;
    s.call_fired     = false;
    if (s.action_idx >= s.seq.Actions().Size()) {
        // ループ判定: loop_count=0 は無限、それ以外は loops_done < loop_count-1 ならもう 1 周
        ++s.loops_done;
        const u32 lc = s.seq.LoopCount();
        if (lc == 0u || s.loops_done < lc) {
            s.action_idx = 0;
        } else {
            // 終了
            s.active = false;
            if (_active_count > 0) --_active_count;
        }
    }
}

void SequenceRunner::FinishAction(Slot& /*s*/, const SeqAction& act) noexcept {
    // 完了時に最終値を正確に書く (浮動小数誤差を残さない)
    switch (act.kind) {
    case SeqAction::Kind::TweenF:
        if (act.tween_f_target) *act.tween_f_target = act.tween_f_to;
        break;
    case SeqAction::Kind::TweenV2:
        if (act.tween_v2_target) *act.tween_v2_target = act.tween_v2_to;
        break;
    case SeqAction::Kind::TweenV3:
        if (act.tween_v3_target) *act.tween_v3_target = act.tween_v3_to;
        break;
    default: break;
    }
}

void SequenceRunner::Tick(f32 dt) noexcept {
    if (_active_count == 0 || dt <= 0.0f) return;

    for (u32 i = 0; i < _slots.Size(); ++i) {
        Slot& s = _slots[i];
        if (!s.active) continue;

        // 1 フレームで複数アクションを進める可能性 (Call が連続する場合等) を扱う
        // ためループする。spiral 防止に上限 (= 1 フレームで max 64 アクション)。
        u32 safety = 64;
        f32 remaining_dt = dt;

        while (s.active && safety-- > 0) {
            const auto& actions = s.seq.Actions();
            if (s.action_idx >= actions.Size()) break;     // ループ判定済の保険
            const SeqAction& act = actions[s.action_idx];

            switch (act.kind) {
            case SeqAction::Kind::Call:
                if (!s.call_fired) {
                    if (act.call_fn) act.call_fn(act.call_user);
                    s.call_fired = true;
                }
                AdvanceToNext(s);
                continue;     // 残 dt で次のアクションへ

            case SeqAction::Kind::Wait:
                s.action_elapsed += remaining_dt;
                if (s.action_elapsed >= act.duration) {
                    // 残 dt は次アクションへ繰り越し
                    remaining_dt = s.action_elapsed - act.duration;
                    AdvanceToNext(s);
                    continue;
                }
                remaining_dt = 0.0f;   // 全消費
                break;

            case SeqAction::Kind::TweenF:
            case SeqAction::Kind::TweenV2:
            case SeqAction::Kind::TweenV3: {
                s.action_elapsed += remaining_dt;
                f32 t = (act.duration > 0.0f) ? (s.action_elapsed / act.duration) : 1.0f;
                const bool finished = (t >= 1.0f);
                if (finished) t = 1.0f;
                const f32 e = act.ease(t);

                if (act.kind == SeqAction::Kind::TweenF && act.tween_f_target) {
                    *act.tween_f_target = finished
                        ? act.tween_f_to
                        : act.tween_f_from + (act.tween_f_to - act.tween_f_from) * e;
                } else if (act.kind == SeqAction::Kind::TweenV2 && act.tween_v2_target) {
                    Vec2* p = act.tween_v2_target;
                    if (finished) {
                        *p = act.tween_v2_to;
                    } else {
                        p->x = act.tween_v2_from.x + (act.tween_v2_to.x - act.tween_v2_from.x) * e;
                        p->y = act.tween_v2_from.y + (act.tween_v2_to.y - act.tween_v2_from.y) * e;
                    }
                } else if (act.kind == SeqAction::Kind::TweenV3 && act.tween_v3_target) {
                    Vec3* p = act.tween_v3_target;
                    if (finished) {
                        *p = act.tween_v3_to;
                    } else {
                        p->x = act.tween_v3_from.x + (act.tween_v3_to.x - act.tween_v3_from.x) * e;
                        p->y = act.tween_v3_from.y + (act.tween_v3_to.y - act.tween_v3_from.y) * e;
                        p->z = act.tween_v3_from.z + (act.tween_v3_to.z - act.tween_v3_from.z) * e;
                    }
                }

                if (finished) {
                    FinishAction(s, act);
                    remaining_dt = s.action_elapsed - act.duration;
                    if (remaining_dt < 0.0f) remaining_dt = 0.0f;
                    AdvanceToNext(s);
                    continue;
                }
                remaining_dt = 0.0f;
                break;
            }
            }     // switch

            if (remaining_dt <= 0.0f) break;
        }     // while
    }     // for slots
}

} // namespace acs::game
