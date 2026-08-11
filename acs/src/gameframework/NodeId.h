// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * シーングラフ ANode を識別する packed 32bit handle (generational)。
 *
 * @details
 * 1 個の u32 に low24=index + high8=generation を pack する POD handle。
 * packed == 0 をそのまま invalid とし、FNodeId() の既定構築と一致させる。
 * 全関数 constexpr noexcept のヘッダオンリ型。
 */
struct FNodeId {
    /** pack 済みの 32bit 値 (0 = invalid、layout: low24=index, high8=generation)。 */
    u32 m_Packed = 0;

    /** 既定構築 = invalid handle (packed == 0)。 */
    constexpr FNodeId() noexcept = default;

    /**
     * index (24bit) と generation (8bit) を pack して構築する。
     *
     * @details index は & 0x00FFFFFFu でマスクされるため、24bit 超の値は上位が落ちる。
     * @param index pool / SoA 配列の index (0 〜 16,777,215)。
     * @param gen slot の世代カウンタ (0 〜 255)。
     */
    constexpr FNodeId(u32 index, u8 gen) noexcept
        : m_Packed((index & 0x00FFFFFFu) | (static_cast<u32>(gen) << 24)) {}

    /**
     * pool / SoA 配列の index を取り出す。
     *
     * @return packed の low24 bit (0 〜 16,777,215)。
     */
    constexpr u32  Index() const noexcept { return m_Packed & 0x00FFFFFFu; }

    /**
     * slot の世代カウンタを取り出す (stale handle 検出用)。
     *
     * @return packed の high8 bit (0 〜 255)。
     */
    constexpr u8   Generation() const noexcept {
        return static_cast<u8>(m_Packed >> 24);
    }

    /**
     * invalid (= packed == 0) でなければ true を返す。
     *
     * @details 「pool に該当 slot が生きているか」は呼び出し側で別途検証すること。
     * @return packed != 0 なら true。
     */
    constexpr bool IsValid() const noexcept { return m_Packed != 0; }

    /**
     * 完全一致比較 (index + generation の両方が一致した時のみ true)。
     *
     * @param o 比較相手のハンドル。
     * @return packed が一致すれば true。
     */
    constexpr bool operator==(FNodeId o) const noexcept { return m_Packed == o.m_Packed; }

    /**
     * 不一致比較 (index または generation が違えば true)。
     *
     * @param o 比較相手のハンドル。
     * @return packed が不一致なら true。
     */
    constexpr bool operator!=(FNodeId o) const noexcept { return m_Packed != o.m_Packed; }
};

} // namespace acs::game
