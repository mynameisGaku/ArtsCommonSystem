// SPDX-License-Identifier: Apache-2.0
// 組込み暗黙変換 — i32/u32/f32/f64/bool/String の主要ペアを Bind 一発で
//
// 使い方:
//   Observable<f32> hp{100.0f};
//   Observable<String> hp_text;
//
//   auto bind = Bind(hp, hp_text);   // 自動で f32 → String 変換 (例: "100.0")
//
//   Observable<i32> level{42};
//   Observable<f32> level_f;
//   auto bind2 = Bind(level, level_f);   // 自動で i32 → f32 (42.0)
//
//   // 同じ型なら OneWayBinder にフォールバック
//   Observable<f32> a{1.0f}, b;
//   auto bind3 = Bind(a, b);   // OneWayBinder<f32>
//
// サポート対象の変換マトリクス:
//   i32  ↔ u32 / f32 / f64 / bool / String
//   u32  ↔ i32 / f32 / f64 / bool / String
//   f32  ↔ i32 / u32 / f64 / bool / String
//   f64  ↔ i32 / u32 / f32 / bool / String
//   bool ↔ i32 / u32 / f32 / f64 / String
//   String ↔ 上記すべて (パース失敗時は T{} = 0/false/空文字)
//
// 設計:
//   ・DefaultConverter<Src, Dst>::Convert(const Src&, void*) → Dst で実装
//   ・特殊化が無いペアは static_assert で「Bind: 既定変換が無い」と教える
//   ・String 変換のみ .cpp に実装 (Container<String> 依存)
#pragma once

#include "foundation/Types.h"

namespace acs { class String; }

namespace acs::mvvm {

// プライマリテンプレート (実装無し → コンパイル時に静的アサート)
template<typename Src, typename Dst>
struct DefaultConverter {
    static_assert(sizeof(Src) == 0,
        "Bind(src, dst): no built-in conversion. Use OneWayConvertBinder<Src, Dst> "
        "with an explicit converter function.");
};

// ==========================================================================
//  ヘッダ実装可能な数値間変換 (static_cast で済むもの)
// ==========================================================================

// --- i32 ↔ ---
template<> struct DefaultConverter<i32, u32> {
    static u32 Convert(const i32& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct DefaultConverter<i32, f32> {
    static f32 Convert(const i32& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct DefaultConverter<i32, f64> {
    static f64 Convert(const i32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct DefaultConverter<i32, bool> {
    static bool Convert(const i32& v, void*) noexcept { return v != 0; }
};

// --- u32 ↔ ---
template<> struct DefaultConverter<u32, i32> {
    static i32 Convert(const u32& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct DefaultConverter<u32, f32> {
    static f32 Convert(const u32& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct DefaultConverter<u32, f64> {
    static f64 Convert(const u32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct DefaultConverter<u32, bool> {
    static bool Convert(const u32& v, void*) noexcept { return v != 0; }
};

// --- f32 ↔ ---
template<> struct DefaultConverter<f32, i32> {
    static i32 Convert(const f32& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct DefaultConverter<f32, u32> {
    static u32 Convert(const f32& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct DefaultConverter<f32, f64> {
    static f64 Convert(const f32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct DefaultConverter<f32, bool> {
    static bool Convert(const f32& v, void*) noexcept { return v != 0.0f; }
};

// --- f64 ↔ ---
template<> struct DefaultConverter<f64, i32> {
    static i32 Convert(const f64& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct DefaultConverter<f64, u32> {
    static u32 Convert(const f64& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct DefaultConverter<f64, f32> {
    static f32 Convert(const f64& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct DefaultConverter<f64, bool> {
    static bool Convert(const f64& v, void*) noexcept { return v != 0.0; }
};

// --- bool ↔ ---
template<> struct DefaultConverter<bool, i32> {
    static i32 Convert(const bool& v, void*) noexcept { return v ? 1 : 0; }
};
template<> struct DefaultConverter<bool, u32> {
    static u32 Convert(const bool& v, void*) noexcept { return v ? 1u : 0u; }
};
template<> struct DefaultConverter<bool, f32> {
    static f32 Convert(const bool& v, void*) noexcept { return v ? 1.0f : 0.0f; }
};
template<> struct DefaultConverter<bool, f64> {
    static f64 Convert(const bool& v, void*) noexcept { return v ? 1.0 : 0.0; }
};

// ==========================================================================
//  String 関連変換 (.cpp 側に実装)
// ==========================================================================

template<> struct DefaultConverter<i32, String>  { static String Convert(const i32& v, void*) noexcept; };
template<> struct DefaultConverter<u32, String>  { static String Convert(const u32& v, void*) noexcept; };
template<> struct DefaultConverter<f32, String>  { static String Convert(const f32& v, void*) noexcept; };
template<> struct DefaultConverter<f64, String>  { static String Convert(const f64& v, void*) noexcept; };
template<> struct DefaultConverter<bool, String> { static String Convert(const bool& v, void*) noexcept; };

template<> struct DefaultConverter<String, i32>  { static i32  Convert(const String& v, void*) noexcept; };
template<> struct DefaultConverter<String, u32>  { static u32  Convert(const String& v, void*) noexcept; };
template<> struct DefaultConverter<String, f32>  { static f32  Convert(const String& v, void*) noexcept; };
template<> struct DefaultConverter<String, f64>  { static f64  Convert(const String& v, void*) noexcept; };
template<> struct DefaultConverter<String, bool> { static bool Convert(const String& v, void*) noexcept; };

} // namespace acs::mvvm
