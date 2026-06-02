// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — Rc.h【非推奨・互換シム】
// -----------------------------------------------------------------------------
// TRc<T> / MakeRc / MakeRcIn は TSharedPtr<T> / MakeShared / MakeSharedIn に
// 改名された。本ヘッダは旧名で書かれた既存コードを壊さないためのエイリアス。
// 新規コードでは memory/SharedPtr.h の TSharedPtr / MakeShared を直接使うこと。
// =============================================================================
#pragma once

#include "memory/SharedPtr.h"

namespace acs {

// 旧名 → 新名のエイリアス（非推奨）
template<typename T> using TRc = TSharedPtr<T>;

template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRc(Args&&... args) noexcept {
    return MakeShared<T>(Forward<Args>(args)...);
}

template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRcIn(FAllocator& a, Args&&... args) noexcept {
    return MakeSharedIn<T>(a, Forward<Args>(args)...);
}

} // namespace acs
