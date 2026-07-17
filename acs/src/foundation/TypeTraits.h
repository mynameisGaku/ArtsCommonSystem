// SPDX-License-Identifier: Apache-2.0
// 必要最小限の型特性のみ提供（<type_traits> 代替）
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * コンパイル時 bool 値を型として運ぶ定数キャリア (std::bool_constant 相当)。
 *
 * @tparam V キャリーする bool 値 (静的メンバ Value に露出する)。
 */
template<bool V> struct TBoolConstant { static constexpr bool Value = V; };

/** Value == true を持つ TBoolConstant 別名 (trait の「真」基底)。 */
using TTrueType  = TBoolConstant<true>;

/** Value == false を持つ TBoolConstant 別名 (trait の「偽」基底)。 */
using TFalseType = TBoolConstant<false>;

/**
 * 二つの型が同一かを判定する trait。
 *
 * @details 既定は偽。A と B が同一型のときだけ部分特殊化が選ばれて真になる。
 * @tparam A 比較する一方の型。
 * @tparam B 比較するもう一方の型。
 */
template<typename A, typename B> struct TIsSame      : TFalseType {};

/** TIsSame の同一型特殊化 (A == B のとき真)。 */
template<typename A>             struct TIsSame<A,A> : TTrueType  {};

/** A と B が同一型なら true となる変数テンプレート。 */
template<typename A, typename B> inline constexpr bool IsSameV = TIsSame<A,B>::Value;

/**
 * 参照修飾子を取り除いた型を Type に提供する trait。
 *
 * @tparam T 参照を外す対象の型。
 */
template<typename T> struct TRemoveRef        { using Type = T; };

/** TRemoveRef の lvalue 参照特殊化 (T& → T)。 */
template<typename T> struct TRemoveRef<T&>    { using Type = T; };

/** TRemoveRef の rvalue 参照特殊化 (T&& → T)。 */
template<typename T> struct TRemoveRef<T&&>   { using Type = T; };

/** T から参照修飾子を外した型の別名。 */
template<typename T> using  RemoveRefT = typename TRemoveRef<T>::Type;

/**
 * const 修飾子を取り除いた型を Type に提供する trait。
 *
 * @tparam T const を外す対象の型。
 */
template<typename T> struct TRemoveConst         { using Type = T; };

/** TRemoveConst の const 特殊化 (const T → T)。 */
template<typename T> struct TRemoveConst<const T>{ using Type = T; };

/** T から const 修飾子を外した型の別名。 */
template<typename T> using  RemoveConstT = typename TRemoveConst<T>::Type;

/**
 * const / volatile 修飾子を取り除いた型を Type に提供する trait。
 *
 * @tparam T cv 修飾を外す対象の型。
 */
template<typename T> struct TRemoveCV             { using Type = T; };

/** TRemoveCV の const 特殊化 (const T → T)。 */
template<typename T> struct TRemoveCV<const T>    { using Type = T; };

/** TRemoveCV の volatile 特殊化 (volatile T → T)。 */
template<typename T> struct TRemoveCV<volatile T> { using Type = T; };

/** TRemoveCV の const volatile 特殊化 (const volatile T → T)。 */
template<typename T> struct TRemoveCV<const volatile T> { using Type = T; };

/** T から const / volatile を外した型の別名。 */
template<typename T> using  RemoveCVT = typename TRemoveCV<T>::Type;

/**
 * 参照と cv 修飾の両方を取り除いた型を Type に提供する trait。
 *
 * @tparam T 参照と cv を外す対象の型。
 */
template<typename T> struct TRemoveCVRef { using Type = RemoveCVT<RemoveRefT<T>>; };

/** T から参照と cv 修飾の両方を外した型の別名。 */
template<typename T> using  RemoveCVRefT = typename TRemoveCVRef<T>::Type;

/**
 * lvalue 参照型かを判定する trait。
 *
 * @details 既定は偽。T& の特殊化のときだけ真になる。
 * @tparam T 判定対象の型。
 */
template<typename T> struct TIsLvalueRef     : TFalseType {};

/** TIsLvalueRef の lvalue 参照特殊化 (T& のとき真)。 */
template<typename T> struct TIsLvalueRef<T&> : TTrueType  {};

/** T が lvalue 参照なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsLvalueRefV = TIsLvalueRef<T>::Value;

/**
 * rvalue 参照型かを判定する trait。
 *
 * @details 既定は偽。T&& の特殊化のときだけ真になる。
 * @tparam T 判定対象の型。
 */
template<typename T> struct TIsRvalueRef      : TFalseType {};

/** TIsRvalueRef の rvalue 参照特殊化 (T&& のとき真)。 */
template<typename T> struct TIsRvalueRef<T&&> : TTrueType  {};

/** T が rvalue 参照なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsRvalueRefV = TIsRvalueRef<T>::Value;

/**
 * 整数型かを判定する実装用 trait (cv 除去前のプレーン型で特殊化)。
 *
 * @details 既定は偽。各組み込み整数型ごとの特殊化で真にする。
 * @tparam T 判定対象の (cv 除去済み) 型。
 */
template<typename T> struct TIsIntegralImpl : TFalseType {};

/** TIsIntegralImpl の bool 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<bool>     : TTrueType {};

/** TIsIntegralImpl の char 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<char>     : TTrueType {};

/** TIsIntegralImpl の i8 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<i8>       : TTrueType {};

/** TIsIntegralImpl の u8 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<u8>       : TTrueType {};

/** TIsIntegralImpl の i16 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<i16>      : TTrueType {};

/** TIsIntegralImpl の u16 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<u16>      : TTrueType {};

/** TIsIntegralImpl の i32 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<i32>      : TTrueType {};

/** TIsIntegralImpl の u32 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<u32>      : TTrueType {};

/** TIsIntegralImpl の i64 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<i64>      : TTrueType {};

/** TIsIntegralImpl の u64 特殊化 (整数型として真)。 */
template<> struct TIsIntegralImpl<u64>      : TTrueType {};

