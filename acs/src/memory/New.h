// ACS Memory — Allocator-aware new/delete helpers.
#pragma once

#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "memory/Allocator.h"

namespace acs {

template<typename T, typename... Args>
ACS_FORCEINLINE T* New(Allocator& a, Args&&... args) noexcept {
    void* p = a.Alloc(sizeof(T), alignof(T), SourceLoc::Current());
    if (!p) return nullptr;
    return ::new (p) T(Forward<Args>(args)...);
}

template<typename T>
ACS_FORCEINLINE void Delete(Allocator& a, T* p) noexcept {
    if (!p) return;
    if constexpr (!IsTriviallyDestructibleV<T>) p->~T();
    a.Free(static_cast<void*>(p));
}

template<typename T>
ACS_FORCEINLINE T* NewArray(Allocator& a, usize n) noexcept {
    if (n == 0) return nullptr;
    void* p = a.Alloc(sizeof(T) * n, alignof(T), SourceLoc::Current());
    if (!p) return nullptr;
    T* arr = static_cast<T*>(p);
    for (usize i = 0; i < n; ++i) ::new (&arr[i]) T();
    return arr;
}

template<typename T>
ACS_FORCEINLINE void DeleteArray(Allocator& a, T* arr, usize n) noexcept {
    if (!arr) return;
    if constexpr (!IsTriviallyDestructibleV<T>) {
        for (usize i = n; i-- > 0;) arr[i].~T();
    }
    a.Free(static_cast<void*>(arr));
}

} // namespace acs
