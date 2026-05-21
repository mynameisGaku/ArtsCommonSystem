// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — Move / Forward / Swap（<utility> 代替）
// -----------------------------------------------------------------------------
// std::move / std::forward / std::swap を独自実装する。コンパイラ最適化で
// 完全に消える inline 関数のみで構成。
//
// 加えて、配置 new (placement new) のグローバル演算子を <new> を取り込まず
// 自前宣言する。プレースメント new は言語機能の一部だが、形式的には <new> で
// 宣言されるためここで先回りする。
// =============================================================================
#pragma once

#include "foundation/Compiler.h"     // ACS_FORCEINLINE
#include "foundation/TypeTraits.h"

namespace acs {

// ---- Move: 値を rvalue 参照にキャストし、ムーブ可能にする ----------------
// 例: T b = Move(a);  // a の中身を b に移譲（コピーは発生しない）
template<typename T>
ACS_FORCEINLINE constexpr RemoveRefT<T>&& Move(T&& v) noexcept {
    return static_cast<RemoveRefT<T>&&>(v);
}

// ---- Forward: 完全転送（perfect forwarding） -----------------------------
// テンプレート関数で受け取った引数を「元のカテゴリ（lvalue / rvalue）」を
// 保持したまま別の関数に渡す。
template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>& v) noexcept {
    return static_cast<T&&>(v);
}

template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>&& v) noexcept {
    // rvalue を lvalue として転送するのは安全でないため、コンパイル時に検出。
    static_assert(!IsLvalueRefV<T>, "Cannot forward an rvalue as an lvalue.");
    return static_cast<T&&>(v);
}

// ---- Swap: 二つの値を入れ替える（ムーブを使った例外なし版） --------------
template<typename T>
ACS_FORCEINLINE constexpr void Swap(T& a, T& b) noexcept {
    T tmp = static_cast<T&&>(a);
    a     = static_cast<T&&>(b);
    b     = static_cast<T&&>(tmp);
}

// ---- Min / Max / Clamp（<algorithm> 代替） -------------------------------
// 整数 / 浮動小数双方で動作する単純な比較関数。三項演算子の constexpr 版。
template<typename T> ACS_FORCEINLINE constexpr T Min(T a, T b) noexcept { return a < b ? a : b; }
template<typename T> ACS_FORCEINLINE constexpr T Max(T a, T b) noexcept { return a > b ? a : b; }
template<typename T> ACS_FORCEINLINE constexpr T Clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace acs

// ---- 配置 new (placement new) ---------------------------------------------
// 標準では <new> ヘッダで宣言されるが、STL 禁止方針のため自前宣言する。
// `::new (ptr) T(...)` 構文を機能させるために必須。
//
// MSVC の vcruntime_new.h は同じ placement new を sentinel
// `__PLACEMENT_NEW_INLINE` で多重防止しているので、それを共有する。
//   - vcruntime が先に include されたら __PLACEMENT_NEW_INLINE が立ち、
//     ACS の以下の定義はスキップされる（vcruntime の定義をそのまま使う）
//   - vcruntime より先に Move.h が読まれた場合は ACS が定義し、
//     後から来る vcruntime はスキップする（sentinel を立てるため）
#if !defined(ACS_PLACEMENT_NEW_DEFINED) && !defined(__PLACEMENT_NEW_INLINE) && !defined(__PLACEMENT_VEC_NEW_INLINE)
    #define ACS_PLACEMENT_NEW_DEFINED 1
    #define __PLACEMENT_NEW_INLINE          // 単体 new/delete の sentinel
    #define __PLACEMENT_VEC_NEW_INLINE      // 配列 new[]/delete[] の sentinel
    inline void* operator new  (acs::usize, void* p) noexcept { return p; }
    inline void* operator new[](acs::usize, void* p) noexcept { return p; }
    inline void  operator delete  (void*, void*) noexcept {}
    inline void  operator delete[](void*, void*) noexcept {}
#endif
