// SPDX-License-Identifier: Apache-2.0
#include "event/TimerManager.h"

#include <cmath>
#include <intrin.h>
#include <limits>

namespace acs {

namespace {

/** 0ではない64-bit値の最下位 set bit 位置を返す。 */
u32 FirstSetBit(u64 value) noexcept {
    /** 検出した bit 位置。 */
    unsigned long index = 0;
    _BitScanForward64(&index, value);
    return static_cast<u32>(index);
}

/**
 * 次の世代番号を取り出し、最大値を割り当てた後は使い切り状態へ移す。
 * @param next_generation 次に割り当てる世代番号。0は使い切り済みを示す。
 */
constexpr u32 ConsumeGeneration(u32& next_generation) noexcept {
    if (next_generation == 0) return 0;
    /** 今回割り当てる世代番号。 */
    const u32 generation = next_generation;
    next_generation = generation == ~u32(0) ? 0 : generation + 1;
    return generation;
}

/** 最大世代を一度だけ割り当てた後は登録を永久に拒否することを検査する。 */
constexpr bool GenerationExhaustionIsPermanent() noexcept {
    /** 最大世代から始める検査用の次番号。 */
    u32 next_generation = ~u32(0);
    return ConsumeGeneration(next_generation) == ~u32(0) && ConsumeGeneration(next_generation) == 0 && next_generation == 0;
}

static_assert(GenerationExhaustionIsPermanent());

} // namespace

/** 次の登録に使うゼロ以外の識別番号を返す。 */
u32 CTimerManager::AcquireId() noexcept {
    /** 今回割り当てる識別番号。 */
    u32 identifier = m_NextId++;
    if (identifier == 0) identifier = m_NextId++;
    return identifier;
}

/** 次の登録に使う世代番号を返し、使い切った場合は0を返す。 */
u32 CTimerManager::AcquireGeneration() noexcept {
    return ConsumeGeneration(m_NextGeneration);
}

/** 新規タイマー枠を active bitset で表せるようにする。 */
bool CTimerManager::EnsureActiveWord(u32 slot_index) noexcept {
    /** 必要な active word 数。 */
    const usize required = static_cast<usize>(slot_index / 64u) + 1;
    /** 拡張前の active word 数。 */
    const usize old_size = m_ActiveWords.Num();
    if (required <= old_size) return true;
    if (!m_ActiveWords.TrySetNum(required)) return false;
    for (/** 初期化する active word 位置。 */ usize i = old_size; i < required; ++i) m_ActiveWords[i] = 0;
    return true;
}

/** 登録に使う空き枠を返し、確保できなければ無効な位置を返す。 */
u32 CTimerManager::AcquireSlotIndex() noexcept {
    if (m_FreeIndices.Num() > 0) {
        /** 再利用するタイマー枠の位置。 */
        const u32 index = m_FreeIndices[m_FreeIndices.Num() - 1];
        m_FreeIndices.Pop();
        return index;
    }
    if (m_Slots.Num() >= static_cast<usize>(std::numeric_limits<u32>::max())) return std::numeric_limits<u32>::max();
    /** 新しく追加するタイマー枠の位置。 */
    const u32 index = static_cast<u32>(m_Slots.Num());
    if (!EnsureActiveWord(index)) return std::numeric_limits<u32>::max();
    return m_Slots.TryAdd(FSlot{}) ? index : std::numeric_limits<u32>::max();
}

/**
 * 検証済みの時間と処理をタイマー枠へ登録する。
 * @param duration_seconds 発火までの秒数または周期。
 * @param repeating 周期実行する場合はtrue。
 * @param cb 発火時に呼ぶ関数。
 * @param user 関数へ渡す値。
 */
FTimerHandle CTimerManager::RegisterTimer(f32 duration_seconds, bool repeating, TimerCallback cb, void* user) noexcept {
    if (m_NextGeneration == 0) return kInvalidTimer;
    /** 登録先のタイマー枠の位置。 */
    const u32 index = AcquireSlotIndex();
    if (index == std::numeric_limits<u32>::max()) return kInvalidTimer;

    /** 登録先のタイマー情報。 */
    FSlot& slot = m_Slots[index];
    if (slot.id == 0) slot.id = AcquireId();
    /** 今回の登録に割り当てる世代番号。 */
    const u32 generation = AcquireGeneration();
    if (generation == 0) return kInvalidTimer;
    slot.generation = generation;
    slot.active = true;
    slot.repeating = repeating;
    slot.pending_until_next_tick = m_TickDepth != 0;
    slot.remaining = duration_seconds;
    slot.period = repeating ? duration_seconds : 0.0f;
    slot.cb = cb;
    slot.user = user;
    MarkActive(index);
    return FTimerHandle{slot.id, slot.generation};
}

/** タイマー枠の active bit を立てる。 */
void CTimerManager::MarkActive(u32 slot_index) noexcept {
    /** active word の位置。 */
    const usize word_index = slot_index / 64u;
    /** active bit の mask。 */
    const u64 mask = u64{1} << (slot_index & 63u);
    if ((m_ActiveWords[word_index] & mask) == 0) {
        m_ActiveWords[word_index] |= mask;
        ++m_ActiveCount;
    }
}

/** タイマー枠の active bit を下ろす。 */
void CTimerManager::MarkInactive(u32 slot_index) noexcept {
    /** active word の位置。 */
    const usize word_index = slot_index / 64u;
    if (word_index >= m_ActiveWords.Num()) return;
    /** active bit の mask。 */
    const u64 mask = u64{1} << (slot_index & 63u);
    if ((m_ActiveWords[word_index] & mask) != 0) {
        m_ActiveWords[word_index] &= ~mask;
        --m_ActiveCount;
    }
}

/** 全タイマー枠を無効にする。 */
void CTimerManager::InvalidateAllSlots() noexcept {
    for (/** 無効にするタイマー枠の位置。 */ usize i = 0; i < m_Slots.Num(); ++i) {
        /** 無効にするタイマー情報。 */
        FSlot& slot = m_Slots[i];
        slot.active = false;
        slot.pending_until_next_tick = false;
        slot.cb = nullptr;
        slot.user = nullptr;
    }
    for (/** 消去する active word の位置。 */ usize i = 0; i < m_ActiveWords.Num(); ++i) m_ActiveWords[i] = 0;
    m_FreeIndices.Reset();
    m_ActiveCount = 0;
    m_NextId = 1;
}

/** 全消去後の保持領域を解放する。 */
void CTimerManager::ReleaseClearedStorage() noexcept {
    m_Slots = TArray<FSlot>{*m_Slots.GetAllocator()};
    m_ActiveWords = TArray<u64>{*m_ActiveWords.GetAllocator()};
    m_FreeIndices = TArray<u32>{*m_FreeIndices.GetAllocator()};
    m_ClearPending = false;
}

/** 全タイマーを無効にして保持領域を空にする。 */
void CTimerManager::Clear() noexcept {
    if (m_ClearPending) return;
    InvalidateAllSlots();
    if (m_TickDepth != 0) {
        m_ClearPending = true;
        return;
    }
    ReleaseClearedStorage();
}

/** 指定時間後に一度だけ関数を呼び出す。 */
FTimerHandle CTimerManager::SetTimeout(f32 delay_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || delay_seconds < 0.0f || !std::isfinite(delay_seconds) || m_ClearPending) return kInvalidTimer;
    return RegisterTimer(delay_seconds, false, cb, user);
}

