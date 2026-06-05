// SPDX-License-Identifier: Apache-2.0
// ACS Threading — TAtomic<T>（std::atomic 代替）
//
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
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "threading/MemoryOrder.h"

#include <intrin.h>

namespace acs {

/** TAtomic のテンプレート分岐用の低レベルアトミック組み込みラッパ群。 */
namespace atomic_detail {

/**
 * acquire セマンティクスでアトミックにロードする。
 *
 * @details x64 では普通のロード + コンパイラバリア、ARM64 では volatile ロード後にバリア。
 * @tparam T ロードする値型 (1/2/4/8 バイト)。
 * @param p ロード元のポインタ。
 * @return *p の値。
 */
template<typename T>
ACS_FORCEINLINE T LoadAcquire(const volatile T* p) noexcept {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "TAtomic load: unsupported size");
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

/**
 * relaxed セマンティクスでアトミックにロードする (順序保証なし、最高速)。
 *
 * @tparam T ロードする値型。
 * @param p ロード元のポインタ。
 * @return *p の値。
 */
template<typename T>
ACS_FORCEINLINE T LoadRelaxed(const volatile T* p) noexcept {
    return *p;
}

/**
 * release セマンティクスでアトミックにストアする。
 *
 * @details x64 ではコンパイラバリア + 普通のストア、ARM64 では volatile ストア + ハードウェアフェンス。
 * @tparam T ストアする値型 (1/2/4/8 バイト)。
 * @param p ストア先のポインタ。
 * @param v 書き込む値。
 */
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

/**
 * relaxed セマンティクスでアトミックにストアする (順序保証なし)。
 *
 * @tparam T ストアする値型。
 * @param p ストア先のポインタ。
 * @param v 書き込む値。
 */
template<typename T>
ACS_FORCEINLINE void StoreRelaxed(volatile T* p, T v) noexcept { *p = v; }

/**
 * アトミックに値を交換する (XCHG 相当)。
 *
 * @tparam T 交換する値型 (1/2/4/8 バイト)。
 * @param p 対象のポインタ。
 * @param v 書き込む新しい値。
 * @return 交換前の古い値。
 */
template<typename T>
ACS_FORCEINLINE T Exchange(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedExchange8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedExchange16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchange  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchange64((volatile __int64*)p, (__int64)v);
}

/**
 * アトミックな比較交換 (CAS) を行う。
 *
 * @details
 * *p == expected なら *p = desired にして true を返す。失敗時は expected に
 * 「実際の値」を書き戻して false を返す (再試行ループで使える)。
 * @tparam T 対象の値型 (1/2/4/8 バイト)。
 * @param p 対象のポインタ。
 * @param expected 期待値。入出力で、失敗時は実際の現在値が書き戻される。
 * @param desired 一致時に書き込む値。
 * @return 交換に成功したら true。
 */
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

/**
 * アトミックに加算し、加算前の値を返す。
 *
 * @tparam T 対象の値型 (4 または 8 バイト)。
 * @param p 対象のポインタ。
 * @param v 加算する値。
 * @return 加算前の値。
 */
template<typename T>
ACS_FORCEINLINE T FetchAdd(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 4) return (T)_InterlockedExchangeAdd  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedExchangeAdd64((volatile __int64*)p, (__int64)v);
}

/**
 * アトミックに減算し、減算前の値を返す。
 *
 * @details 2 の補数を加算することで FetchAdd に帰着させる。
 * @tparam T 対象の値型 (4 または 8 バイト)。
 * @param p 対象のポインタ。
 * @param v 減算する値。
 * @return 減算前の値。
 */
template<typename T>
ACS_FORCEINLINE T FetchSub(volatile T* p, T v) noexcept {
    return FetchAdd(p, static_cast<T>(0) - v);  // 2 の補数で減算を加算として扱う
}

/**
 * アトミックにビット OR を取り、演算前の値を返す。
 *
 * @tparam T 対象の値型 (1/2/4/8 バイト)。
 * @param p 対象のポインタ。
 * @param v OR するビットマスク。
 * @return 演算前の値。
 */
template<typename T>
ACS_FORCEINLINE T FetchOr(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedOr8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedOr16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedOr  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedOr64((volatile __int64*)p, (__int64)v);
}

/**
 * アトミックにビット AND を取り、演算前の値を返す。
 *
 * @tparam T 対象の値型 (1/2/4/8 バイト)。
 * @param p 対象のポインタ。
 * @param v AND するビットマスク。
 * @return 演算前の値。
 */
template<typename T>
ACS_FORCEINLINE T FetchAnd(volatile T* p, T v) noexcept {
    if constexpr (sizeof(T) == 1) return (T)_InterlockedAnd8 ((volatile char*)p,    (char)v);
    if constexpr (sizeof(T) == 2) return (T)_InterlockedAnd16((volatile short*)p,   (short)v);
    if constexpr (sizeof(T) == 4) return (T)_InterlockedAnd  ((volatile long*)p,    (long)v);
    if constexpr (sizeof(T) == 8) return (T)_InterlockedAnd64((volatile __int64*)p, (__int64)v);
}

} // namespace atomic_detail


