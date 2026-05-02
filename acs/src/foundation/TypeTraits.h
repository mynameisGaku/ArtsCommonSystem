// ACS Foundation — Minimal type traits (no <type_traits>).
// Only the traits we actually use are defined here. Add more on demand.
#pragma once

#include "foundation/Types.h"

namespace acs {

// ---- Bool constant ---------------------------------------------------------
template<bool V> struct BoolConstant { static constexpr bool Value = V; };
using TrueType  = BoolConstant<true>;
using FalseType = BoolConstant<false>;

// ---- IsSame ----------------------------------------------------------------
template<typename A, typename B> struct IsSame      : FalseType {};
template<typename A>             struct IsSame<A,A> : TrueType  {};
template<typename A, typename B> inline constexpr bool IsSameV = IsSame<A,B>::Value;

// ---- Reference / pointer manipulation --------------------------------------
template<typename T> struct RemoveRef        { using Type = T; };
template<typename T> struct RemoveRef<T&>    { using Type = T; };
template<typename T> struct RemoveRef<T&&>   { using Type = T; };
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

// ---- Lvalue/rvalue reference traits ----------------------------------------
template<typename T> struct IsLvalueRef     : FalseType {};
template<typename T> struct IsLvalueRef<T&> : TrueType  {};
template<typename T> inline constexpr bool IsLvalueRefV = IsLvalueRef<T>::Value;

template<typename T> struct IsRvalueRef      : FalseType {};
template<typename T> struct IsRvalueRef<T&&> : TrueType  {};
template<typename T> inline constexpr bool IsRvalueRefV = IsRvalueRef<T>::Value;

// ---- Integral / floating ---------------------------------------------------
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
template<typename T> struct IsIntegral : IsIntegralImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsIntegralV = IsIntegral<T>::Value;

template<typename T> struct IsFloatingImpl : FalseType {};
template<> struct IsFloatingImpl<f32>      : TrueType {};
template<> struct IsFloatingImpl<f64>      : TrueType {};
template<typename T> struct IsFloating : IsFloatingImpl<RemoveCVT<T>> {};
template<typename T> inline constexpr bool IsFloatingV = IsFloating<T>::Value;

template<typename T> inline constexpr bool IsArithmeticV = IsIntegralV<T> || IsFloatingV<T>;

// ---- Pointer / null pointer ------------------------------------------------
template<typename T> struct IsPointer     : FalseType {};
template<typename T> struct IsPointer<T*> : TrueType  {};
template<typename T> inline constexpr bool IsPointerV = IsPointer<RemoveCVT<T>>::Value;

// ---- Compiler intrinsic-backed traits --------------------------------------
template<typename T> inline constexpr bool IsTriviallyCopyableV     = __is_trivially_copyable(T);
template<typename T> inline constexpr bool IsTriviallyDestructibleV = __is_trivially_destructible(T);
template<typename T> inline constexpr bool IsTriviallyConstructibleV= __is_trivially_constructible(T);
template<typename T> inline constexpr bool IsEmptyV                 = __is_empty(T);
template<typename T> inline constexpr bool IsAbstractV              = __is_abstract(T);
template<typename T> inline constexpr bool IsEnumV                  = __is_enum(T);
template<typename Base, typename Derived> inline constexpr bool IsBaseOfV = __is_base_of(Base, Derived);

// ---- EnableIf --------------------------------------------------------------
template<bool C, typename T = void> struct EnableIf {};
template<typename T> struct EnableIf<true, T> { using Type = T; };
template<bool C, typename T = void> using EnableIfT = typename EnableIf<C,T>::Type;

// ---- Conditional -----------------------------------------------------------
template<bool C, typename A, typename B> struct Conditional             { using Type = A; };
template<typename A, typename B>         struct Conditional<false, A, B>{ using Type = B; };
template<bool C, typename A, typename B> using  ConditionalT = typename Conditional<C,A,B>::Type;

} // namespace acs