/** 指定時間後に一度だけデリゲートを呼び出す。 */
FTimerHandle CTimerManager::SetTimeout(f32 delay_seconds, FSimpleDelegate delegate) noexcept {
    return SetTimeout(delay_seconds, delegate.Function(), delegate.User());
}

/** 指定周期で関数を繰り返し呼び出す。 */
FTimerHandle CTimerManager::SetInterval(f32 period_seconds, TimerCallback cb, void* user) noexcept {
    if (!cb || period_seconds <= 0.0f || !std::isfinite(period_seconds) || m_ClearPending) return kInvalidTimer;
    return RegisterTimer(period_seconds, true, cb, user);
}

/** 指定周期でデリゲートを繰り返し呼び出す。 */
FTimerHandle CTimerManager::SetInterval(f32 period_seconds, FSimpleDelegate delegate) noexcept {
    return SetInterval(period_seconds, delegate.Function(), delegate.User());
}

/**
 * 指定したタイマーを取り消す。
 * @param handle 取り消すタイマーのハンドル。
 */
bool CTimerManager::Cancel(FTimerHandle handle) noexcept {
    if (!handle.IsValid()) return false;
    /** 識別番号から求めたタイマー枠の位置。 */
    const u32 index = handle.id - 1;
    if (index >= m_Slots.Num()) return false;
    ++m_Diagnostics.cancel_slot_probes;
    /** 取り消し候補のタイマー情報。 */
    FSlot& slot = m_Slots[index];
    if (slot.id != handle.id || slot.generation != handle.generation || !slot.active) return false;
    slot.active = false;
    slot.pending_until_next_tick = false;
    slot.cb = nullptr;
    slot.user = nullptr;
    MarkInactive(index);
    (void)m_FreeIndices.TryAdd(index);
    return true;
}

/** 登録中のタイマーをすべて取り消す。 */
void CTimerManager::CancelAll() noexcept {
    Clear();
}

/**
 * 指定したタイマーが現在も登録中かを返す。
 * @param handle 調べるタイマーのハンドル。
 */
