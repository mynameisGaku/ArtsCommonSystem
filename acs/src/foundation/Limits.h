// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — 数値型の最小／最大値（<limits> 代替）
// -----------------------------------------------------------------------------
// STL 禁止のため std::numeric_limits を使えない。代わりに TNumLimits<T> を
// テンプレート特殊化で提供する。Min / Max / Epsilon / Infinity を提供。
// すべて constexpr。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs {

// ---- 一般テンプレート（特殊化されていない型へのアクセスはコンパイルエラー） ----
template<typename T> struct TNumLimits;

// ---- 符号なし整数 ----
template<> struct TNumLimits<u8>  { static constexpr u8  Min() { return 0; } static constexpr u8  Max() { return 0xFFu; } };
template<> struct TNumLimits<u16> { static constexpr u16 Min() { return 0; } static constexpr u16 Max() { return 0xFFFFu; } };
template<> struct TNumLimits<u32> { static constexpr u32 Min() { return 0; } static constexpr u32 Max() { return 0xFFFFFFFFu; } };
template<> struct TNumLimits<u64> { static constexpr u64 Min() { return 0; } static constexpr u64 Max() { return 0xFFFFFFFFFFFFFFFFull; } };

// ---- 符号付き整数 ----
// 注: Min は -MAX-1 として書くことで「-N が型に収まらない」UB を回避する。
template<> struct TNumLimits<i8>  { static constexpr i8  Min() { return -127 - 1; }                     static constexpr i8  Max() { return 127; } };
template<> struct TNumLimits<i16> { static constexpr i16 Min() { return -32767 - 1; }                   static constexpr i16 Max() { return 32767; } };
template<> struct TNumLimits<i32> { static constexpr i32 Min() { return -2147483647 - 1; }              static constexpr i32 Max() { return 2147483647; } };
template<> struct TNumLimits<i64> { static constexpr i64 Min() { return -9223372036854775807ll - 1; }   static constexpr i64 Max() { return 9223372036854775807ll; } };

// ---- 浮動小数点 ----
// IEEE 754 の最小正規化数 / 最大値 / マシンイプシロン / 無限大を提供する。
// Infinity は __builtin_huge_val* を使用（HUGE_VALF はマクロで constexpr 不可）。
template<> struct TNumLimits<f32> {
    static constexpr f32 Min()      { return 1.175494351e-38f; }    // 最小正の正規化数
    static constexpr f32 Max()      { return 3.402823466e+38f; }    // 最大有限値
    static constexpr f32 Epsilon()  { return 1.192092896e-07f; }    // 1.0 と次の表現可能値の差
    static constexpr f32 Infinity() { return __builtin_huge_valf(); }
};
template<> struct TNumLimits<f64> {
    static constexpr f64 Min()      { return 2.2250738585072014e-308; }
    static constexpr f64 Max()      { return 1.7976931348623158e+308; }
    static constexpr f64 Epsilon()  { return 2.2204460492503131e-016; }
    static constexpr f64 Infinity() { return __builtin_huge_val(); }
};

} // namespace acs
