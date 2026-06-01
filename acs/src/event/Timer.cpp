// SPDX-License-Identifier: Apache-2.0
// FTimerManager 実装
#include "event/Timer.h"

namespace acs {

FTimerHandle FTimerManager::SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || delay_seconds < 0.0f) return kInvalidTimer;

    u32 idx;
    if (m_FreeIndices.Size() > 0) {
        idx = m_FreeIndices[m_FreeIndices.Size() - 1];
        m_FreeIndices.PopBack();
    } else {
        idx = static_cast<u32>(m_Slots.Size());
        m_Slots.PushBack(Slot{});
    }

    Slot& s = m_Slots[idx];
    if (s.id == 0) s.id = m_NextId++;
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
    if (m_FreeIndices.Size() > 0) {
        idx = m_FreeIndices[m_FreeIndices.Size() - 1];
        m_FreeIndices.PopBack();
    } else {
        idx = static_cast<u32>(m_Slots.Size());
        m_Slots.PushBack(Slot{});
    }

    Slot& s = m_Slots[idx];
    if (s.id == 0) s.id = m_NextId++;
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
    // id は 1-based、m_Slots の中を線形検索（タイマ数は通常少ないので OK）
    for (usize i = 0; i < m_Slots.Size(); ++i) {
        Slot& s = m_Slots[i];
        if (s.id == h.id && s.generation == h.generation && s.active) {
            s.active = false;
            s.cb     = nullptr;
            s.user   = nullptr;
            m_FreeIndices.PushBack(static_cast<u32>(i));
            return true;
        }
    }
    return false;
}

void FTimerManager::Tick(f32 dt) noexcept {
    // コールバック中にタイマを追加 / Cancel される可能性があるので、
    // ループ中の m_Slots.Size() を毎回読み直す。新規追加は末尾のため安全。
    // 既存スロットを active=false にされても次のループで checked される。
    const usize initial_count = m_Slots.Size();
    for (usize i = 0; i < initial_count; ++i) {
        Slot& s = m_Slots[i];
        if (!s.active) continue;
        s.remaining -= dt;
        if (s.remaining > 0.0f) continue;

        if (!s.repeating) {
            // 1 回限りはコールバックを呼ぶ前にスロット解放
            // (コールバック中に同じハンドルを Cancel しても false が返るだけで安全)
            TimerCallback cb   = s.cb;
            void*         user = s.user;
            s.active = false;
            s.cb     = nullptr;
            s.user   = nullptr;
            m_FreeIndices.PushBack(static_cast<u32>(i));
            cb(user);
            continue;
        }

        // 周期タイマ: dt が複数周期ぶんなら複数回発火させる (catch-up)。drift しないよう
        // period を都度加算する。コールバックが SetTimeout/SetInterval で m_Slots を再 alloc
        // し得るため、発火のたびに index で slot を取り直し、id/generation で妥当性を確認して
        // から次を撃つ (取り直さないと再確保で参照が dangling して use-after-free になる)。
        const u32 fire_id  = s.id;
        const u32 fire_gen = s.generation;
        const u32 kMaxCatchUp = 4096;  // period << dt (or period→0) のときの発火数爆発を防ぐ上限
        u32 fired = 0;
        while (true) {
            if (i >= m_Slots.Size()) break;
            Slot& cur = m_Slots[i];
            if (!cur.active || !cur.repeating ||
                cur.id != fire_id || cur.generation != fire_gen) break;  // Cancel / 再利用された
            if (cur.remaining > 0.0f) break;                             // 追いつき完了
            if (cur.period <= 0.0f) { cur.active = false; break; }       // 不正 period の無限ループ防止
            cur.remaining += cur.period;
            if (fired >= kMaxCatchUp) {
                // 上限到達: これ以上は今回の Tick で発火しない。次 Tick へ繰り越せるよう
                // remaining を正方向へ帳尻合わせする。
                if (cur.remaining < 0.0f) cur.remaining = cur.period;
                break;
            }
            ++fired;
            TimerCallback cb   = cur.cb;
            void*         user = cur.user;
            cb(user);  // 呼び出し後 cur/s は dangling し得る → 参照を残さずループ先頭で取り直す
        }
    }
}

u32 FTimerManager::ActiveCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < m_Slots.Size(); ++i) if (m_Slots[i].active) ++n;
    return n;
}

} // namespace acs
