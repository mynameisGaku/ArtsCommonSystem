// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "container/StringView.h"

namespace acs {

/**
 * 64bit 値を強く混ぜる finalizer (Murmur3 fmix64 と等価)。
 *
 * @details 整数キー / ポインタキー用。1 回の乗算 + xor-shift を 2 段で混ぜる。
 * @param x 混ぜる入力値。
 * @return アバランチ処理した 64bit ハッシュ。
 */
ACS_FORCEINLINE u64 HashMix64(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

/**
 * 任意バイト列の 64bit ハッシュを計算する (xxhash 風、SMHasher 上位品質)。
 *
 * @param data ハッシュ対象の先頭ポインタ。nullptr は len==0 のみ正規入力。
 * @param len バイト長。
 * @param seed 初期シード (既定は FNV offset basis)。
 * @return 64bit ハッシュ値。
 */
u64 HashBytes(const void* data, usize len, u64 seed = 0xCBF29CE484222325ull) noexcept;

/**
 * 既定ハッシュ functor のプライマリテンプレート (特殊化しない型は未定義 → コンパイルエラー)。
 *
 * @tparam T ハッシュ対象のキー型。
 */
template<typename T> struct THasher;

/** u8 キー用ハッシュ functor (HashMix64 による高品質ハッシュ)。 */
template<> struct THasher<u8>  { ACS_FORCEINLINE u64 operator()(u8  v) const noexcept { return HashMix64(v); } };

/** u16 キー用ハッシュ functor (HashMix64 による高品質ハッシュ)。 */
template<> struct THasher<u16> { ACS_FORCEINLINE u64 operator()(u16 v) const noexcept { return HashMix64(v); } };

/** u32 キー用ハッシュ functor (HashMix64 による高品質ハッシュ)。 */
template<> struct THasher<u32> { ACS_FORCEINLINE u64 operator()(u32 v) const noexcept { return HashMix64(v); } };

/** u64 キー用ハッシュ functor (HashMix64 による高品質ハッシュ)。 */
template<> struct THasher<u64> { ACS_FORCEINLINE u64 operator()(u64 v) const noexcept { return HashMix64(v); } };

/** i8 キー用ハッシュ functor (u64 化して HashMix64)。 */
template<> struct THasher<i8>  { ACS_FORCEINLINE u64 operator()(i8  v) const noexcept { return HashMix64((u64)v); } };

/** i16 キー用ハッシュ functor (u64 化して HashMix64)。 */
template<> struct THasher<i16> { ACS_FORCEINLINE u64 operator()(i16 v) const noexcept { return HashMix64((u64)v); } };

/** i32 キー用ハッシュ functor (u64 化して HashMix64)。 */
template<> struct THasher<i32> { ACS_FORCEINLINE u64 operator()(i32 v) const noexcept { return HashMix64((u64)v); } };

/** i64 キー用ハッシュ functor (u64 化して HashMix64)。 */
template<> struct THasher<i64> { ACS_FORCEINLINE u64 operator()(i64 v) const noexcept { return HashMix64((u64)v); } };

/**
 * ポインタキー用ハッシュ functor (アドレスを HashMix64 で混ぜる)。
 *
 * @tparam T ポインタの指す型。
 */
template<typename T>
struct THasher<T*> {
    /**
     * ポインタアドレスのハッシュを返す。
     *
     * @param p ハッシュ対象のポインタ。
     * @return アドレスを混ぜた 64bit ハッシュ。
     */
    ACS_FORCEINLINE u64 operator()(T* p) const noexcept {
        return HashMix64(reinterpret_cast<u64>(p));
    }
};

/** FStringView キー用ハッシュ functor (HashBytes によるバイト列ハッシュ)。 */
template<> struct THasher<FStringView> {
    /**
     * 文字列バイト列のハッシュを返す。
     *
     * @param s ハッシュ対象のビュー。
     * @return バイト列を混ぜた 64bit ハッシュ。
     */
    ACS_FORCEINLINE u64 operator()(FStringView s) const noexcept {
        return HashBytes(s.Data(), s.Size());
    }
};

} // namespace acs