/**
 * std::atomic 代替のアトミック値型。
 *
 * @details
 * MSVC の _Interlocked* 組み込みをラップし、1/2/4/8 バイトの整数・列挙を
 * アトミックに扱う。コピー不可。メモリ順序は各操作で EMemoryOrder を指定する。
 * @tparam T 1/2/4/8 バイトのトリビアルな値型。
 */
template<typename T>
class TAtomic {
    static_assert(sizeof(T) == 1 || sizeof(T) == 2 || sizeof(T) == 4 || sizeof(T) == 8,
                  "TAtomic<T>: T must be 1, 2, 4, or 8 bytes");
public:
    /** 値を 0 初期化して構築する。 */
    TAtomic() noexcept = default;

    /**
     * 初期値を指定して構築する。
     *
     * @param v 初期値。
     */
    constexpr explicit TAtomic(T v) noexcept : m_V(v) {}

    /** コピー禁止 (std::atomic と同様の制約)。 */
    TAtomic(const TAtomic&) = delete;

    /** コピー代入も禁止。 */
    TAtomic& operator=(const TAtomic&) = delete;

    /**
     * 値をアトミックにロードする。
     *
     * @param o メモリ順序 (既定 SeqCst、Relaxed 以外は acquire 扱い)。
     * @return 読み出した値。
     */
    ACS_FORCEINLINE T Load(EMemoryOrder o = EMemoryOrder::SeqCst) const noexcept {
        return o == EMemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (&m_V)
            : atomic_detail::LoadAcquire (&m_V);
    }

    /**
     * 値をアトミックにストアする。
     *
     * @param v 書き込む値。
     * @param o メモリ順序 (既定 SeqCst、Relaxed 以外は release 扱い)。
     */
    ACS_FORCEINLINE void Store(T v, EMemoryOrder o = EMemoryOrder::SeqCst) noexcept {
        if (o == EMemoryOrder::Relaxed) atomic_detail::StoreRelaxed(&m_V, v);
        else                           atomic_detail::StoreRelease(&m_V, v);
    }

    /**
     * 値をアトミックに交換する。
     *
     * @param v 書き込む新しい値。
     * @return 交換前の古い値。
     */
    ACS_FORCEINLINE T Exchange(T v) noexcept                          { return atomic_detail::Exchange(&m_V, v); }

    /**
     * アトミックな比較交換 (CAS) を行う。
     *
     * @param expected 期待値。失敗時は実際の現在値が書き戻される。
     * @param desired 一致時に書き込む値。
     * @return 交換に成功したら true。
     */
    ACS_FORCEINLINE bool CompareExchange(T& expected, T desired) noexcept {
        return atomic_detail::CompareExchange(&m_V, expected, desired);
    }

    /**
     * アトミックに加算し、加算前の値を返す。
     *
     * @param v 加算する値。
     * @return 加算前の値。
     */
    ACS_FORCEINLINE T FetchAdd(T v) noexcept                          { return atomic_detail::FetchAdd(&m_V, v); }

    /**
     * アトミックに減算し、減算前の値を返す。
     *
     * @param v 減算する値。
     * @return 減算前の値。
     */
    ACS_FORCEINLINE T FetchSub(T v) noexcept                          { return atomic_detail::FetchSub(&m_V, v); }

    /**
     * アトミックにビット OR を取り、演算前の値を返す。
     *
     * @param v OR するビットマスク。
     * @return 演算前の値。
     */
    ACS_FORCEINLINE T FetchOr (T v) noexcept                          { return atomic_detail::FetchOr (&m_V, v); }

    /**
     * アトミックにビット AND を取り、演算前の値を返す。
     *
     * @param v AND するビットマスク。
     * @return 演算前の値。
     */
    ACS_FORCEINLINE T FetchAnd(T v) noexcept                          { return atomic_detail::FetchAnd(&m_V, v); }

