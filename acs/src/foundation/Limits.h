// ACS Foundation — Numeric limits replacements (no <limits>).
#pragma once

#include "foundation/Types.h"

namespace acs {

template<typename T> struct NumLimits;

template<> struct NumLimits<u8>  { static constexpr u8  Min() { return 0; } static constexpr u8  Max() { return 0xFFu; } };
template<> struct NumLimits<u16> { static constexpr u16 Min() { return 0; } static constexpr u16 Max() { return 0xFFFFu; } };
template<> struct NumLimits<u32> { static constexpr u32 Min() { return 0; } static constexpr u32 Max() { return 0xFFFFFFFFu; } };
template<> struct NumLimits<u64> { static constexpr u64 Min() { return 0; } static constexpr u64 Max() { return 0xFFFFFFFFFFFFFFFFull; } };

template<> struct NumLimits<i8>  { static constexpr i8  Min() { return -127 - 1; }                     static constexpr i8  Max() { return 127; } };
template<> struct NumLimits<i16> { static constexpr i16 Min() { return -32767 - 1; }                   static constexpr i16 Max() { return 32767; } };
template<> struct NumLimits<i32> { static constexpr i32 Min() { return -2147483647 - 1; }              static constexpr i32 Max() { return 2147483647; } };
template<> struct NumLimits<i64> { static constexpr i64 Min() { return -9223372036854775807ll - 1; }   static constexpr i64 Max() { return 9223372036854775807ll; } };

template<> struct NumLimits<f32> {
    static constexpr f32 Min()      { return 1.175494351e-38f; }
    static constexpr f32 Max()      { return 3.402823466e+38f; }
    static constexpr f32 Epsilon()  { return 1.192092896e-07f; }
    static constexpr f32 Infinity() { return __builtin_huge_valf(); }
};
template<> struct NumLimits<f64> {
    static constexpr f64 Min()      { return 2.2250738585072014e-308; }
    static constexpr f64 Max()      { return 1.7976931348623158e+308; }
    static constexpr f64 Epsilon()  { return 2.2204460492503131e-016; }
    static constexpr f64 Infinity() { return __builtin_huge_val(); }
};

} // namespace acs
