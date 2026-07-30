// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * 固定容量のディスクリプタ番号を再利用する、確保を伴わないスロットプール。
 *
 * @details 同期とバッチ確保の契約を次に示す。
 * 外側で同期することを前提とする。バッチ確保は全件成功または全件失敗であり、
 * 途中失敗による返却処理を呼び出し側へ要求しない。
 */
template<u32 Capacity>
class TDescriptorSlotPool {
    static_assert(Capacity > 0u);

public:
    /** 一つのスロットを確保し、空きがなければ -1 を返す。 */
    i32 Allocate() noexcept {
        /** 確保したスロット番号。 */
        i32 slot = -1;
        return AllocateBatch(&slot, 1u) ? slot : -1;
    }

    /**
     * 指定数のスロットをまとめて確保する。
     *
     * @param output 確保した番号を書き込む配列。
     * @param count 確保するスロット数。
     * @return 全件を確保できれば true。出力先不正または空き不足では false。
     */
    bool AllocateBatch(i32* output, u32 count) noexcept {
        if (count == 0u) return true;
        if (output == nullptr || count > AvailableCount()) return false;
        /** 出力済みスロット数。 */
        u32 written = 0u;
        while (written < count && m_FreeCount > 0u)
            output[written++] = m_FreeList[--m_FreeCount];
        while (written < count)
            output[written++] = static_cast<i32>(m_HighWater++);
        return true;
    }

    /**
     * 一つのスロットを再利用待ちへ戻す。
     *
     * @param slot 返却するスロット番号。範囲外または返却済みなら変更しない。
     */
    void Free(i32 slot) noexcept {
        if (slot < 0 || static_cast<u32>(slot) >= m_HighWater) return;
        /** 重複返却を確認する再利用待ち位置。 */
        for (u32 i = 0u; i < m_FreeCount; ++i) {
            if (m_FreeList[i] == slot) return;
        }
        if (m_FreeCount < Capacity) m_FreeList[m_FreeCount++] = slot;
    }

    /**
     * 指定されたスロット群を再利用待ちへ戻す。
     *
     * @param slots 返却するスロット番号の配列。
     * @param count 配列要素数。slots が空なら変更しない。
     */
    void FreeBatch(const i32* slots, u32 count) noexcept {
        if (slots == nullptr) return;
        /** 返却する配列位置。 */
        for (u32 i = 0u; i < count; ++i) Free(slots[i]);
    }

    /** 新規に発行済みの最大位置を返す。 */
    u32 HighWater() const noexcept { return m_HighWater; }
    /** 再利用待ちのスロット数を返す。 */
    u32 FreeCount() const noexcept { return m_FreeCount; }
    /** 現在確保できるスロット数を返す。 */
    u32 AvailableCount() const noexcept {
        return m_FreeCount + (Capacity - m_HighWater);
    }

    /** 全スロットを未発行状態へ戻す。 */
    void Reset() noexcept {
        m_HighWater = 0u;
        m_FreeCount = 0u;
    }

private:
    /** 再利用待ちスロットを保持するスタック。 */
    i32 m_FreeList[Capacity]{};
    /** 連続領域から次に発行する位置。 */
    u32 m_HighWater = 0u;
    /** 再利用待ちスロット数。 */
    u32 m_FreeCount = 0u;
};

} // namespace acs
