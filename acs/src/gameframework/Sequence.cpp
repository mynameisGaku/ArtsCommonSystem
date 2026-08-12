// SPDX-License-Identifier: Apache-2.0
// wait・callback・tween action列を構築し、generation付きhandleで登録順に実行する。
#include "gameframework/Sequence.h"
#include "foundation/Move.h"

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

/** Wait アクションを末尾に追加する。 */
FSequence& FSequence::Wait(f32 seconds) noexcept {
    if (!std::isfinite(seconds)) return *this;
    FSeqAction a;
    a.kind     = FSeqAction::EKind::Wait;
    a.duration = seconds < 0.0f ? 0.0f : seconds;
    m_Actions.Add(Move(a));
    return *this;
}

/** Call アクションを末尾に追加する。 */
FSequence& FSequence::Call(void(*fn)(void*) noexcept, void* user) noexcept {
    FSeqAction a;
    a.kind      = FSeqAction::EKind::Call;
    a.duration  = 0.0f;
    a.call_fn   = fn;
    a.call_user = user;
    m_Actions.Add(Move(a));
    return *this;
}

/** f32 を tween する TweenF アクションを末尾に追加する。 */
FSequence& FSequence::Tween(f32* target, f32 from, f32 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return *this;
    }
    FSeqAction a;
    a.kind           = FSeqAction::EKind::TweenF;
    a.duration       = duration < 0.0f ? 0.0f : duration;
    a.tween_f_target = target;
    a.tween_f_from   = from;
    a.tween_f_to     = to;
    a.ease           = ease != nullptr ? ease : Easing::Linear;
    m_Actions.Add(Move(a));
    return *this;
}

FSequence& FSequence::Tween(f32* target, f32 from, f32 to, f32 duration,
                           Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

/** FVec2 を tween する TweenV2 アクションを末尾に追加する。 */
FSequence& FSequence::Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return *this;
    }
    FSeqAction a;
    a.kind            = FSeqAction::EKind::TweenV2;
    a.duration        = duration < 0.0f ? 0.0f : duration;
    a.tween_v2_target = target;
    a.tween_v2_from   = from;
    a.tween_v2_to     = to;
    a.ease            = ease != nullptr ? ease : Easing::Linear;
    m_Actions.Add(Move(a));
    return *this;
}

FSequence& FSequence::Tween(FVec2* target, FVec2 from, FVec2 to, f32 duration,
                           Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

/** FVec3 を tween する TweenV3 アクションを末尾に追加する。 */
FSequence& FSequence::Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                           Easing::EasingFn ease) noexcept {
    if (!IsFiniteTweenValue(duration)
        || !IsFiniteTweenValue(from)
        || !IsFiniteTweenValue(to)) {
        return *this;
    }
    FSeqAction a;
    a.kind            = FSeqAction::EKind::TweenV3;
    a.duration        = duration < 0.0f ? 0.0f : duration;
    a.tween_v3_target = target;
    a.tween_v3_from   = from;
    a.tween_v3_to     = to;
    a.ease            = ease != nullptr ? ease : Easing::Linear;
    m_Actions.Add(Move(a));
    return *this;
}

FSequence& FSequence::Tween(FVec3* target, FVec3 from, FVec3 to, f32 duration,
                           Easing::EEasingType ease) noexcept {
    return Tween(target, from, to, duration, Easing::GetFunction(ease));
}

/** 空きスロットを再利用優先で確保する (無ければ末尾に追加)。 */
u32 CSequenceRunner::AcquireSlot() noexcept {
    for (u32 i = 0; i < m_Slots.Num(); ++i) {
        if (!m_Slots[i].active) return i;
    }
    m_Slots.Add({});
    return static_cast<u32>(m_Slots.Num()) - 1u;
}

/** シーケンスをスロットに格納して実行開始する。 */
FSeqHandle CSequenceRunner::Start(FSequence seq) noexcept {
    if (seq.Actions().Num() == 0) return {};
    const u32 idx = AcquireSlot();
    FSlot& s = m_Slots[idx];
    // Slot は再利用なので明示的にリセット (seq は新しいものを Move 代入)
    s.seq            = Move(seq);
    s.action_idx     = 0;
    s.action_elapsed = 0.0f;
    s.loops_done     = 0;
    s.call_fired     = false;
    s.active         = true;
    s.generation     = s.generation + 1u;
    if (s.generation == 0u) s.generation = 1u;
    ++m_ActiveCount;
    return FSeqHandle{idx, s.generation};
}

/** ハンドルが指すスロットを非アクティブ化する。 */
void CSequenceRunner::Cancel(FSeqHandle h) noexcept {
    if (!h.IsValid() || h.index >= m_Slots.Num()) return;
    FSlot& s = m_Slots[h.index];
    if (s.generation != h.generation || !s.active) return;
    s.active = false;
    if (m_ActiveCount > 0) --m_ActiveCount;
}

void CSequenceRunner::CancelAll() noexcept {
    for (u32 i = 0; i < m_Slots.Num(); ++i) {
        m_Slots[i].active = false;
    }
    m_ActiveCount = 0;
}

bool CSequenceRunner::IsActive(FSeqHandle h) const noexcept {
    if (!h.IsValid() || h.index >= m_Slots.Num()) return false;
    const FSlot& s = m_Slots[h.index];
    return s.active && s.generation == h.generation;
}

