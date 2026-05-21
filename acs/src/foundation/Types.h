// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Foundation — 基本型エイリアス定義
// -----------------------------------------------------------------------------
// このファイルでは、ACS 全体で使用する整数 / 浮動小数点 / ポインタ系の
// 短いエイリアス（u32, i64, f32, usize 等）を提供する。
//
// STL 禁止方針のため、依存は <cstdint> と <cstddef> のみ（C 標準ライブラリ）。
// プラットフォーム間でビット幅を保証するため、すべて固定幅整数を採用。
// =============================================================================
#pragma once

#include <cstdint>   // 固定幅整数（int32_t など）
#include <cstddef>   // size_t / ptrdiff_t

namespace acs {

// ---- 符号なし整数 ----------------------------------------------------------
using u8  = ::uint8_t;   // 8  ビット符号なし整数
using u16 = ::uint16_t;  // 16 ビット符号なし整数
using u32 = ::uint32_t;  // 32 ビット符号なし整数
using u64 = ::uint64_t;  // 64 ビット符号なし整数

// ---- 符号付き整数 ----------------------------------------------------------
using i8  = ::int8_t;    // 8  ビット符号付き整数
using i16 = ::int16_t;   // 16 ビット符号付き整数
using i32 = ::int32_t;   // 32 ビット符号付き整数
using i64 = ::int64_t;   // 64 ビット符号付き整数

// ---- 浮動小数点 ------------------------------------------------------------
using f32 = float;       // 単精度浮動小数（IEEE 754 binary32）
using f64 = double;      // 倍精度浮動小数（IEEE 754 binary64）

// ---- サイズ / ポインタ整数 -------------------------------------------------
using usize = ::size_t;     // メモリサイズ・配列インデックス用（ポインタ幅と同一）
using isize = ::ptrdiff_t;  // ポインタ差分用
using uptr  = ::uintptr_t;  // ポインタを符号なし整数として扱う際に使用
using iptr  = ::intptr_t;   // ポインタを符号付き整数として扱う際に使用

// ---- 文字 / バイト ---------------------------------------------------------
using byte = unsigned char; // 生のバイト列を扱う際の型
using c8   = char;          // UTF-8 用 1 バイト文字
using c16  = char16_t;      // UTF-16 文字
using c32  = char32_t;      // UTF-32 文字

// ---- 静的サイズ検証 --------------------------------------------------------
// 各プラットフォーム間で同一サイズになることを保証する（ビルド時に検出）。
static_assert(sizeof(u8)  == 1, "u8 must be 1 byte");
static_assert(sizeof(u16) == 2, "u16 must be 2 bytes");
static_assert(sizeof(u32) == 4, "u32 must be 4 bytes");
static_assert(sizeof(u64) == 8, "u64 must be 8 bytes");
static_assert(sizeof(f32) == 4, "f32 must be 4 bytes");
static_assert(sizeof(f64) == 8, "f64 must be 8 bytes");
static_assert(sizeof(usize) == sizeof(void*), "usize must match pointer size");

} // namespace acs
