// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Assert.h"
#include "foundation/Move.h"
#include "memory/PoolAllocator.h"

namespace acs {

/**
 * 要素型と容量をコンパイル時に固定する型付きプール。
 *
 * @tparam T プールへ格納する要素型。
 * @tparam Capacity 同時に保持できる要素数。
 */
template<typename T, usize Capacity>
class TTypedPoolAllocator final {
    static_assert(Capacity > 0u, "Capacity must be greater than zero");

    /** T とフリーリストポインタの双方を満たす整列値。 */
    static constexpr usize kAlignment = alignof(T) > sizeof(void*) ? alignof(T) : sizeof(void*);

    /** T とフリーリストポインタのうち大きい生ブロック長。 */
    static constexpr usize kRawBlockSize = sizeof(T) > sizeof(void*) ? sizeof(T) : sizeof(void*);

    /** 整列値の倍数へ切り上げた実ブロック長。 */
    static constexpr usize kBlockSize = (kRawBlockSize + kAlignment - 1u) & ~(kAlignment - 1u);

public:
    /**
     * 型から求めた固定レイアウトでプールを構築する。
     *
     * @param BackingAllocator ストレージの確保元。nullptr は既定アロケータを使う。
     */
    explicit TTypedPoolAllocator(IAllocator* BackingAllocator = nullptr) noexcept
        : m_Pool(kBlockSize, Capacity, kAlignment, BackingAllocator)
    {
    }

    /** 破棄前に全構築値が明示的に返却済みであることを検証する。 */
    ~TTypedPoolAllocator() noexcept
    {
        ACS_ASSERTF(m_Pool.AllocationCount() == 0u, "TTypedPoolAllocator の破棄前に全要素を返却する必要があります");
    }

    /** ストレージの単独所有を守るためコピー構築を禁止する。 */
    TTypedPoolAllocator(const TTypedPoolAllocator&) = delete;

    /** ストレージの単独所有を守るためコピー代入を禁止する。 */
    TTypedPoolAllocator& operator=(const TTypedPoolAllocator&) = delete;

    /**
     * 未構築の T 用領域を取得する。
     *
     * @return 取得した領域。プール枯渇時は nullptr。
     */
    T* Allocate() noexcept
    {
        return static_cast<T*>(m_Pool.AllocBlock());
    }

    /**
     * T を引数付き構築して返す。
     *
     * @param Values T のコンストラクタへ転送する値。
     * @return 構築した T。プール枯渇時は nullptr。
     */
    template<typename... Args>
    T* Create(Args&&... Values) noexcept
    {
        /** T を構築する未初期化領域。 */
        void* const Storage = m_Pool.AllocBlock();
        if (Storage == nullptr) return nullptr;
        return ::new (Storage) T(Forward<Args>(Values)...);
    }

    /**
     * 構築済み T を一度だけ破棄してプールへ返す。
     *
     * @param Pointer このプールから取得した構築済み T。
     * @return 破棄できた場合は true。無効ポインタまたは重複破棄は false。
     */
    bool Destroy(T* Pointer) noexcept
    {
        if (!m_Pool.TryBeginDestroyBlock(Pointer)) return false;
        Pointer->~T();
        m_Pool.FinishDestroyBlock(Pointer);
        return true;
    }

    /**
     * 未構築領域をプールへ返す。
     *
     * @param Pointer Allocate で取得した未構築領域。
     */
    void Deallocate(T* Pointer) noexcept
    {
        m_Pool.Free(Pointer);
    }

    /** コンパイル時に決まる実ブロック長を返す。 */
    static constexpr usize BlockSize() noexcept { return kBlockSize; }

    /** コンパイル時に決まるブロック総数を返す。 */
    static constexpr usize BlockCount() noexcept { return Capacity; }

    /** 現在払い出し中のブロック数を返す。 */
    u64 AllocationCount() const noexcept { return m_Pool.AllocationCount(); }

    /** 確保と返却で取得した累積ロック回数を返す。 */
    u64 LockAcquisitionCount() const noexcept { return m_Pool.LockAcquisitionCount(); }

    /** 型なしプールへ可変参照でアクセスする。 */
    CPoolAllocator& Untyped() noexcept { return m_Pool; }

    /** 型なしプールへ読み取り専用参照でアクセスする。 */
    const CPoolAllocator& Untyped() const noexcept { return m_Pool; }

private:
    /** 実ストレージと所有状態を管理する型なしプール。 */
    CPoolAllocator m_Pool;
};

} // namespace acs
