// SPDX-License-Identifier: Apache-2.0
// ACS Threading — スレッド ID
//
// 軽量な型として FThreadId 構造体を提供。OS スレッド ID（DWORD）を u32 で保持。
// std::thread::id 相当だが POD で扱いやすい。
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * OS スレッド ID を保持する POD 型 (OS の DWORD と等価、比較可能)。
 *
 * @details std::thread::id 相当だが POD で扱いやすく、ハッシュキーにも使える。
 */
struct FThreadId {
    /** OS スレッド ID の生の値 (Windows の DWORD)。 */
    u32 raw = 0;

    /**
     * 2 つのスレッド ID が等しいかを返す。
     *
     * @param o 比較相手。
     * @return 同じスレッド ID なら true。
     */
    constexpr bool operator==(FThreadId o) const noexcept { return raw == o.raw; }

    /**
     * 2 つのスレッド ID が異なるかを返す。
     *
     * @param o 比較相手。
     * @return 異なるスレッド ID なら true。
     */
    constexpr bool operator!=(FThreadId o) const noexcept { return raw != o.raw; }
};

/**
 * 現在のスレッドの ID を返す (GetCurrentThreadId のラッパ)。
 *
 * @return 呼び出しスレッドの FThreadId。
 */
FThreadId CurrentThreadId() noexcept;

} // namespace acs
