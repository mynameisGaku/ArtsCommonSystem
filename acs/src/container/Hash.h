// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — ハッシュ関数群
// -----------------------------------------------------------------------------
// バイト列用の汎用ハッシュ + 整数 / ポインタ用の高速 finalizer。
// THashMap や Set の既定ハッシュとして使用。
//
// 汎用バイトハッシュは xxhash-likes のシンプル版（速度・品質ともに上位）。
// 整数 / ポインタは Murmur fmix64 系の 1 mul + 2 xor-shift で混ぜる。
//
// THashMap 側は「これらの結果は十分混ざっている」と仮定するため、再 mix を
// しない。is_avalanching の前提を破ると分布が悪化するので、独自の THasher
// 特殊化を追加する場合は注意すること。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "container/StringView.h"

namespace acs {

// 64bit 値を強く混ぜる finalizer。Murmur3 fmix64 と等価。
// 整数キー / ポインタキー用。
ACS_FORCEINLINE u64 HashMix64(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

// 任意バイト列のハッシュ。SMHasher テストで上位品質。
u64 HashBytes(const void* data, usize len, u64 seed = 0xCBF29CE484222325ull) noexcept;

// 既定 THasher（特殊化しない型に対しては未定義 → コンパイルエラー）
template<typename T> struct THasher;

// 整数型特殊化（mix64 による高品質ハッシュ）
template<> struct THasher<u8>  { ACS_FORCEINLINE u64 operator()(u8  v) const noexcept { return HashMix64(v); } };
template<> struct THasher<u16> { ACS_FORCEINLINE u64 operator()(u16 v) const noexcept { return HashMix64(v); } };
template<> struct THasher<u32> { ACS_FORCEINLINE u64 operator()(u32 v) const noexcept { return HashMix64(v); } };
template<> struct THasher<u64> { ACS_FORCEINLINE u64 operator()(u64 v) const noexcept { return HashMix64(v); } };
template<> struct THasher<i8>  { ACS_FORCEINLINE u64 operator()(i8  v) const noexcept { return HashMix64((u64)v); } };
template<> struct THasher<i16> { ACS_FORCEINLINE u64 operator()(i16 v) const noexcept { return HashMix64((u64)v); } };
template<> struct THasher<i32> { ACS_FORCEINLINE u64 operator()(i32 v) const noexcept { return HashMix64((u64)v); } };
template<> struct THasher<i64> { ACS_FORCEINLINE u64 operator()(i64 v) const noexcept { return HashMix64((u64)v); } };

// ポインタ特殊化
template<typename T>
struct THasher<T*> {
    ACS_FORCEINLINE u64 operator()(T* p) const noexcept {
        return HashMix64(reinterpret_cast<u64>(p));
    }
};

// FStringView 特殊化（バイト列ハッシュ）
template<> struct THasher<FStringView> {
    ACS_FORCEINLINE u64 operator()(FStringView s) const noexcept {
        return HashBytes(s.Data(), s.Size());
    }
};

} // namespace acs
