// ACS Foundation — Primitive type aliases.
// We rely on <cstdint> / <cstddef> only (C standard library, not STL containers).
#pragma once

#include <cstdint>
#include <cstddef>

namespace acs {

using u8  = ::uint8_t;
using u16 = ::uint16_t;
using u32 = ::uint32_t;
using u64 = ::uint64_t;

using i8  = ::int8_t;
using i16 = ::int16_t;
using i32 = ::int32_t;
using i64 = ::int64_t;

using f32 = float;
using f64 = double;

using usize = ::size_t;
using isize = ::ptrdiff_t;
using uptr  = ::uintptr_t;
using iptr  = ::intptr_t;

using byte = unsigned char;
using c8   = char;
using c16  = char16_t;
using c32  = char32_t;

static_assert(sizeof(u8)  == 1, "u8 must be 1 byte");
static_assert(sizeof(u16) == 2, "u16 must be 2 bytes");
static_assert(sizeof(u32) == 4, "u32 must be 4 bytes");
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");
static_assert(sizeof(usize) == sizeof(void*), "usize must match pointer size");

} // namespace acs
