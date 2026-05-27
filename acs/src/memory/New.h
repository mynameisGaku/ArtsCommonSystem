// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FAllocator 経由の new/delete ヘルパ
// -----------------------------------------------------------------------------
// グローバル new/delete を使わず、FAllocator から確保 → 配置 new でコンストラクタ
// 呼び出し → デストラクタ呼び出し → FAllocator に返す、という流れを 1 関数化。
//
// 例:
//   MyObj* p = New<MyObj>(allocator, args...);
//   ...
//   Delete(allocator, p);
// =============================================================================
#pragma once

#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "memory/Allocator.h"

namespace acs {

// 単一オブジェクトの構築（コンストラクタ引数を完全転送）
template<typename T, typename... Args>
ACS_FORCEINLINE T* New(FAllocator& a, Args&&... args) noexcept {
    void* p = a.Alloc(sizeof(T), alignof(T), FSourceLoc::Current());
    if (!p) return nullptr;
    return ::new (p) T(Forward<Args>(args)...);  // 配置 new
}

// 単一オブジェクトの破棄（デストラクタ呼び出し → Free）
// トリビアル破棄可能型は ~T() を省略（最適化）。
template<typename T>
ACS_FORCEINLINE void Delete(FAllocator& a, T* p) noexcept {
    if (!p) return;
    if constexpr (!IsTriviallyDestructibleV<T>) p->~T();
    a.Free(static_cast<void*>(p));
}

// 配列構築（n 個分まとめて確保 → 各要素をデフォルト構築）
template<typename T>
ACS_FORCEINLINE T* NewArray(FAllocator& a, usize n) noexcept {
    if (n == 0) return nullptr;
    void* p = a.Alloc(sizeof(T) * n, alignof(T), FSourceLoc::Current());
    if (!p) return nullptr;
    T* arr = static_cast<T*>(p);
    for (usize i = 0; i < n; ++i) ::new (&arr[i]) T();
    return arr;
}

// 配列破棄（後ろから順にデストラクタ呼び出し → 一括 Free）
template<typename T>
ACS_FORCEINLINE void DeleteArray(FAllocator& a, T* arr, usize n) noexcept {
    if (!arr) return;
    if constexpr (!IsTriviallyDestructibleV<T>) {
        for (usize i = n; i-- > 0;) arr[i].~T();
    }
    a.Free(static_cast<void*>(arr));
}

} // namespace acs
