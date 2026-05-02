// ACS Container — Hash functions.
//
// Default byte hash uses wyhash-final (small, fast, good quality per SMHasher).
// Integers and pointers route through a Murmur64 finalizer (1 mul + 2 xorshifts).
// All of these mark themselves "is_avalanching" so HashMap skips its own remix.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "container/StringView.h"

namespace acs {

ACS_FORCEINLINE u64 HashMix64(u64 x) noexcept {
    x ^= x >> 33;
    x *= 0xFF51AFD7ED558CCDull;
    x ^= x >> 33;
    x *= 0xC4CEB9FE1A85EC53ull;
    x ^= x >> 33;
    return x;
}

u64 HashBytes(const void* data, usize len, u64 seed = 0xCBF29CE484222325ull) noexcept;

template<typename T> struct Hasher;

template<> struct Hasher<u8>  { ACS_FORCEINLINE u64 operator()(u8  v) const noexcept { return HashMix64(v); } };
template<> struct Hasher<u16> { ACS_FORCEINLINE u64 operator()(u16 v) const noexcept { return HashMix64(v); } };
template<> struct Hasher<u32> { ACS_FORCEINLINE u64 operator()(u32 v) const noexcept { return HashMix64(v); } };
template<> struct Hasher<u64> { ACS_FORCEINLINE u64 operator()(u64 v) const noexcept { return HashMix64(v); } };
template<> struct Hasher<i8>  { ACS_FORCEINLINE u64 operator()(i8  v) const noexcept { return HashMix64((u64)v); } };
template<> struct Hasher<i16> { ACS_FORCEINLINE u64 operator()(i16 v) const noexcept { return HashMix64((u64)v); } };
template<> struct Hasher<i32> { ACS_FORCEINLINE u64 operator()(i32 v) const noexcept { return HashMix64((u64)v); } };
template<> struct Hasher<i64> { ACS_FORCEINLINE u64 operator()(i64 v) const noexcept { return HashMix64((u64)v); } };

template<typename T>
struct Hasher<T*> {
    ACS_FORCEINLINE u64 operator()(T* p) const noexcept {
        return HashMix64(reinterpret_cast<u64>(p));
    }
};

template<> struct Hasher<StringView> {
    ACS_FORCEINLINE u64 operator()(StringView s) const noexcept {
        return HashBytes(s.Data(), s.Size());
    }
};

} // namespace acs