/**
 * 整数型かを判定する trait (cv 修飾を除去してから判定)。
 *
 * @tparam T 判定対象の型。
 */
template<typename T> struct TIsIntegral : TIsIntegralImpl<RemoveCVT<T>> {};

/** T が整数型なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsIntegralV = TIsIntegral<T>::Value;

/**
 * 浮動小数型かを判定する実装用 trait (cv 除去前のプレーン型で特殊化)。
 *
 * @details 既定は偽。f32 / f64 の特殊化で真にする。
 * @tparam T 判定対象の (cv 除去済み) 型。
 */
template<typename T> struct TIsFloatingImpl : TFalseType {};

/** TIsFloatingImpl の f32 特殊化 (浮動小数型として真)。 */
template<> struct TIsFloatingImpl<f32>      : TTrueType {};

/** TIsFloatingImpl の f64 特殊化 (浮動小数型として真)。 */
template<> struct TIsFloatingImpl<f64>      : TTrueType {};

/**
 * 浮動小数型かを判定する trait (cv 修飾を除去してから判定)。
 *
 * @tparam T 判定対象の型。
 */
template<typename T> struct TIsFloating : TIsFloatingImpl<RemoveCVT<T>> {};

/** T が浮動小数型なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsFloatingV = TIsFloating<T>::Value;

/** T が算術型 (整数型または浮動小数型) なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsArithmeticV = IsIntegralV<T> || IsFloatingV<T>;

/**
 * ポインタ型かを判定する trait。
 *
 * @details 既定は偽。T* の特殊化のときだけ真になる。
 * @tparam T 判定対象の型。
 */
template<typename T> struct TIsPointer     : TFalseType {};

/** TIsPointer のポインタ特殊化 (T* のとき真)。 */
template<typename T> struct TIsPointer<T*> : TTrueType  {};

/** T が (cv 除去後) ポインタ型なら true となる変数テンプレート。 */
template<typename T> inline constexpr bool IsPointerV = TIsPointer<RemoveCVT<T>>::Value;

/** T がトリビアルコピー可能なら true (コンパイラ組み込み __is_trivially_copyable のラッパ)。 */
template<typename T> inline constexpr bool IsTriviallyCopyableV     = __is_trivially_copyable(T);

/** T がトリビアル破棄可能なら true (コンパイラ組み込み __is_trivially_destructible のラッパ)。 */
template<typename T> inline constexpr bool IsTriviallyDestructibleV = __is_trivially_destructible(T);

/** T がトリビアル構築可能なら true (コンパイラ組み込み __is_trivially_constructible のラッパ)。 */
template<typename T> inline constexpr bool IsTriviallyConstructibleV= __is_trivially_constructible(T);

/** T が const T& からコピー構築可能なら true (コンパイラ組み込み __is_constructible のラッパ)。 */
template<typename T> inline constexpr bool IsCopyConstructibleV     = __is_constructible(T, const T&);

/** T が空クラス (非静的メンバを持たない) なら true (コンパイラ組み込み __is_empty のラッパ)。 */
template<typename T> inline constexpr bool IsEmptyV                 = __is_empty(T);

/** T が抽象クラスなら true (コンパイラ組み込み __is_abstract のラッパ)。 */
template<typename T> inline constexpr bool IsAbstractV              = __is_abstract(T);

/** T が列挙型なら true (コンパイラ組み込み __is_enum のラッパ)。 */
template<typename T> inline constexpr bool IsEnumV                  = __is_enum(T);

/**
 * Base が Derived の基底クラスなら true (コンパイラ組み込み __is_base_of のラッパ)。
 *
 * @tparam Base 基底候補の型。
 * @tparam Derived 派生候補の型。
 */
template<typename Base, typename Derived> inline constexpr bool IsBaseOfV = __is_base_of(Base, Derived);

/**
 * SFINAE 用: 条件が true のときだけ Type を露出させる trait。
 *
 * @details 既定 (C==false) は Type メンバを持たず、オーバーロード候補から外れる。
 * @tparam C 有効化条件。
 * @tparam T C が true のとき露出させる型 (既定 void)。
 */
template<bool C, typename T = void> struct TEnableIf {};

/** TEnableIf の true 特殊化 (Type に T を露出させる)。 */
template<typename T> struct TEnableIf<true, T> { using Type = T; };

/** C が true のときだけ T を解決する SFINAE 用別名。 */
template<bool C, typename T = void> using EnableIfT = typename TEnableIf<C,T>::Type;

/**
 * コンパイル時三項演算子: C ? A : B の型を Type に提供する trait。
 *
 * @details 既定 (C==true) は A を選ぶ。
 * @tparam C 選択条件。
 * @tparam A C が true のとき選ばれる型。
 * @tparam B C が false のとき選ばれる型。
 */
template<bool C, typename A, typename B> struct TConditional             { using Type = A; };

/** TConditional の false 特殊化 (B を選ぶ)。 */
template<typename A, typename B>         struct TConditional<false, A, B>{ using Type = B; };

/** C ? A : B を解決する別名。 */
template<bool C, typename A, typename B> using  ConditionalT = typename TConditional<C,A,B>::Type;

} // namespace acs
