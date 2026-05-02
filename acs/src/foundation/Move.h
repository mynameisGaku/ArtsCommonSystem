// ACS Foundation — Move / Forward / Swap (no <utility>).
// Inline-only.
#pragma once

#include "foundation/TypeTraits.h"

namespace acs {

template<typename T>
ACS_FORCEINLINE constexpr RemoveRefT<T>&& Move(T&& v) noexcept {
    return static_cast<RemoveRefT<T>&&>(v);
}

template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>& v) noexcept {
    return static_cast<T&&>(v);
}

template<typename T>
ACS_FORCEINLINE constexpr T&& Forward(RemoveRefT<T>&& v) noexcept {
    static_assert(!IsLvalueRefV<T>, "Cannot forward an rvalue as an lvalue.");
    return static_cast<T&&>(v);
}

template<typename T>
ACS_FORCEINLINE constexpr void Swap(T& a, T& b) noexcept {
    T tmp = static_cast<T&&>(a);
    a     = static_cast<T&&>(b);
    b     = static_cast<T&&>(tmp);
}

// Integer min / max — no <algorithm>.
template<typename T> ACS_FORCEINLINE constexpr T Min(T a, T b) noexcept { return a < b ? a : b; }
template<typename T> ACS_FORCEINLINE constexpr T Max(T a, T b) noexcept { return a > b ? a : b; }
template<typename T> ACS_FORCEINLINE constexpr T Clamp(T v, T lo, T hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

} // namespace acs

// Placement new — provided by the language, but the global placement new
// signatures are formally declared in <new>. We declare them ourselves to
// avoid pulling the entire STL header.
#if !defined(ACS_PLACEMENT_NEW_DEFINED)
    #define ACS_PLACEMENT_NEW_DEFINED 1
    inline void* operator new  (acs::usize, void* p) noexcept { return p; }
    inline void* operator new[](acs::usize, void* p) noexcept { return p; }
    inline void  operator delete  (void*, void*) noexcept {}
    inline void  operator delete[](void*, void*) noexcept {}
#endif
