// SPDX-License-Identifier: Apache-2.0
// 必要最小限の型特性のみ提供（<type_traits> 代替）
#pragma once

#include "foundation/Types.h"

namespace acs {

// bool 値をテンプレート引数として伝播させる
template<bool V> struct TBoolConstant { static constexpr bool Value = V; };
using TTrueType  = TBoolConstant<true>;
using TFalseType = TBoolConstant<false>;

// 二つの型が同一なら TTrueType
template<typename A, typename B> struct TIsSame      : TFalseType {};
template<typename A>             struct TIsSame<A,A> : TTrueType  {};
template<typename A, typename B> inline constexpr bool IsSameV = TIsSame<A,B>::Value;

// 参照修飾子を取り除く
template<typename T> struct TRemoveRef        { using Type = T; };
template<typename T> struct TRemoveRef<T&>    { using Type = T; };
template<typename T> struct TRemoveRef<T&&>   { using Type = T; };
template<typename T> using  RemoveRefT = typename TRemoveRef<T>::Type;

// const 修飾子を取り除く
template<typename T> struct TRemoveConst         { using Type = T; };
template<typename T> struct TRemoveConst<const T>{ using Type = T; };
template<typename T> using  RemoveConstT = typename TRemoveConst<T>::Type;

// const / volatile を取り除く
template<typename T> struct TRemoveCV             { using Type = T; };
template<typename T> struct TRemoveCV<const T>    { using Type = T; };
template<typename T> struct TRemoveCV<volatile T> { using Type = T; };
template<typename T> struct TRemoveCV<const volatile T> { using Type = T; };
template<typename T> using  RemoveCVT = typename TRemoveCV<T>::Type;

// 参照と CV 両方を取り除く
template<typename T> struct TRemoveCVRef { using Type = RemoveCVT<RemoveRefT<T>>; };
template<typename T> using  RemoveCVRefT = typename TRemoveCVRef<T>::Type;

// lvalue 参照かどうか
template<typename T> struct TIsLvalueRef     : TFalseType {};
template<typename T> struct TIsLvalueRef<T&> : TTrueType  {};
template<typename T> inline constexpr bool IsLvalueRefV = TIsLvalueRef<T>::Value;

// rvalue 参照かどうか
template<typename T> struct TIsRvalueRef      : TFalseType {};
template<typename T> struct TIsRvalueRef<T&&> : TTrueType  {};
template<typename T> inline constexpr bool IsRvalueRefV = TIsRvalueRef<T>::Value;

// 整数型かどうかを各組み込み型で特殊化
template<typename T> struct TIsIntegralImpl : TFalseType {};
template<> struct TIsIntegralImpl<bool>     : TTrueType {};
template<> struct TIsIntegralImpl<char>     : TTrueType {};
template<> struct TIsIntegralImpl<i8>       : TTrueType {};
template<> struct TIsIntegralImpl<u8>       : TTrueType {};
template<> struct TIsIntegralImpl<i16>      : TTrueType {};
template<> struct TIsIntegralImpl<u16>      : TTrueType {};
template<> struct TIsIntegralImpl<i32>      : TTrueType {};
template<> struct TIsIntegralImpl<u32>      : TTrueType {};
template<> struct TIsIntegralImpl<i64>      : TTrueType {};
template<> struct TIsIntegralImpl<u64>      : TTrueType {};
// CV 修飾を除去してから判定
template<typename T> struct TIsIntegral : TIsIntegralImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsIntegralV = TIsIntegral<T>::Value;

// 浮動小数型かどうか
template<typename T> struct TIsFloatingImpl : TFalseType {};
template<> struct TIsFloatingImpl<f32>      : TTrueType {};
template<> struct TIsFloatingImpl<f64>      : TTrueType {};
template<typename T> struct TIsFloating : TIsFloatingImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsFloatingV = TIsFloating<T>::Value;

// 算術型 = 整数型 or 浮動小数型
template<typename T> inline constexpr bool IsArithmeticV = IsIntegralV<T> || IsFloatingV<T>;

// ポインタ型かどうか
template<typename T> struct TIsPointer     : TFalseType {};
template<typename T> struct TIsPointer<T*> : TTrueType  {};
template<typename T> inline constexpr bool IsPointerV = TIsPointer<RemoveCVT<T>>::Value;

// コンパイラ組み込み trait の薄いラッパ
template<typename T> inline constexpr bool IsTriviallyCopyableV     = __is_trivially_copyable(T);
template<typename T> inline constexpr bool IsTriviallyDestructibleV = __is_trivially_destructible(T);
template<typename T> inline constexpr bool IsTriviallyConstructibleV= __is_trivially_constructible(T);
template<typename T> inline constexpr bool IsEmptyV                 = __is_empty(T);
template<typename T> inline constexpr bool IsAbstractV              = __is_abstract(T);
template<typename T> inline constexpr bool IsEnumV                  = __is_enum(T);
template<typename Base, typename FDerived> inline constexpr bool IsBaseOfV = __is_base_of(Base, FDerived);

// SFINAE 用: cond が true のときのみ T を露出させる
template<bool C, typename T = void> struct TEnableIf {};
template<typename T> struct TEnableIf<true, T> { using Type = T; };
template<bool C, typename T = void> using EnableIfT = typename TEnableIf<C,T>::Type;

// コンパイル時三項演算子: cond ? A : B
template<bool C, typename A, typename B> struct TConditional             { using Type = A; };
template<typename A, typename B>         struct TConditional<false, A, B>{ using Type = B; };
template<bool C, typename A, typename B> using  ConditionalT = typename TConditional<C,A,B>::Type;

} // namespace acs
