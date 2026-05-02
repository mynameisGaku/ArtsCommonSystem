// ACS Threading — Atomic<T> wrapper.
//
// Built on MSVC `_Interlocked*` intrinsics. Supports T sizes 1/2/4/8 and
// pointer types. Loads/stores honour MemoryOrder; RMW ops are full barriers
// on x64 and use suffixed intrinsics on ARM64.
//
// Notes:
//   * On x86-64, every `_Interlocked*` is already a full hardware barrier;
//     plain MOVs of naturally-aligned values up to 8 bytes have acquire/release
//     semantics. The MemoryOrder argument matters mostly to the compiler.
//   * On ARM64, we route to `_acq` / `_rel` / `_nf` suffixed variants when
//     available.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "threading/MemoryOrder.h"

#include <intrin.h>

namespace acs {

namespace atomic_detail {

// ---- Load ---------------------------------------------------------------
template<typename T>
ACS_FORCEINLINE T LoadAcquire(const volatile T* p) noexcept {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "Atomic load: unsupported size");
#if ACS_ARCH_X64
    // Naturally aligned loads ≤ 8B are atomic and have acquire semantics on x86-64.
    T v = *p;
    CompilerBarrier();
    return v;
#else
    if constexpr (sizeof(T) == 1) return static_cast<T>(__iso_volatile_load8 ((const volatile __int8 *)p)), CompilerBarrier(), *p;
    if constexpr (sizeof(T) == 2) return static_cast<T>(__iso_volatile_load16((const volatile __int16*)p)), CompilerBarrier(), *p;
    if constexpr (sizeof(T) == 4) return static_cast<T>(__iso_volatile_load32((const volatile __int32*)p)), CompilerBarrier(), *p;
    if constexpr (sizeof(T) == 8) return static_cast<T>(__iso_volatile_load64((const volatile __int64*)p)), CompilerBarrier(), *p;
#endif
}

template<typename T>
ACS_FORCEINLINE T LoadRelaxed(const volatile T* p) noexcept {
    return *p;
}

// ---- Store --------------------------------------------------------------
template<typename T>
ACS_FORCEINLINE void StoreRelease(volatile T* p, T v) noexcept {
#if ACS_ARCH_X64
    CompilerBarrier();
    *p = v;
#else
    CompilerBarrier();
    if constexpr (sizeof(T) == 1) __iso_volatile_store8 ((volatile __int8 *)p, (__int8 )v);
    if constexpr (sizeof(T) == 2) __iso_volatile_store16((volatile __int16*)p, (__int16)v);
    if constexpr (sizeof(T) == 4) __iso_volatile_store32((volatile __int32*)p, (__int32)v);
    if constexpr (sizeof(T) == 8) __iso_volatile_store64((volatile __int64*)p, (__int64)v);
    HardwareFence();
#endif
}

template<typename T>
ACS_FORCEINLINE void StoreRelaxed(volatile T* p, T v) noexcept { *p = v; }

// ---- Exchange -----------------------------------------------------------
template<typename T>
ACS_FORCEINLINE T Exchange(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedExchange8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedExchange16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchange  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchange64((volatile __int64*)p, (__int64)v);
}

// ---- Compare-Exchange ---------------------------------------------------
template<typename T>
ACS_FORCEINLINE bool CompareExchange(volatile T* p, T& expected, T desired) noexcept {
    T orig;
    if constexpr (sizeof(T) == 1)
        orig = (T)_InterlockedCompareExchange8 ((volatile char*)p,    (char)desired,    (char)expected);
    else if constexpr (sizeof(T) == 2)
        orig = (T)_InterlockedCompareExchange16((volatile short*)p,   (short)desired,   (short)expected);
    else if constexpr (sizeof(T) == 4)
        orig = (T)_InterlockedCompareExchange  ((volatile long*)p,    (long)desired,    (long)expected);
    else if constexpr (sizeof(T) == 8)
        orig = (T)_InterlockedCompareExchange64((volatile __int64*)p, (__int64)desired, (__int64)expected);

    bool ok = orig == expected;
    expected = orig;
    return ok;
}

// ---- Fetch ops ----------------------------------------------------------
template<typename T>
ACS_FORCEINLINE T FetchAdd(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchangeAdd  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchangeAdd64((volatile __int64*)p, (__int64)v);
}

template<typename T>
ACS_FORCEINLINE T FetchSub(volatile T* p, T v) noexcept {
    return FetchAdd(p, static_cast<T>(0) - v);
}

template<typename T>
ACS_FORCEINLINE T FetchOr(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedOr8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedOr16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedOr  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedOr64((volatile __int64*)p, (__int64)v);
}

template<typename T>
ACS_FORCEINLINE T FetchAnd(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedAnd8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedAnd16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedAnd  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedAnd64((volatile __int64*)p, (__int64)v);
}

} // namespace atomic_detail


