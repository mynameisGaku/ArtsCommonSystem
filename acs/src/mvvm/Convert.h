// SPDX-License-Identifier: Apache-2.0
// 組込み暗黙変換 — i32/u32/f32/f64/bool/FString の主要ペアを Bind 一発で
//
// 使い方:
//   TObservable<f32> hp{100.0f};
//   TObservable<FString> hp_text;
//
//   auto bind = Bind(hp, hp_text);   // 自動で f32 → FString 変換 (例: "100.0")
//
//   TObservable<i32> level{42};
//   TObservable<f32> level_f;
//   auto bind2 = Bind(level, level_f);   // 自動で i32 → f32 (42.0)
//
//   // 同じ型なら TOneWayBinder にフォールバック
//   TObservable<f32> a{1.0f}, b;
//   auto bind3 = Bind(a, b);   // TOneWayBinder<f32>
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
//   ・FString 変換のみ .cpp に実装 (Container<FString> 依存)
#pragma once

#include "foundation/Types.h"

namespace acs { class FString; }

namespace acs::mvvm {

/**
 * Bind が使う既定型変換のプライマリテンプレート (既定変換が無いことを示す)。
 *
 * @details
 * 特殊化が無いペアでインスタンス化されると static_assert が発火し、明示変換関数つきの
 * TOneWayConvertBinder を使うよう促す。実際の変換は型ペアごとの特殊化が提供する。
 * @tparam Src 変換元の型。
 * @tparam Dst 変換先の型。
 */
template<typename Src, typename Dst>
struct TDefaultConverter {
    static_assert(sizeof(Src) == 0,
        "Bind(src, dst): no built-in conversion. Use TOneWayConvertBinder<Src, Dst> "
        "with an explicit converter function.");
};

/** i32 → u32 への既定変換。 */
template<> struct TDefaultConverter<i32, u32> {
    /**
     * i32 を u32 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した u32。
     */
    static u32 Convert(const i32& v, void*) noexcept { return static_cast<u32>(v); }
};

/** i32 → f32 への既定変換。 */
template<> struct TDefaultConverter<i32, f32> {
    /**
     * i32 を f32 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f32。
     */
    static f32 Convert(const i32& v, void*) noexcept { return static_cast<f32>(v); }
};

/** i32 → f64 への既定変換。 */
template<> struct TDefaultConverter<i32, f64> {
    /**
     * i32 を f64 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f64。
     */
    static f64 Convert(const i32& v, void*) noexcept { return static_cast<f64>(v); }
};

/** i32 → bool への既定変換。 */
template<> struct TDefaultConverter<i32, bool> {
    /**
     * i32 を bool へ変換する。
     *
     * @param v 変換元の値。
     * @return 0 以外なら true。
     */
    static bool Convert(const i32& v, void*) noexcept { return v != 0; }
};

/** u32 → i32 への既定変換。 */
template<> struct TDefaultConverter<u32, i32> {
    /**
     * u32 を i32 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した i32。
     */
    static i32 Convert(const u32& v, void*) noexcept { return static_cast<i32>(v); }
};

/** u32 → f32 への既定変換。 */
template<> struct TDefaultConverter<u32, f32> {
    /**
     * u32 を f32 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f32。
     */
    static f32 Convert(const u32& v, void*) noexcept { return static_cast<f32>(v); }
};

/** u32 → f64 への既定変換。 */
template<> struct TDefaultConverter<u32, f64> {
    /**
     * u32 を f64 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f64。
     */
    static f64 Convert(const u32& v, void*) noexcept { return static_cast<f64>(v); }
};

/** u32 → bool への既定変換。 */
template<> struct TDefaultConverter<u32, bool> {
    /**
     * u32 を bool へ変換する。
     *
     * @param v 変換元の値。
     * @return 0 以外なら true。
     */
    static bool Convert(const u32& v, void*) noexcept { return v != 0; }
};

/** f32 → i32 への既定変換。 */
template<> struct TDefaultConverter<f32, i32> {
    /**
     * f32 を i32 へ変換する (切り捨て)。
     *
     * @param v 変換元の値。
     * @return static_cast した i32。
     */
    static i32 Convert(const f32& v, void*) noexcept { return static_cast<i32>(v); }
};

/** f32 → u32 への既定変換。 */
template<> struct TDefaultConverter<f32, u32> {
    /**
     * f32 を u32 へ変換する (切り捨て)。
     *
     * @param v 変換元の値。
     * @return static_cast した u32。
     */
    static u32 Convert(const f32& v, void*) noexcept { return static_cast<u32>(v); }
};

/** f32 → f64 への既定変換。 */
template<> struct TDefaultConverter<f32, f64> {
    /**
     * f32 を f64 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f64。
     */
    static f64 Convert(const f32& v, void*) noexcept { return static_cast<f64>(v); }
};

/** f32 → bool への既定変換。 */
template<> struct TDefaultConverter<f32, bool> {
    /**
     * f32 を bool へ変換する。
     *
     * @param v 変換元の値。
     * @return 0.0f 以外なら true。
     */
    static bool Convert(const f32& v, void*) noexcept { return v != 0.0f; }
};

/** f64 → i32 への既定変換。 */
template<> struct TDefaultConverter<f64, i32> {
    /**
     * f64 を i32 へ変換する (切り捨て)。
     *
     * @param v 変換元の値。
     * @return static_cast した i32。
     */
    static i32 Convert(const f64& v, void*) noexcept { return static_cast<i32>(v); }
};

/** f64 → u32 への既定変換。 */
template<> struct TDefaultConverter<f64, u32> {
    /**
     * f64 を u32 へ変換する (切り捨て)。
     *
     * @param v 変換元の値。
     * @return static_cast した u32。
     */
    static u32 Convert(const f64& v, void*) noexcept { return static_cast<u32>(v); }
};

/** f64 → f32 への既定変換。 */
template<> struct TDefaultConverter<f64, f32> {
    /**
     * f64 を f32 へ変換する。
     *
     * @param v 変換元の値。
     * @return static_cast した f32。
     */
    static f32 Convert(const f64& v, void*) noexcept { return static_cast<f32>(v); }
};

/** f64 → bool への既定変換。 */
template<> struct TDefaultConverter<f64, bool> {
    /**
     * f64 を bool へ変換する。
     *
     * @param v 変換元の値。
     * @return 0.0 以外なら true。
     */
    static bool Convert(const f64& v, void*) noexcept { return v != 0.0; }
};

/** bool → i32 への既定変換。 */
template<> struct TDefaultConverter<bool, i32> {
    /**
     * bool を i32 へ変換する。
     *
     * @param v 変換元の値。
     * @return true なら 1、false なら 0。
     */
    static i32 Convert(const bool& v, void*) noexcept { return v ? 1 : 0; }
};

/** bool → u32 への既定変換。 */
template<> struct TDefaultConverter<bool, u32> {
    /**
     * bool を u32 へ変換する。
     *
     * @param v 変換元の値。
     * @return true なら 1、false なら 0。
     */
    static u32 Convert(const bool& v, void*) noexcept { return v ? 1u : 0u; }
};

/** bool → f32 への既定変換。 */
template<> struct TDefaultConverter<bool, f32> {
    /**
     * bool を f32 へ変換する。
     *
     * @param v 変換元の値。
     * @return true なら 1.0f、false なら 0.0f。
     */
    static f32 Convert(const bool& v, void*) noexcept { return v ? 1.0f : 0.0f; }
};

/** bool → f64 への既定変換。 */
template<> struct TDefaultConverter<bool, f64> {
    /**
     * bool を f64 へ変換する。
     *
     * @param v 変換元の値。
     * @return true なら 1.0、false なら 0.0。
     */
    static f64 Convert(const bool& v, void*) noexcept { return v ? 1.0 : 0.0; }
};

/** i32 → FString への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<i32, FString>  {
    /**
     * i32 を 10 進文字列へ変換する。
     *
     * @param v 変換元の値。
     * @return v を "%d" で整形した FString。
     */
    static FString Convert(const i32& v, void*) noexcept;
};

