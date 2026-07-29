// SPDX-License-Identifier: Apache-2.0
// FTimerManager 実装
#include "event/Timer.h"

#include <intrin.h>

namespace acs {

namespace {

/** 0 ではない 64-bit 値の最下位 set bit index を返す。 */
u32 FirstSetBit(u64 value) noexcept
{
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<u32>(index);
}

} // namespace

/** 次の登録へ割り当てる世代番号を返し、無効値 0 は飛ばす。 */
u32 FTimerManager::AcquireGeneration() noexcept
{
    u32 generation = m_NextGeneration++;
    if (generation == 0) generation = m_NextGeneration++;
    return generation;
}

/** slot index に対応する active word を確保し、新領域を 0 初期化する。 */
void FTimerManager::EnsureActiveWord(u32 slot_index) noexcept
{
    const usize required = static_cast<usize>(slot_index / 64u) + 1;
    const usize old_size = m_ActiveWords.Size();
    if (required <= old_size) return;
    m_ActiveWords.Resize(required);
    for (usize i = old_size; i < required; ++i) m_ActiveWords[i] = 0;
}

/** slot の active bit を立て、active 数を更新する。 */
void FTimerManager::MarkActive(u32 slot_index) noexcept
{
    EnsureActiveWord(slot_index);
    const usize word_index = slot_index / 64u;
    const u64 mask = u64{1} << (slot_index & 63u);
    if ((m_ActiveWords[word_index] & mask) == 0) {
        m_ActiveWords[word_index] |= mask;
        ++m_ActiveCount;
    }
}

/** slot の active bit を下ろし、active 数を更新する。 */
void FTimerManager::MarkInactive(u32 slot_index) noexcept
{
    const usize word_index = slot_index / 64u;
    if (word_index >= m_ActiveWords.Size()) return;
    const u64 mask = u64{1} << (slot_index & 63u);
    if ((m_ActiveWords[word_index] & mask) != 0) {
        m_ActiveWords[word_index] &= ~mask;
        --m_ActiveCount;
    }
}

/** 全 slot を無効化し、コールバックと user pointer を直ちに切り離す。 */
void FTimerManager::InvalidateAllSlots() noexcept
{
    for (usize i = 0; i < m_Slots.Size(); ++i) {
        FSlot& slot = m_Slots[i];
        slot.active = false;
        slot.cb = nullptr;
        slot.user = nullptr;
    }
    for (usize i = 0; i < m_ActiveWords.Size(); ++i) m_ActiveWords[i] = 0;
    m_ActiveCount = 0;
    m_NextId = 1;
}

/** Clear 済み slot の確保容量を解放し、再利用可能な空状態に戻す。 */
void FTimerManager::ReleaseClearedStorage() noexcept
{
    m_Slots = TArray<FSlot>{*m_Slots.GetAllocator()};
    m_ActiveWords = TArray<u64>{*m_ActiveWords.GetAllocator()};
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
        m_Slots.PushBack(FSlot{});
    }

    FSlot& s = m_Slots[idx];
    if (s.id == 0) s.id = m_NextId++;
    s.generation = AcquireGeneration();
    s.active    = true;
    s.repeating = false;
    s.remaining = delay_seconds;
    s.period    = 0.0f;
    s.cb        = cb;
    s.user      = user;
    MarkActive(idx);
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
        m_Slots.PushBack(FSlot{});
    }

    FSlot& s = m_Slots[idx];
    if (s.id == 0) s.id = m_NextId++;
    s.generation = AcquireGeneration();
    s.active    = true;
    s.repeating = true;
    s.remaining = period_seconds;
    s.period    = period_seconds;
    s.cb        = cb;
    s.user      = user;
    MarkActive(idx);
    return FTimerHandle{ s.id, s.generation };
}

/** 指定タイマを id から直接特定して解除し、slot を再利用待ちへ戻す。 */
bool FTimerManager::Cancel(FTimerHandle h) noexcept {
    if (!h.IsValid()) return false;
    // id は新規 slot の index + 1 として一度だけ採番され、再利用時も変わらない。
    const u32 idx = h.id - 1;
    if (idx >= m_Slots.Size()) return false;
    ++m_Diagnostics.cancel_slot_probes;
    FSlot& s = m_Slots[idx];
    if (s.id != h.id || s.generation != h.generation || !s.active) return false;
    s.active = false;
    s.cb     = nullptr;
    s.user   = nullptr;
    MarkInactive(idx);
    m_FreeIndices.PushBack(idx);
    return true;
}