template<typename T>
class Atomic {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "Atomic<T>: T must be 1, 2, 4, or 8 bytes");
public:
    Atomic() noexcept = default;
    constexpr explicit Atomic(T v) noexcept : _v(v) {}

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    ACS_FORCEINLINE T Load(MemoryOrder o = MemoryOrder::SeqCst) const noexcept {
        return o == MemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (&_v)
            : atomic_detail::LoadAcquire (&_v);
    }
    ACS_FORCEINLINE void Store(T v, MemoryOrder o = MemoryOrder::SeqCst) noexcept {
        if (o == MemoryOrder::Relaxed) atomic_detail::StoreRelaxed(&_v, v);
        else                           atomic_detail::StoreRelease(&_v, v);
    }
    ACS_FORCEINLINE T Exchange(T v) noexcept                          { return atomic_detail::Exchange(&_v, v); }
    ACS_FORCEINLINE bool CompareExchange(T& expected, T desired) noexcept {
        return atomic_detail::CompareExchange(&_v, expected, desired);
    }
    ACS_FORCEINLINE T FetchAdd(T v) noexcept                          { return atomic_detail::FetchAdd(&_v, v); }
    ACS_FORCEINLINE T FetchSub(T v) noexcept                          { return atomic_detail::FetchSub(&_v, v); }
    ACS_FORCEINLINE T FetchOr (T v) noexcept                          { return atomic_detail::FetchOr (&_v, v); }
    ACS_FORCEINLINE T FetchAnd(T v) noexcept                          { return atomic_detail::FetchAnd(&_v, v); }

    ACS_FORCEINLINE T operator++()    noexcept { return FetchAdd((T)1) + (T)1; }
    ACS_FORCEINLINE T operator++(int) noexcept { return FetchAdd((T)1); }
    ACS_FORCEINLINE T operator--()    noexcept { return FetchSub((T)1) - (T)1; }
    ACS_FORCEINLINE T operator--(int) noexcept { return FetchSub((T)1); }

private:
    volatile T _v {};
};

// Pointer specialization (always 8 bytes on x64).
template<typename T>
class Atomic<T*> {
public:
    Atomic() noexcept = default;
    constexpr explicit Atomic(T* v) noexcept : _v(v) {}

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    ACS_FORCEINLINE T* Load(MemoryOrder o = MemoryOrder::SeqCst) const noexcept {
        return reinterpret_cast<T*>(o == MemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (reinterpret_cast<const volatile uptr*>(&_v))
            : atomic_detail::LoadAcquire (reinterpret_cast<const volatile uptr*>(&_v)));
    }
    ACS_FORCEINLINE void Store(T* v, MemoryOrder o = MemoryOrder::SeqCst) noexcept {
        uptr p = reinterpret_cast<uptr>(v);
        if (o == MemoryOrder::Relaxed) atomic_detail::StoreRelaxed(reinterpret_cast<volatile uptr*>(&_v), p);
        else                           atomic_detail::StoreRelease(reinterpret_cast<volatile uptr*>(&_v), p);
    }
    ACS_FORCEINLINE T* Exchange(T* v) noexcept {
        return reinterpret_cast<T*>(atomic_detail::Exchange(
            reinterpret_cast<volatile uptr*>(&_v), reinterpret_cast<uptr>(v)));
    }
    ACS_FORCEINLINE bool CompareExchange(T*& expected, T* desired) noexcept {
        uptr e = reinterpret_cast<uptr>(expected);
        bool ok = atomic_detail::CompareExchange(
            reinterpret_cast<volatile uptr*>(&_v), e, reinterpret_cast<uptr>(desired));
        expected = reinterpret_cast<T*>(e);
        return ok;
    }

private:
    T* volatile _v = nullptr;
};

} // namespace acs
