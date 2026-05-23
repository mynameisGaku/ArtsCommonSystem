// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Threading — Atomic<T>（std::atomic 代替）
// -----------------------------------------------------------------------------
// MSVC の _Interlocked* 組み込み関数をテンプレートでラップ。
// サポート対象: 1 / 2 / 4 / 8 バイトの整数 / 列挙 / ポインタ。
//
// メモ:
//   - x64 では「自然整列の 8 バイト以下のロード/ストア」は CPU レベルで
//     アトミック。さらに普通の MOV が acquire / release セマンティクスを
//     満たすため、Load/Store はコンパイラバリアだけで足りる。
//   - ARM64 では弱メモリモデルなので、明示的に _acq / _rel サフィックスを
//     付けた組み込みを呼んで dmb を最小化する。
//   - RMW 系（Exchange / CompareExchange / FetchAdd...）は x64 では常に
//     完全バリア。ARM64 ではサフィックス付き版を使うが現状実装は無印で統一。
// =============================================================================
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "threading/EMemoryOrder.h"

#include <intrin.h>

namespace acs {

// =============================================================================
// 内部実装ヘルパ（テンプレート分岐用）
// =============================================================================
namespace atomic_detail {

// ---- ロード ----
// acquire セマンティクスでのロード。x64 では普通のロード + コンパイラバリア。
template<typename T>
ACS_FORCEINLINE T LoadAcquire(const volatile T* p) noexcept {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "Atomic load: unsupported size");
#if ACS_ARCH_X64
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

// relaxed ロード（順序保証なし、最高速）
template<typename T>
ACS_FORCEINLINE T LoadRelaxed(const volatile T* p) noexcept {
    return *p;
}

// ---- ストア ----
// release セマンティクスでのストア。
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

// relaxed ストア
template<typename T>
ACS_FORCEINLINE void StoreRelaxed(volatile T* p, T v) noexcept { *p = v; }

// ---- Exchange (アトミック交換) ----
// 古い値を返しつつ新しい値を書き込む（XCHG 相当）
template<typename T>
ACS_FORCEINLINE T Exchange(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedExchange8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedExchange16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchange  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchange64((volatile __int64*)p, (__int64)v);
}

// ---- CompareExchange (CAS) ----
// 比較交換: *p == expected なら *p = desired にして true を返す。
// 失敗時は expected に「実際の値」を書き戻して false を返す。
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
    expected = orig;  // 失敗時は呼び出し元へ「実際の値」を返す
    return ok;
}

// ---- FetchAdd / FetchSub ----
// 加算 / 減算しつつ「加算前」の値を返す
template<typename T>
ACS_FORCEINLINE T FetchAdd(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchangeAdd  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchangeAdd64((volatile __int64*)p, (__int64)v);
}

template<typename T>
ACS_FORCEINLINE T FetchSub(volatile T* p, T v) noexcept {
    return FetchAdd(p, static_cast<T>(0) - v);  // 2 の補数で減算を加算として扱う
}

// ---- FetchOr / FetchAnd ----
// ビット OR / AND しつつ「演算前」の値を返す
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


// =============================================================================
// Atomic<T> — 値型用
// =============================================================================
template<typename T>
class Atomic {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "Atomic<T>: T must be 1, 2, 4, or 8 bytes");
public:
    Atomic() noexcept = default;
    constexpr explicit Atomic(T v) noexcept : _v(v) {}

    // アトミック型はコピーできない（std::atomic と同様の制約）
    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    // ---- ロード ----
    ACS_FORCEINLINE T Load(EMemoryOrder o = EMemoryOrder::SeqCst) const noexcept {
        return o == EMemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (&_v)
            : atomic_detail::LoadAcquire (&_v);
    }
    // ---- ストア ----
    ACS_FORCEINLINE void Store(T v, EMemoryOrder o = EMemoryOrder::SeqCst) noexcept {
        if (o == EMemoryOrder::Relaxed) atomic_detail::StoreRelaxed(&_v, v);
        else                           atomic_detail::StoreRelease(&_v, v);
    }
    // ---- RMW 群 ----
    ACS_FORCEINLINE T Exchange(T v) noexcept                          { return atomic_detail::Exchange(&_v, v); }
    ACS_FORCEINLINE bool CompareExchange(T& expected, T desired) noexcept {
        return atomic_detail::CompareExchange(&_v, expected, desired);
    }
    ACS_FORCEINLINE T FetchAdd(T v) noexcept                          { return atomic_detail::FetchAdd(&_v, v); }
    ACS_FORCEINLINE T FetchSub(T v) noexcept                          { return atomic_detail::FetchSub(&_v, v); }
    ACS_FORCEINLINE T FetchOr (T v) noexcept                          { return atomic_detail::FetchOr (&_v, v); }
    ACS_FORCEINLINE T FetchAnd(T v) noexcept                          { return atomic_detail::FetchAnd(&_v, v); }

    // ---- インクリメント / デクリメント ----
    ACS_FORCEINLINE T operator++()    noexcept { return FetchAdd((T)1) + (T)1; }
    ACS_FORCEINLINE T operator++(int) noexcept { return FetchAdd((T)1); }
    ACS_FORCEINLINE T operator--()    noexcept { return FetchSub((T)1) - (T)1; }
    ACS_FORCEINLINE T operator--(int) noexcept { return FetchSub((T)1); }

private:
    volatile T _v {};
};

// =============================================================================
// Atomic<T*> — ポインタ用特殊化（ポインタは x64 では常に 8 バイト）
// =============================================================================
template<typename T>
class Atomic<T*> {
public:
    Atomic() noexcept = default;
    constexpr explicit Atomic(T* v) noexcept : _v(v) {}

    Atomic(const Atomic&) = delete;
    Atomic& operator=(const Atomic&) = delete;

    ACS_FORCEINLINE T* Load(EMemoryOrder o = EMemoryOrder::SeqCst) const noexcept {
        return reinterpret_cast<T*>(o == EMemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (reinterpret_cast<const volatile uptr*>(&_v))
            : atomic_detail::LoadAcquire (reinterpret_cast<const volatile uptr*>(&_v)));
    }
    ACS_FORCEINLINE void Store(T* v, EMemoryOrder o = EMemoryOrder::SeqCst) noexcept {
        uptr p = reinterpret_cast<uptr>(v);
        if (o == EMemoryOrder::Relaxed) atomic_detail::StoreRelaxed(reinterpret_cast<volatile uptr*>(&_v), p);
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
