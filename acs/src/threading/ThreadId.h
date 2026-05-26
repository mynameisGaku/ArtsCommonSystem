// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — スレッド ID
// -----------------------------------------------------------------------------
// 軽量な型として ThreadId 構造体を提供。OS スレッド ID（DWORD）を u32 で保持。
// std::thread::id 相当だが POD で扱いやすい。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs {

// スレッド ID（OS の DWORD と等価、ハッシュ可能、比較可能）
struct ThreadId {
    u32 raw = 0;
    constexpr bool operator==(ThreadId o) const noexcept { return raw == o.raw; }
    constexpr bool operator!=(ThreadId o) const noexcept { return raw != o.raw; }
};

// 現在のスレッド ID を取得（GetCurrentThreadId のラッパ）
ThreadId CurrentThreadId() noexcept;

} // namespace acs