/** dt を経過させ、発火条件を満たしたタイマを呼ぶ (周期は catch-up で複数回発火し得る)。 */
void FTimerManager::Tick(f32 dt) noexcept {
    // 外側のコールバックが Clear 済みなら、配列の解放を担当する最外周 Tick へ戻る。
    if (m_ClearPending) return;
    ++m_TickDepth;

    m_Diagnostics.active_slots_visited = 0;
    m_Diagnostics.active_words_loaded = 0;

    // コールバック中の追加・Cancel・Clear を許しつつ、Tick 開始時点の slot 範囲だけを
    // active bitset で昇順に走査する。コールバック後は同じ word を読み直すので、未走査の
    // 空き slot が再利用された場合も従来どおり同じ Tick 内で処理される。
    const usize initial_count = m_Slots.Size();
    const usize initial_words = (initial_count + 63u) / 64u;
    for (usize word_index = 0; word_index < initial_words && word_index < m_ActiveWords.Size(); ++word_index) {
        ++m_Diagnostics.active_words_loaded;
        /** この word で次に探索する最小 bit。 */
        u32 minimum_bit = 0;
        while (minimum_bit < 64u) {
            /** 初期範囲内かつ未走査の active bit。 */
            u64 candidates = m_ActiveWords[word_index];
            if (minimum_bit != 0) candidates &= (~u64{0} << minimum_bit);
            if (word_index + 1 == initial_words && (initial_count & 63u) != 0) {
                candidates &= (u64{1} << (initial_count & 63u)) - 1;
            }
            if (candidates == 0) break;

            /** 次に処理する word 内 bit。 */
            const u32 bit = FirstSetBit(candidates);
            /** 次に処理する slot index。 */
            const usize slot_index = word_index * 64u + bit;
            minimum_bit = bit + 1;
            ++m_Diagnostics.active_slots_visited;

            /** 現在処理する active slot。 */
            FSlot& slot = m_Slots[slot_index];
            if (!slot.active) {
                MarkInactive(static_cast<u32>(slot_index));
                continue;
            }
            slot.remaining -= dt;
            if (slot.remaining > 0.0f) continue;

            if (!slot.repeating) {
                // コールバック中の同一ハンドル Cancel を安全に拒否できるよう、先に解放する。
                /** 1 回だけ呼ぶ callback。 */
                const TimerCallback callback = slot.cb;
                /** callback へ渡す利用者データ。 */
                void* const user = slot.user;
                slot.active = false;
                slot.cb = nullptr;
                slot.user = nullptr;
                MarkInactive(static_cast<u32>(slot_index));
                m_FreeIndices.PushBack(static_cast<u32>(slot_index));
                callback(user);
                continue;
            }

            // 発火ごとに index で取り直し、配列再確保後の参照を残さない。
            /** 再利用を検知する id。 */
            const u32 fire_id = slot.id;
            /** 再利用を検知する世代。 */
            const u32 fire_generation = slot.generation;
            /** 1 Tick 内の発火数爆発を防ぐ上限。 */
            const u32 kMaxCatchUp = 4096;
            /** 今回の Tick で発火した回数。 */
            u32 fired = 0;
            while (true) {
                if (slot_index >= m_Slots.Size()) break;
                /** callback 後に取り直した slot。 */
                FSlot& current_slot = m_Slots[slot_index];
                // Cancel または再利用済みなら現在の周期処理を終える。
                if (!current_slot.active || !current_slot.repeating || current_slot.id != fire_id || current_slot.generation != fire_generation) break;
                if (current_slot.remaining > 0.0f) break;
                // 不正 period による無限ループを防ぎ、slot を再利用可能にする。
                if (current_slot.period <= 0.0f) {
                    current_slot.active = false;
                    current_slot.cb = nullptr;
                    current_slot.user = nullptr;
                    MarkInactive(static_cast<u32>(slot_index));
                    m_FreeIndices.PushBack(static_cast<u32>(slot_index));
                    break;
                }
                current_slot.remaining += current_slot.period;
                if (fired >= kMaxCatchUp) {
                    // 次 Tick へ繰り越せるよう remaining を正方向へ戻す。
                    if (current_slot.remaining < 0.0f) current_slot.remaining = current_slot.period;
                    break;
                }
                ++fired;
                /** 今回呼ぶ callback。 */
                const TimerCallback callback = current_slot.cb;
                /** callback へ渡す利用者データ。 */
                void* const user = current_slot.user;
                // 呼び出し後は current_slot が無効になり得るため、次反復で取り直す。
                callback(user);
            }
        }
    }

    --m_TickDepth;
    if (m_TickDepth == 0 && m_ClearPending) ReleaseClearedStorage();
}

/** active な slot を数えて現在のアクティブタイマ数を返す。 */
u32 FTimerManager::ActiveCount() const noexcept {
    return m_ActiveCount;
}

} // namespace acs
