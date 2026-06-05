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

/**
 * 値を rvalue 参照にキャストし、ムーブ可能にする (std::move 相当)。
 *
 * @details コピーは発生せず、`T b = Move(a);` で a の中身を b に移譲できる。
 * @tparam T 渡された引数の (推論された) 型。
 * @param v ムーブ元の値。
 * @return v を指す rvalue 参照。
 */
template<typename T>
ACS_FORCEINLINE constexpr RemoveRefT<T>&& Move(T&& v) noexcept {
    return static_cast<RemoveRefT<T>&&>(v);
}

/**
 * lvalue を完全転送する (perfect forwarding、std::forward 相当)。
 *
 * @details テンプレート関数で受け取った引数を、元のカテゴリ (lvalue / rvalue) を
 * 保持したまま別の関数へ渡す。lvalue 版。
 * @tparam T 転送先で復元する元の型。
 * @param v 転送する lvalue。
 * @return T のカテゴリで転送される参照。
 */
template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>& v) noexcept {
    return static_cast<T&&>(v);
}

/**
 * rvalue を完全転送する (perfect forwarding、std::forward 相当)。
 *
 * @details rvalue を lvalue として転送するのは安全でないため、T が lvalue 参照の
 * ときは static_assert でコンパイル時に弾く。
 * @tparam T 転送先で復元する元の型。
 * @param v 転送する rvalue。
 * @return T のカテゴリで転送される rvalue 参照。
 */
template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>&& v) noexcept {
    // rvalue を lvalue として転送するのは安全でないため、コンパイル時に検出。
    static_assert(!IsLvalueRefV<T>, "Cannot forward an rvalue as an lvalue.");
    return static_cast<T&&>(v);
}

/**
 * 二つの値をムーブで入れ替える (例外なし版、std::swap 相当)。
 *
 * @tparam T 入れ替える値の型。
 * @param a 入れ替える一方への参照。
 * @param b 入れ替えるもう一方への参照。
 */
template<typename T>
ACS_FORCEINLINE constexpr void Swap(T& a, T& b) noexcept {
    T tmp = static_cast<T&&>(a);
    a     = static_cast<T&&>(b);
    b     = static_cast<T&&>(tmp);
}

/**
 * 二つの値のうち小さい方を返す (三項演算子の constexpr 版)。
 *
 * @tparam T 比較する値の型。
 * @param a 比較する一方。
 * @param b 比較するもう一方。
 * @return a と b のうち小さい方 (等しければ a)。
 */
template<typename T> ACS_FORCEINLINE constexpr T Min(T a, T b) noexcept { return a < b ? a : b; }

/**
 * 二つの値のうち大きい方を返す (三項演算子の constexpr 版)。
 *
 * @tparam T 比較する値の型。
 * @param a 比較する一方。
 * @param b 比較するもう一方。
 * @return a と b のうち大きい方 (等しければ a)。
 */
template<typename T> ACS_FORCEINLINE constexpr T Max(T a, T b) noexcept { return a > b ? a : b; }

/**
 * 値を [lo, hi] の範囲に丸める。
 *
 * @tparam T 丸める値の型。
 * @param v 丸める対象の値。
 * @param lo 下限。
 * @param hi 上限。
 * @return v が lo 未満なら lo、hi 超過なら hi、それ以外は v。
 */
template<typename T> ACS_FORCEINLINE constexpr T Clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace acs

// 配置 new (placement new)
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

    /**
     * 配置 new (単体)。与えられたポインタにオブジェクトを構築する。
     *
     * @param p 構築先として受け取った領域 (そのまま返す)。
     * @return p (確保は行わず受け取った領域を返すだけ)。
     */
    inline void* operator new  (acs::usize, void* p) noexcept { return p; }

    /**
     * 配置 new[] (配列)。与えられたポインタに配列を構築する。
     *
     * @param p 構築先として受け取った領域 (そのまま返す)。
     * @return p (確保は行わず受け取った領域を返すだけ)。
     */
    inline void* operator new[](acs::usize, void* p) noexcept { return p; }

    /**
     * 配置 new に対応する placement delete (単体)。何もしない。
     *
     * @details 構築中に例外が出た場合に対で呼ばれるが、ACS は例外を使わないため空。
     */
    inline void  operator delete  (void*, void*) noexcept {}

    /**
     * 配置 new[] に対応する placement delete[] (配列)。何もしない。
     *
     * @details 構築中に例外が出た場合に対で呼ばれるが、ACS は例外を使わないため空。
     */
    inline void  operator delete[](void*, void*) noexcept {}
#endif