bool CTimerManager::IsActive(FTimerHandle handle) const noexcept {
    if (!handle.IsValid()) return false;
    /** 識別番号から求めたタイマー枠の位置。 */
    const u32 index = handle.id - 1;
    if (index >= m_Slots.Num()) return false;
    /** 生存確認するタイマー情報。 */
    const FSlot& slot = m_Slots[index];
    return slot.id == handle.id && slot.generation == handle.generation && slot.active;
}

/**
 * 時間を進めて発火条件を満たしたタイマーを呼び出す。
 * @param dt 前回から経過した秒数。
 */
void CTimerManager::Tick(f32 dt) noexcept {
    if (m_ClearPending || m_TickDepth != 0 || dt < 0.0f || !std::isfinite(dt)) return;
    ++m_TickDepth;
    m_Diagnostics.active_slots_visited = 0;
    m_Diagnostics.active_words_loaded = 0;

    /** 更新開始時点のタイマー枠数。 */
    const usize initial_count = m_Slots.Num();
    /** 更新開始時点の active word 数。 */
    const usize initial_words = (initial_count + 63u) / 64u;
    for (/** 現在更新する active word 位置。 */ usize word_index = 0; word_index < initial_words && word_index < m_ActiveWords.Num(); ++word_index) {
        ++m_Diagnostics.active_words_loaded;
        /** この word で次に調べる最小 bit。 */
        u32 minimum_bit = 0;
        while (minimum_bit < 64u) {
            /** 初期範囲内かつ未走査の active bit。 */
            u64 candidates = m_ActiveWords[word_index];
            if (minimum_bit != 0) candidates &= (~u64{0} << minimum_bit);
            if (word_index + 1 == initial_words && (initial_count & 63u) != 0) candidates &= (u64{1} << (initial_count & 63u)) - 1;
            if (candidates == 0) break;

            /** 次に処理する word 内 bit。 */
            const u32 bit = FirstSetBit(candidates);
            /** 次に処理するタイマー枠の位置。 */
            const usize slot_index = word_index * 64u + bit;
            minimum_bit = bit + 1;
            ++m_Diagnostics.active_slots_visited;

            /** 現在処理するタイマー情報。 */
            FSlot& slot = m_Slots[slot_index];
            if (!slot.active) {
                MarkInactive(static_cast<u32>(slot_index));
                continue;
            }
            if (slot.pending_until_next_tick) continue;
            slot.remaining -= dt;
            if (slot.remaining > 0.0f) continue;

            if (!slot.repeating) {
                /** 一回だけ呼ぶ関数。 */
                const TimerCallback callback = slot.cb;
                /** 関数へ渡す値。 */
                void* const user = slot.user;
                slot.active = false;
                slot.cb = nullptr;
                slot.user = nullptr;
                MarkInactive(static_cast<u32>(slot_index));
                (void)m_FreeIndices.TryAdd(static_cast<u32>(slot_index));
                callback(user);
                continue;
            }

            /** 再利用を検出するタイマー番号。 */
            const u32 fire_id = slot.id;
            /** 再利用を検出する世代番号。 */
            const u32 fire_generation = slot.generation;
            /** 一回の更新で許す最大発火数。 */
            constexpr u32 kMaxCatchUp = 4096;
            /** 今回の更新で発火した回数。 */
            u32 fired = 0;
            while (true) {
                if (slot_index >= m_Slots.Num()) break;
                /** 呼出し後の状態を取り直したタイマー情報。 */
                FSlot& current_slot = m_Slots[slot_index];
                if (!current_slot.active || !current_slot.repeating || current_slot.id != fire_id || current_slot.generation != fire_generation) break;
                if (current_slot.remaining > 0.0f) break;
                if (current_slot.period <= 0.0f) {
                    current_slot.active = false;
                    current_slot.cb = nullptr;
                    current_slot.user = nullptr;
                    MarkInactive(static_cast<u32>(slot_index));
                    (void)m_FreeIndices.TryAdd(static_cast<u32>(slot_index));
                    break;
                }
                current_slot.remaining += current_slot.period;
                if (fired >= kMaxCatchUp) {
                    if (current_slot.remaining < 0.0f) current_slot.remaining = current_slot.period;
                    break;
                }
                ++fired;
                /** 今回呼び出す関数。 */
                const TimerCallback callback = current_slot.cb;
                /** 関数へ渡す値。 */
                void* const user = current_slot.user;
                callback(user);
            }
        }
    }

    for (/** 次回更新から有効にするタイマー枠の位置。 */ usize i = 0; i < m_Slots.Num(); ++i) m_Slots[i].pending_until_next_tick = false;
    --m_TickDepth;
    if (m_TickDepth == 0 && m_ClearPending) ReleaseClearedStorage();
}

/** 現在有効なタイマー数を返す。 */
u32 CTimerManager::ActiveCount() const noexcept {
    return m_ActiveCount;
}

} // namespace acs
