// SPDX-License-Identifier: Apache-2.0
// FTimerManager 実装
#include "event/Timer.h"

namespace acs {

/** 次の登録へ割り当てる世代番号を返し、無効値 0 は飛ばす。 */
u32 FTimerManager::AcquireGeneration() noexcept
{
    u32 generation = m_NextGeneration++;
    if (generation == 0) generation = m_NextGeneration++;
    return generation;
}

/** 全 slot を無効化し、コールバックと user pointer を直ちに切り離す。 */
void FTimerManager::InvalidateAllSlots() noexcept
{
    for (usize i = 0; i < m_Slots.Size(); ++i) {
        Slot& slot = m_Slots[i];
        slot.active = false;
        slot.cb = nullptr;
        slot.user = nullptr;
    }
    m_NextId = 1;
}

/** Clear 済み slot の確保容量を解放し、再利用可能な空状態に戻す。 */
void FTimerManager::ReleaseClearedStorage() noexcept
{
    m_Slots = TArray<Slot>{*m_Slots.GetAllocator()};
    m_FreeIndices = TArray<u32>{*m_FreeIndices.GetAllocator()};
    m_ClearPending = false;
}

/** 全タイマを無効化し、Tick 中なら配列容量の解放だけを安全な時点まで延期する。 */
void FTimerManager::Clear() noexcept
{
    if (m_ClearPending) return;

    InvalidateAllSlots();
    if (m_TickDepth != 0) {
        // Tick が保持し得る Slot& を壊さず、コールバック復帰後の走査を安全に終わらせる。
        m_ClearPending = true;
        return;
    }
    ReleaseClearedStorage();
}

/** delay_seconds 後に 1 回だけ呼ぶタイマを登録する (空き slot 再利用 + 世代更新)。 */
FTimerHandle FTimerManager::SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || delay_seconds < 0.0f || m_ClearPending) return kInvalidTimer;

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
    s.generation = AcquireGeneration();
    s.active    = true;
    s.repeating = false;
    s.remaining = delay_seconds;
    s.period    = 0.0f;
    s.cb        = cb;
    s.user      = user;
    return FTimerHandle{ s.id, s.generation };
}

/** period_seconds ごとに繰り返し呼ぶタイマを登録する (空き slot 再利用 + 世代更新)。 */
FTimerHandle FTimerManager::SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || period_seconds <= 0.0f || m_ClearPending) return kInvalidTimer;

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
    s.generation = AcquireGeneration();
    s.active    = true;
    s.repeating = true;
    s.remaining = period_seconds;
    s.period    = period_seconds;
    s.cb        = cb;
    s.user      = user;
    return FTimerHandle{ s.id, s.generation };
}

/** 指定タイマを線形検索して解除し、slot を再利用待ちへ戻す。 */
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

/** dt を経過させ、発火条件を満たしたタイマを呼ぶ (周期は catch-up で複数回発火し得る)。 */
void FTimerManager::Tick(f32 dt) noexcept {
    // 外側のコールバックが Clear 済みなら、配列の解放を担当する最外周 Tick へ戻る。
    if (m_ClearPending) return;
    ++m_TickDepth;

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
            const TimerCallback cb   = s.cb;
            void* const         user = s.user;
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
            const TimerCallback cb   = cur.cb;
            void* const         user = cur.user;
            cb(user);  // 呼び出し後 cur/s は dangling し得る → 参照を残さずループ先頭で取り直す
        }
    }

    --m_TickDepth;
    if (m_TickDepth == 0 && m_ClearPending) ReleaseClearedStorage();
}

/** active な slot を数えて現在のアクティブタイマ数を返す。 */
u32 FTimerManager::ActiveCount() const noexcept {
    u32 n = 0;
    for (usize i = 0; i < m_Slots.Size(); ++i) if (m_Slots[i].active) ++n;
    return n;
}

} // namespace acs