    /**
     * 前置インクリメント。アトミックに 1 加算する。
     *
     * @return インクリメント後の値。
     */
    ACS_FORCEINLINE T operator++()    noexcept { return FetchAdd((T)1) + (T)1; }

    /**
     * 後置インクリメント。アトミックに 1 加算する。
     *
     * @return インクリメント前の値。
     */
    ACS_FORCEINLINE T operator++(int) noexcept { return FetchAdd((T)1); }

    /**
     * 前置デクリメント。アトミックに 1 減算する。
     *
     * @return デクリメント後の値。
     */
    ACS_FORCEINLINE T operator--()    noexcept { return FetchSub((T)1) - (T)1; }

    /**
     * 後置デクリメント。アトミックに 1 減算する。
     *
     * @return デクリメント前の値。
     */
    ACS_FORCEINLINE T operator--(int) noexcept { return FetchSub((T)1); }

private:
    /** アトミックに操作される実体ストレージ。 */
    volatile T m_V {};
};

/**
 * TAtomic のポインタ用特殊化。
 *
 * @details
 * ポインタを uptr に再解釈してアトミックに扱う (x64 では常に 8 バイト)。
 * 整数版にあるビット演算・算術 RMW は持たず、Load/Store/Exchange/CompareExchange のみ。
 * @tparam T ポインタが指す型。
 */
template<typename T>
class TAtomic<T*> {
public:
    /** ポインタを nullptr 初期化して構築する。 */
    TAtomic() noexcept = default;

    /**
     * 初期ポインタを指定して構築する。
     *
     * @param v 初期ポインタ。
     */
    constexpr explicit TAtomic(T* v) noexcept : m_V(v) {}

    /** コピー禁止 (std::atomic と同様の制約)。 */
    TAtomic(const TAtomic&) = delete;

    /** コピー代入も禁止。 */
    TAtomic& operator=(const TAtomic&) = delete;

    /**
     * ポインタをアトミックにロードする。
     *
     * @param o メモリ順序 (既定 SeqCst、Relaxed 以外は acquire 扱い)。
     * @return 読み出したポインタ。
     */
    ACS_FORCEINLINE T* Load(EMemoryOrder o = EMemoryOrder::SeqCst) const noexcept {
        return reinterpret_cast<T*>(o == EMemoryOrder::Relaxed
            ? atomic_detail::LoadRelaxed (reinterpret_cast<const volatile uptr*>(&m_V))
            : atomic_detail::LoadAcquire (reinterpret_cast<const volatile uptr*>(&m_V)));
    }

    /**
     * ポインタをアトミックにストアする。
     *
     * @param v 書き込むポインタ。
     * @param o メモリ順序 (既定 SeqCst、Relaxed 以外は release 扱い)。
     */
    ACS_FORCEINLINE void Store(T* v, EMemoryOrder o = EMemoryOrder::SeqCst) noexcept {
        uptr p = reinterpret_cast<uptr>(v);
        if (o == EMemoryOrder::Relaxed) atomic_detail::StoreRelaxed(reinterpret_cast<volatile uptr*>(&m_V), p);
        else                           atomic_detail::StoreRelease(reinterpret_cast<volatile uptr*>(&m_V), p);
    }

    /**
     * ポインタをアトミックに交換する。
     *
     * @param v 書き込む新しいポインタ。
     * @return 交換前の古いポインタ。
     */
    ACS_FORCEINLINE T* Exchange(T* v) noexcept {
        return reinterpret_cast<T*>(atomic_detail::Exchange(
            reinterpret_cast<volatile uptr*>(&m_V), reinterpret_cast<uptr>(v)));
    }

    /**
     * ポインタのアトミックな比較交換 (CAS) を行う。
     *
     * @param expected 期待ポインタ。失敗時は実際の現在値が書き戻される。
     * @param desired 一致時に書き込むポインタ。
     * @return 交換に成功したら true。
     */
    ACS_FORCEINLINE bool CompareExchange(T*& expected, T* desired) noexcept {
        uptr e = reinterpret_cast<uptr>(expected);
        bool ok = atomic_detail::CompareExchange(
            reinterpret_cast<volatile uptr*>(&m_V), e, reinterpret_cast<uptr>(desired));
        expected = reinterpret_cast<T*>(e);
        return ok;
    }

private:
    /** アトミックに操作される実体ポインタストレージ。 */
    T* volatile m_V = nullptr;
};

} // namespace acs