/** u32 → FString への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<u32, FString>  {
    /**
     * u32 を 10 進文字列へ変換する。
     *
     * @param v 変換元の値。
     * @return v を "%u" で整形した FString。
     */
    static FString Convert(const u32& v, void*) noexcept;
};

/** f32 → FString への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<f32, FString>  {
    /**
     * f32 を文字列へ変換する。
     *
     * @param v 変換元の値。
     * @return v を "%g" で整形した FString。
     */
    static FString Convert(const f32& v, void*) noexcept;
};

/** f64 → FString への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<f64, FString>  {
    /**
     * f64 を文字列へ変換する。
     *
     * @param v 変換元の値。
     * @return v を "%g" で整形した FString。
     */
    static FString Convert(const f64& v, void*) noexcept;
};

/** bool → FString への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<bool, FString> {
    /**
     * bool を文字列へ変換する。
     *
     * @param v 変換元の値。
     * @return true なら "true"、false なら "false" の FString。
     */
    static FString Convert(const bool& v, void*) noexcept;
};

/** FString → i32 への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<FString, i32>  {
    /**
     * 文字列を i32 へパースする。
     *
     * @param v 変換元の文字列。
     * @return 10 進パース結果 (失敗・null なら 0)。
     */
    static i32  Convert(const FString& v, void*) noexcept;
};

/** FString → u32 への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<FString, u32>  {
    /**
     * 文字列を u32 へパースする。
     *
     * @param v 変換元の文字列。
     * @return 10 進パース結果 (失敗・null なら 0)。
     */
    static u32  Convert(const FString& v, void*) noexcept;
};

/** FString → f32 への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<FString, f32>  {
    /**
     * 文字列を f32 へパースする。
     *
     * @param v 変換元の文字列。
     * @return パース結果 (失敗・null なら 0.0f)。
     */
    static f32  Convert(const FString& v, void*) noexcept;
};

/** FString → f64 への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<FString, f64>  {
    /**
     * 文字列を f64 へパースする。
     *
     * @param v 変換元の文字列。
     * @return パース結果 (失敗・null なら 0.0)。
     */
    static f64  Convert(const FString& v, void*) noexcept;
};

/** FString → bool への既定変換 (実装は Convert.cpp)。 */
template<> struct TDefaultConverter<FString, bool> {
    /**
     * 文字列を bool へパースする。
     *
     * @param v 変換元の文字列。
     * @return "true"/"True"/"TRUE"/"1" なら true、それ以外は false。
     */
    static bool Convert(const FString& v, void*) noexcept;
};

} // namespace acs::mvvm