void CSequenceRunner::AdvanceToNext(FSlot& s) noexcept {
    ++s.action_idx;
    s.action_elapsed = 0.0f;
    s.call_fired     = false;
    if (s.action_idx >= s.seq.Actions().Num()) {
        // ループ判定: loop_count=0 は無限、それ以外は loops_done < loop_count-1 ならもう 1 周
        ++s.loops_done;
        const u32 lc = s.seq.LoopCount();
        if (lc == 0u || s.loops_done < lc) {
            s.action_idx = 0;
        } else {
            // 終了
            s.active = false;
            if (m_ActiveCount > 0) --m_ActiveCount;
        }
    }
}

void CSequenceRunner::FinishAction(FSlot& /*s*/, const FSeqAction& act) noexcept {
    // 完了時に最終値を正確に書く (浮動小数誤差を残さない)
    switch (act.kind) {
    case FSeqAction::EKind::TweenF:
        if (act.tween_f_target) *act.tween_f_target = act.tween_f_to;
        break;
    case FSeqAction::EKind::TweenV2:
        if (act.tween_v2_target) *act.tween_v2_target = act.tween_v2_to;
        break;
    case FSeqAction::EKind::TweenV3:
        if (act.tween_v3_target) *act.tween_v3_target = act.tween_v3_to;
        break;
    default: break;
    }
}

void CSequenceRunner::Tick(f32 dt) noexcept {
    if (m_ActiveCount == 0 || dt <= 0.0f || !std::isfinite(dt)) return;

    for (u32 i = 0; i < m_Slots.Num(); ++i) {
        FSlot& s = m_Slots[i];
        if (!s.active) continue;

        // 1 フレームで複数アクションを進める可能性 (Call が連続する場合等) を扱う
        // ためループする。spiral 防止に上限 (= 1 フレームで max 64 アクション)。
        u32 safety = 64;
        f32 remaining_dt = dt;

        while (s.active && safety-- > 0) {
            const auto& actions = s.seq.Actions();
            if (s.action_idx >= actions.Num()) break;     // ループ判定済の保険
            const FSeqAction& act = actions[s.action_idx];

            switch (act.kind) {
            case FSeqAction::EKind::Call:
                if (!s.call_fired) {
                    s.call_fired = true;                 // 先に立てて二重発火を防ぐ
                    const auto fn   = act.call_fn;       // act 参照を退避 (callback 後 s/act は dangling し得る)
                    const auto user = act.call_user;
                    if (fn) {
                        fn(user);   // callback 内 Start() 等で m_Slots 再確保 → s/act が無効化され得る
                        // s/act を触らず index で再取得。まだ同一の active slot なら進める。
                        if (i < m_Slots.Num() && m_Slots[i].active) AdvanceToNext(m_Slots[i]);
                        break;      // 今フレームのこの slot 処理は安全のため打ち切り (次 Tick で継続)
                    }
                }
                AdvanceToNext(s);   // callback 無し (fn==null / 既発火) → s は有効
                continue;     // 残 dt で次のアクションへ

            case FSeqAction::EKind::Wait:
                s.action_elapsed += remaining_dt;
                if (!std::isfinite(s.action_elapsed)) s.action_elapsed = act.duration;
                if (s.action_elapsed >= act.duration) {
                    // 残 dt は次アクションへ繰り越し
                    remaining_dt = s.action_elapsed - act.duration;
                    AdvanceToNext(s);
                    continue;
                }
                remaining_dt = 0.0f;   // 全消費
                break;

            case FSeqAction::EKind::TweenF:
            case FSeqAction::EKind::TweenV2:
            case FSeqAction::EKind::TweenV3: {
                s.action_elapsed += remaining_dt;
                if (!std::isfinite(s.action_elapsed)) s.action_elapsed = act.duration;
                f32 t = (act.duration > 0.0f) ? (s.action_elapsed / act.duration) : 1.0f;
                const bool finished = (t >= 1.0f);
                if (finished) t = 1.0f;
                const f32 eased = act.ease(t);
                const f32 e = std::isfinite(eased) ? eased : t;

                if (act.kind == FSeqAction::EKind::TweenF && act.tween_f_target) {
                    if (finished) {
                        *act.tween_f_target = act.tween_f_to;
                    } else {
                        const f32 staged = act.tween_f_from
                            + (act.tween_f_to - act.tween_f_from) * e;
                        if (IsFiniteTweenValue(staged)) {
                            *act.tween_f_target = staged;
                        }
                    }
                } else if (act.kind == FSeqAction::EKind::TweenV2 && act.tween_v2_target) {
                    FVec2* p = act.tween_v2_target;
                    if (finished) {
                        *p = act.tween_v2_to;
                    } else {
                        const FVec2 staged{
                            act.tween_v2_from.x
                                + (act.tween_v2_to.x - act.tween_v2_from.x) * e,
                            act.tween_v2_from.y
                                + (act.tween_v2_to.y - act.tween_v2_from.y) * e,
                        };
                        if (IsFiniteTweenValue(staged)) *p = staged;
                    }
                } else if (act.kind == FSeqAction::EKind::TweenV3 && act.tween_v3_target) {
                    FVec3* p = act.tween_v3_target;
                    if (finished) {
                        *p = act.tween_v3_to;
                    } else {
                        const FVec3 staged{
                            act.tween_v3_from.x
                                + (act.tween_v3_to.x - act.tween_v3_from.x) * e,
                            act.tween_v3_from.y
                                + (act.tween_v3_to.y - act.tween_v3_from.y) * e,
                            act.tween_v3_from.z
                                + (act.tween_v3_to.z - act.tween_v3_from.z) * e,
                        };
                        if (IsFiniteTweenValue(staged)) *p = staged;
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
