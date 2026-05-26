// SPDX-License-Identifier: Apache-2.0
// 組込み暗黙変換 — i32/u32/f32/f64/bool/FString の主要ペアを Bind 一発で
//
// 使い方:
//   FObservable<f32> hp{100.0f};
//   FObservable<FString> hp_text;
//
//   auto bind = Bind(hp, hp_text);   // 自動で f32 → FString 変換 (例: "100.0")
//
//   FObservable<i32> level{42};
//   FObservable<f32> level_f;
//   auto bind2 = Bind(level, level_f);   // 自動で i32 → f32 (42.0)
//
//   // 同じ型なら FOneWayBinder にフォールバック
//   FObservable<f32> a{1.0f}, b;
//   auto bind3 = Bind(a, b);   // FOneWayBinder<f32>
//
// サポート対象の変換マトリクス:
//   i32  ↔ u32 / f32 / f64 / bool / FString
//   u32  ↔ i32 / f32 / f64 / bool / FString
//   f32  ↔ i32 / u32 / f64 / bool / FString
//   f64  ↔ i32 / u32 / f32 / bool / FString
//   bool ↔ i32 / u32 / f32 / f64 / FString
//   FString ↔ 上記すべて (パース失敗時は T{} = 0/false/空文字)
//
// 設計:
//   ・TDefaultConverter<Src, Dst>::Convert(const Src&, void*) → Dst で実装
//   ・特殊化が無いペアは static_assert で「Bind: 既定変換が無い」と教える
//   ・FString 変換のみ .cpp に実装 (FContainer<FString> 依存)
#pragma once

#include "foundation/Types.h"

namespace acs { class FString; }

namespace acs::mvvm {

// プライマリテンプレート (実装無し → コンパイル時に静的アサート)
template<typename Src, typename Dst>
struct TDefaultConverter {
    static_assert(sizeof(Src) == 0,
        "Bind(src, dst): no built-in conversion. Use FOneWayConvertBinder<Src, Dst> "
        "with an explicit converter function.");
};

// ==========================================================================
//  ヘッダ実装可能な数値間変換 (static_cast で済むもの)
// ==========================================================================

// --- i32 ↔ ---
template<> struct TDefaultConverter<i32, u32> {
    static u32 Convert(const i32& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct TDefaultConverter<i32, f32> {
    static f32 Convert(const i32& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct TDefaultConverter<i32, f64> {
    static f64 Convert(const i32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct TDefaultConverter<i32, bool> {
    static bool Convert(const i32& v, void*) noexcept { return v != 0; }
};

// --- u32 ↔ ---
template<> struct TDefaultConverter<u32, i32> {
    static i32 Convert(const u32& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct TDefaultConverter<u32, f32> {
    static f32 Convert(const u32& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct TDefaultConverter<u32, f64> {
    static f64 Convert(const u32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct TDefaultConverter<u32, bool> {
    static bool Convert(const u32& v, void*) noexcept { return v != 0; }
};

// --- f32 ↔ ---
template<> struct TDefaultConverter<f32, i32> {
    static i32 Convert(const f32& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct TDefaultConverter<f32, u32> {
    static u32 Convert(const f32& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct TDefaultConverter<f32, f64> {
    static f64 Convert(const f32& v, void*) noexcept { return static_cast<f64>(v); }
};
template<> struct TDefaultConverter<f32, bool> {
    static bool Convert(const f32& v, void*) noexcept { return v != 0.0f; }
};

// --- f64 ↔ ---
template<> struct TDefaultConverter<f64, i32> {
    static i32 Convert(const f64& v, void*) noexcept { return static_cast<i32>(v); }
};
template<> struct TDefaultConverter<f64, u32> {
    static u32 Convert(const f64& v, void*) noexcept { return static_cast<u32>(v); }
};
template<> struct TDefaultConverter<f64, f32> {
    static f32 Convert(const f64& v, void*) noexcept { return static_cast<f32>(v); }
};
template<> struct TDefaultConverter<f64, bool> {
    static bool Convert(const f64& v, void*) noexcept { return v != 0.0; }
};

// --- bool ↔ ---
template<> struct TDefaultConverter<bool, i32> {
    static i32 Convert(const bool& v, void*) noexcept { return v ? 1 : 0; }
};
template<> struct TDefaultConverter<bool, u32> {
    static u32 Convert(const bool& v, void*) noexcept { return v ? 1u : 0u; }
};
template<> struct TDefaultConverter<bool, f32> {
    static f32 Convert(const bool& v, void*) noexcept { return v ? 1.0f : 0.0f; }
};
template<> struct TDefaultConverter<bool, f64> {
    static f64 Convert(const bool& v, void*) noexcept { return v ? 1.0 : 0.0; }
};

// ==========================================================================
//  FString 関連変換 (.cpp 側に実装)
// ==========================================================================

template<> struct TDefaultConverter<i32, FString>  { static FString Convert(const i32& v, void*) noexcept; };
template<> struct TDefaultConverter<u32, FString>  { static FString Convert(const u32& v, void*) noexcept; };
template<> struct TDefaultConverter<f32, FString>  { static FString Convert(const f32& v, void*) noexcept; };
template<> struct TDefaultConverter<f64, FString>  { static FString Convert(const f64& v, void*) noexcept; };
template<> struct TDefaultConverter<bool, FString> { static FString Convert(const bool& v, void*) noexcept; };

template<> struct TDefaultConverter<FString, i32>  { static i32  Convert(const FString& v, void*) noexcept; };
template<> struct TDefaultConverter<FString, u32>  { static u32  Convert(const FString& v, void*) noexcept; };
template<> struct TDefaultConverter<FString, f32>  { static f32  Convert(const FString& v, void*) noexcept; };
template<> struct TDefaultConverter<FString, f64>  { static f64  Convert(const FString& v, void*) noexcept; };
template<> struct TDefaultConverter<FString, bool> { static bool Convert(const FString& v, void*) noexcept; };

} // namespace acs::mvvm
