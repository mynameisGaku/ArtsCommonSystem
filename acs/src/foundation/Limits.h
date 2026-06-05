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

/**
 * 数値型 T の最小／最大値などを提供する型特性 (std::numeric_limits 代替)。
 *
 * @details
 * 一般テンプレートは宣言のみで定義を持たないため、特殊化されていない型に
 * アクセスするとコンパイルエラーになる。各組み込み数値型ごとに特殊化を提供する。
 * @tparam T 限界値を問い合わせる数値型。
 */
template<typename T> struct TNumLimits;

/** u8 の限界値特性 (Min=0、Max=0xFF)。 */
template<> struct TNumLimits<u8>  { static constexpr u8  Min() { return 0; } static constexpr u8  Max() { return 0xFFu; } };

/** u16 の限界値特性 (Min=0、Max=0xFFFF)。 */
template<> struct TNumLimits<u16> { static constexpr u16 Min() { return 0; } static constexpr u16 Max() { return 0xFFFFu; } };

/** u32 の限界値特性 (Min=0、Max=0xFFFFFFFF)。 */
template<> struct TNumLimits<u32> { static constexpr u32 Min() { return 0; } static constexpr u32 Max() { return 0xFFFFFFFFu; } };

/** u64 の限界値特性 (Min=0、Max=0xFFFFFFFFFFFFFFFF)。 */
template<> struct TNumLimits<u64> { static constexpr u64 Min() { return 0; } static constexpr u64 Max() { return 0xFFFFFFFFFFFFFFFFull; } };

/**
 * i8 の限界値特性 (Min=-128、Max=127)。
 *
 * @details Min は -127-1 として書き「-N が型に収まらない」UB を回避する。
 */
template<> struct TNumLimits<i8>  { static constexpr i8  Min() { return -127 - 1; }                     static constexpr i8  Max() { return 127; } };

/**
 * i16 の限界値特性 (Min=-32768、Max=32767)。
 *
 * @details Min は -32767-1 として書き「-N が型に収まらない」UB を回避する。
 */
template<> struct TNumLimits<i16> { static constexpr i16 Min() { return -32767 - 1; }                   static constexpr i16 Max() { return 32767; } };

/**
 * i32 の限界値特性 (Min=-2147483648、Max=2147483647)。
 *
 * @details Min は -2147483647-1 として書き「-N が型に収まらない」UB を回避する。
 */
template<> struct TNumLimits<i32> { static constexpr i32 Min() { return -2147483647 - 1; }              static constexpr i32 Max() { return 2147483647; } };

/**
 * i64 の限界値特性 (Min=-9223372036854775808、Max=9223372036854775807)。
 *
 * @details Min は -9223372036854775807-1 として書き「-N が型に収まらない」UB を回避する。
 */
template<> struct TNumLimits<i64> { static constexpr i64 Min() { return -9223372036854775807ll - 1; }   static constexpr i64 Max() { return 9223372036854775807ll; } };

/**
 * f32 の限界値特性 (最小正規化数・最大有限値・イプシロン・無限大)。
 *
 * @details Infinity は __builtin_huge_valf を使う (HUGE_VALF はマクロで constexpr 不可)。
 */
template<> struct TNumLimits<f32> {
    /** 最小の正の正規化数を返す。 */
    static constexpr f32 Min()      { return 1.175494351e-38f; }

    /** 最大の有限値を返す。 */
    static constexpr f32 Max()      { return 3.402823466e+38f; }

    /** 1.0 と次に表現可能な値との差 (マシンイプシロン) を返す。 */
    static constexpr f32 Epsilon()  { return 1.192092896e-07f; }

    /** 正の無限大を返す。 */
    static constexpr f32 Infinity() { return __builtin_huge_valf(); }
};

/**
 * f64 の限界値特性 (最小正規化数・最大有限値・イプシロン・無限大)。
 *
 * @details Infinity は __builtin_huge_val を使う (HUGE_VAL はマクロで constexpr 不可)。
 */
template<> struct TNumLimits<f64> {
    /** 最小の正の正規化数を返す。 */
    static constexpr f64 Min()      { return 2.2250738585072014e-308; }

    /** 最大の有限値を返す。 */
    static constexpr f64 Max()      { return 1.7976931348623158e+308; }

    /** 1.0 と次に表現可能な値との差 (マシンイプシロン) を返す。 */
    static constexpr f64 Epsilon()  { return 2.2204460492503131e-016; }

    /** 正の無限大を返す。 */
    static constexpr f64 Infinity() { return __builtin_huge_val(); }
};

} // namespace acs
