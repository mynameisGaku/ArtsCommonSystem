// SPDX-License-Identifier: Apache-2.0
// ACS Memory — FAllocator 経由の new/delete ヘルパ
//
// グローバル new/delete を使わず、FAllocator から確保 → 配置 new でコンストラクタ
// 呼び出し → デストラクタ呼び出し → FAllocator に返す、という流れを 1 関数化。
//
// 例:
//   FMyObj* p = New<FMyObj>(allocator, args...);
//   ...
//   Delete(allocator, p);
#pragma once

#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "memory/Allocator.h"

namespace acs {

/**
 * FAllocator から確保した領域に単一オブジェクトを構築する。
 *
 * @details a.Alloc で sizeof(T)/alignof(T) を確保し、配置 new で T を構築する。
 * コンストラクタ引数は完全転送する。
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param a 確保に使うアロケータ。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した T へのポインタ。確保失敗時は nullptr。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE T* New(FAllocator& a, Args&&... args) noexcept {
    void* const p = a.Alloc(sizeof(T), alignof(T), FSourceLoc::Current());
    if (!p) return nullptr;
    return ::new (p) T(Forward<Args>(args)...);  // 配置 new
}

/**
 * New で確保した単一オブジェクトを破棄して領域を返す。
 *
 * @details デストラクタを呼んだ後に a.Free する。トリビアル破棄可能型は ~T() を省略する。
 * @tparam T 破棄するオブジェクト型。
 * @param a New 時に使ったアロケータ。
 * @param p 破棄する対象 (nullptr は no-op)。
 */
template<typename T>
ACS_FORCEINLINE void Delete(FAllocator& a, T* p) noexcept {
    if (!p) return;
    if constexpr (!IsTriviallyDestructibleV<T>) p->~T();
    a.Free(static_cast<void*>(p));
}

/**
 * n 要素の配列をまとめて確保し各要素をデフォルト構築する。
 *
 * @details sizeof(T)*n のオーバーフローを検出し、その場合や n==0 では nullptr を返す。
 * @tparam T 配列要素の型。
 * @param a 確保に使うアロケータ。
 * @param n 確保する要素数。
 * @return 配列先頭へのポインタ。n==0・オーバーフロー・確保失敗時は nullptr。
 */
template<typename T>
ACS_FORCEINLINE T* NewArray(FAllocator& a, usize n) noexcept {
    if (n == 0) return nullptr;
    if (n > (~usize(0)) / sizeof(T)) return nullptr;  // sizeof(T)*n のラップ → 失敗
    void* const p = a.Alloc(sizeof(T) * n, alignof(T), FSourceLoc::Current());
    if (!p) return nullptr;
    T* const arr = static_cast<T*>(p);
    for (usize i = 0; i < n; ++i) ::new (&arr[i]) T();
    return arr;
}

/**
 * NewArray で確保した配列を破棄して領域を返す。
 *
 * @details 各要素を後ろから順にデストラクタ呼び出ししてから一括 Free する。
 * トリビアル破棄可能型はデストラクタ呼び出しを省略する。
 * @tparam T 配列要素の型。
 * @param a NewArray 時に使ったアロケータ。
 * @param arr 破棄する配列先頭 (nullptr は no-op)。
 * @param n 配列の要素数。
 */
template<typename T>
ACS_FORCEINLINE void DeleteArray(FAllocator& a, T* arr, usize n) noexcept {
    if (!arr) return;
    if constexpr (!IsTriviallyDestructibleV<T>) {
        for (usize i = n; i-- > 0;) arr[i].~T();
    }
    a.Free(static_cast<void*>(arr));
}

} // namespace acs
