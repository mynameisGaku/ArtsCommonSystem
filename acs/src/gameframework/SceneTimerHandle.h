// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"

namespace acs::game {

/**
 * CSceneTimer が管理するタイマーを識別する 32bit の世代付きハンドル。
 *
 * @details 下位 24bit にスロット番号、上位 8bit に世代番号を保持する。
 * m_Packed == 0 を無効値とし、スロット再利用後の古い参照は世代番号で拒否する。
 */
struct FSceneTimerHandle {
    /** スロット番号と世代番号を詰めた値。0 は無効値。 */
    u32 m_Packed = 0u;

    /** スロット番号に割り当てるビット数。 */
    static constexpr u32 kIndexBits = 24u;

    /** スロット番号を取り出すマスク。 */
    static constexpr u32 kIndexMask = (1u << kIndexBits) - 1u;

    /** スロット番号の予約済み上限値。 */
    static constexpr u32 kMaxIndex = kIndexMask;

    /**
     * ハンドルが無効値でないかを返す。
     *
     * @return m_Packed が 0 でなければ true。
     */
    constexpr bool IsValid() const noexcept
    {
        return m_Packed != 0u;
    }

    /**
     * スロット番号と世代番号からハンドルを組み立てる。
     *
     * @param index 下位 24bit に格納するスロット番号。
     * @param gen 上位 8bit に格納する世代番号。
     * @return 指定値を詰めたハンドル。
     */
    static constexpr FSceneTimerHandle Pack(u32 index, u8 gen) noexcept
    {
        /** 組み立てるハンドル。 */
        FSceneTimerHandle handle{};
        handle.m_Packed = (static_cast<u32>(gen) << kIndexBits) | (index & kIndexMask);
        return handle;
    }

    /**
     * ハンドルからスロット番号を取り出す。
     *
     * @return 下位 24bit のスロット番号。
     */
    constexpr u32 Index() const noexcept
    {
        return m_Packed & kIndexMask;
    }

    /**
     * ハンドルから世代番号を取り出す。
     *
     * @return 上位 8bit の世代番号。
     */
    constexpr u8 Gen() const noexcept
    {
        return static_cast<u8>(m_Packed >> kIndexBits);
    }
};

/**
 * FSceneTimerHandle の旧ソース互換名。
 *
 * @details ACS 0.x の移行期間だけ残し、ACS 1.0 で削除する。削除条件は、リポジトリ内の
 * 実利用と配布用 consumer の旧名利用が 0 件になり、移行案内を 1 release 継続したこと。
 * 型名変更で CSceneTimer の修飾シンボルが変わるため binary ABI は維持しない。既存 consumer は
 * ACS ライブラリと同時に再ビルドする必要がある。新規コードでは FSceneTimerHandle を使う。
 */
using FTimerHandle [[deprecated("ACS 1.0 で削除予定です。FSceneTimerHandle を使用してください。")]] = FSceneTimerHandle;

} // namespace acs::game
