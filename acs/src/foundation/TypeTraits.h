// =============================================================================
// ACS Foundation — 最小型特性ライブラリ（<type_traits> 代替）
// -----------------------------------------------------------------------------
// 必要最小限の型特性のみ提供する。新たに必要になったら追加する方針。
//
// 提供:
//   - BoolConstant / TrueType / FalseType
//   - IsSame, IsLvalueRef, IsRvalueRef
//   - RemoveRef, RemoveConst, RemoveCV, RemoveCVRef
//   - IsIntegral, IsFloating, IsArithmetic, IsPointer
//   - コンパイラ組み込み trait をラップ: IsTriviallyCopyable など
//   - EnableIf, Conditional
//
// 例外を投げない constexpr / inline 評価のみ。
// =============================================================================
#pragma once

#include "foundation/Types.h"

namespace acs {

// ---- bool 定数 ------------------------------------------------------------
// std::integral_constant<bool, V> 相当。bool 値をテンプレート引数として伝播させる。
template<bool V> struct BoolConstant { static constexpr bool Value = V; };
using TrueType  = BoolConstant<true>;
using FalseType = BoolConstant<false>;

// ---- IsSame: 二つの型が同一か判定 -----------------------------------------
template<typename A, typename B> struct IsSame      : FalseType {};
template<typename A>             struct IsSame<A,A> : TrueType  {};
template<typename A, typename B> inline constexpr bool IsSameV = IsSame<A,B>::Value;

// ---- 参照／const 修飾子の除去 ---------------------------------------------
// テンプレートメタプログラミングで型を「素」の形に戻すために使う。
template<typename T> struct RemoveRef        { using Type = T; };  // 参照ではない
template<typename T> struct RemoveRef<T&>    { using Type = T; };  // lvalue 参照
template<typename T> struct RemoveRef<T&&>   { using Type = T; };  // rvalue 参照
template<typename T> using  RemoveRefT = typename RemoveRef<T>::Type;

template<typename T> struct RemoveConst         { using Type = T; };
template<typename T> struct RemoveConst<const T>{ using Type = T; };
template<typename T> using  RemoveConstT = typename RemoveConst<T>::Type;

template<typename T> struct RemoveCV             { using Type = T; };
template<typename T> struct RemoveCV<const T>    { using Type = T; };
template<typename T> struct RemoveCV<volatile T> { using Type = T; };
template<typename T> struct RemoveCV<const volatile T> { using Type = T; };
template<typename T> using  RemoveCVT = typename RemoveCV<T>::Type;

template<typename T> struct RemoveCVRef { using Type = RemoveCVT<RemoveRefT<T>>; };
template<typename T> using  RemoveCVRefT = typename RemoveCVRef<T>::Type;

// ---- 参照種別判定 ---------------------------------------------------------
template<typename T> struct IsLvalueRef     : FalseType {};
template<typename T> struct IsLvalueRef<T&> : TrueType  {};
template<typename T> inline constexpr bool IsLvalueRefV = IsLvalueRef<T>::Value;

template<typename T> struct IsRvalueRef      : FalseType {};
template<typename T> struct IsRvalueRef<T&&> : TrueType  {};
template<typename T> inline constexpr bool IsRvalueRefV = IsRvalueRef<T>::Value;

// ---- 整数型／浮動小数型／算術型判定 ---------------------------------------
// 各組み込み型を網羅的に列挙して TrueType に特殊化する。
template<typename T> struct IsIntegralImpl : FalseType {};
template<> struct IsIntegralImpl<bool>     : TrueType {};
template<> struct IsIntegralImpl<char>     : TrueType {};
template<> struct IsIntegralImpl<i8>       : TrueType {};
template<> struct IsIntegralImpl<u8>       : TrueType {};
template<> struct IsIntegralImpl<i16>      : TrueType {};
template<> struct IsIntegralImpl<u16>      : TrueType {};
template<> struct IsIntegralImpl<i32>      : TrueType {};
template<> struct IsIntegralImpl<u32>      : TrueType {};
template<> struct IsIntegralImpl<i64>      : TrueType {};
template<> struct IsIntegralImpl<u64>      : TrueType {};
// const / volatile 修飾を除去してから判定する。
template<typename T> struct IsIntegral : IsIntegralImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsIntegralV = IsIntegral<T>::Value;

template<typename T> struct IsFloatingImpl : FalseType {};
template<> struct IsFloatingImpl<f32>      : TrueType {};
template<> struct IsFloatingImpl<f64>      : TrueType {};
template<typename T> struct IsFloating : IsFloatingImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsFloatingV = IsFloating<T>::Value;

// 算術型 = 整数型 or 浮動小数型
template<typename T> inline constexpr bool IsArithmeticV = IsIntegralV<T> || IsFloatingV<T>;

// ---- ポインタ判定 ---------------------------------------------------------
template<typename T> struct IsPointer     : FalseType {};
template<typename T> struct IsPointer<T*> : TrueType  {};
template<typename T> inline constexpr bool IsPointerV = IsPointer<RemoveCVT<T>>::Value;

// ---- コンパイラ組み込み trait ラッパ ------------------------------------
// __is_* 系の組み込みは MSVC / Clang / GCC すべてで利用可能。これらは
// ユーザー定義型に対する「明らかな」性質判定に使う。
template<typename T> inline constexpr bool IsTriviallyCopyableV     = __is_trivially_copyable(T);
template<typename T> inline constexpr bool IsTriviallyDestructibleV = __is_trivially_destructible(T);
template<typename T> inline constexpr bool IsTriviallyConstructibleV= __is_trivially_constructible(T);
template<typename T> inline constexpr bool IsEmptyV                 = __is_empty(T);
template<typename T> inline constexpr bool IsAbstractV              = __is_abstract(T);
template<typename T> inline constexpr bool IsEnumV                  = __is_enum(T);
template<typename Base, typename Derived> inline constexpr bool IsBaseOfV = __is_base_of(Base, Derived);

// ---- EnableIf: SFINAE 用 -------------------------------------------------
// EnableIfT<cond, T> は cond が true のときのみ T を生成（false なら無効化）。
template<bool C, typename T = void> struct EnableIf {};
template<typename T> struct EnableIf<true, T> { using Type = T; };
template<bool C, typename T = void> using EnableIfT = typename EnableIf<C,T>::Type;

// ---- Conditional: コンパイル時三項演算子 ---------------------------------
// ConditionalT<cond, A, B> は cond ? A : B 相当の型を返す。
template<bool C, typename A, typename B> struct Conditional             { using Type = A; };
template<typename A, typename B>         struct Conditional<false, A, B>{ using Type = B; };
template<bool C, typename A, typename B> using  ConditionalT = typename Conditional<C,A,B>::Type;

} // namespace acs
