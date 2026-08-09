// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "container/Array.h"
#include "foundation/Assert.h"
#include "foundation/Move.h"

namespace acs {

/**
 * 小規模な配列をオブジェクト内に保持し、容量超過時だけ動的配列へ移行する。
 *
 * @details 動的領域へ移行した場合の再利用規則を次に示す。
 * 一度動的領域へ移行した後は Reset 後も確保済み容量を再利用する。これにより、
 * フレームごとに同じピークへ達するキューで確保と解放を繰り返さない。
 */
template<typename T, usize InlineCapacity>
class TInlineArray {
    static_assert(InlineCapacity > 0u);

public:
    TInlineArray() noexcept = default;

    explicit TInlineArray(IAllocator& allocator) noexcept : m_Overflow(allocator) {
    }

    TInlineArray(const TInlineArray&) = delete;
    TInlineArray& operator=(const TInlineArray&) = delete;

    TInlineArray(TInlineArray&& other) noexcept : m_Overflow(Move(other.m_Overflow)), m_InlineSize(other.m_InlineSize), m_UsingOverflow(other.m_UsingOverflow) {
        if (!m_UsingOverflow) {
            for (usize i = 0u; i < m_InlineSize; ++i) {
                ::new (static_cast<void*>(InlineData() + i)) T(Move(other.InlineData()[i]));
                other.InlineData()[i].~T();
            }
        }
        other.m_InlineSize = 0u;
        other.m_UsingOverflow = false;
    }

    TInlineArray& operator=(TInlineArray&& other) noexcept {
        if (this == &other) return *this;
        DestroyInline();
        m_Overflow = Move(other.m_Overflow);
        m_InlineSize = other.m_InlineSize;
        m_UsingOverflow = other.m_UsingOverflow;
        if (!m_UsingOverflow) {
            for (usize i = 0u; i < m_InlineSize; ++i) {
                ::new (static_cast<void*>(InlineData() + i)) T(Move(other.InlineData()[i]));
                other.InlineData()[i].~T();
            }
        }
        other.m_InlineSize = 0u;
        other.m_UsingOverflow = false;
        return *this;
    }

    ~TInlineArray() noexcept {
        DestroyInline();
    }

    usize Num() const noexcept {
        return m_UsingOverflow ? m_Overflow.Num() : m_InlineSize;
    }

    bool IsEmpty() const noexcept {
        return Num() == 0u;
    }

    bool UsesInlineStorage() const noexcept {
        return !m_UsingOverflow;
    }

    T& operator[](usize index) noexcept {
        ACS_ASSERT(index < Num());
        return m_UsingOverflow ? m_Overflow[index] : InlineData()[index];
    }

    const T& operator[](usize index) const noexcept {
        ACS_ASSERT(index < Num());
        return m_UsingOverflow ? m_Overflow[index] : InlineData()[index];
    }

    bool TryAdd(const T& value) noexcept {
        if (m_UsingOverflow) return m_Overflow.TryAdd(value);
        if (m_InlineSize < InlineCapacity) {
            ::new (static_cast<void*>(InlineData() + m_InlineSize)) T(value);
            ++m_InlineSize;
            return true;
        }
        if (!MoveToOverflow()) return false;
        return m_Overflow.TryAdd(value);
    }

    bool TryAdd(T&& value) noexcept {
        if (m_UsingOverflow) return m_Overflow.TryAdd(Move(value));
        if (m_InlineSize < InlineCapacity) {
            ::new (static_cast<void*>(InlineData() + m_InlineSize)) T(Move(value));
            ++m_InlineSize;
            return true;
        }
        if (!MoveToOverflow()) return false;
        return m_Overflow.TryAdd(Move(value));
    }

    void Add(const T& value) noexcept {
        ACS_CHECKF(TryAdd(value), "TInlineArray::Add failed (size=%zu, T=%zu)", Num(), sizeof(T));
    }

    void Add(T&& value) noexcept {
        ACS_CHECKF(TryAdd(Move(value)), "TInlineArray::Add failed (size=%zu, T=%zu)", Num(), sizeof(T));
    }

    void Pop() noexcept {
        ACS_ASSERT(!IsEmpty());
        if (m_UsingOverflow) {
            m_Overflow.Pop();
            return;
        }
        --m_InlineSize;
        InlineData()[m_InlineSize].~T();
    }

    /**
     * value と == で一致する最初の要素を、順序を保って削除する。
     *
     * @details 直接領域と動的領域のどちらでも新しい確保は行わない。動的領域へ
     * 移行済みの場合は、削除後も動的領域を使い続ける。
     * @param value 削除する値。配列内の要素自身を渡してもよい。
     * @return 削除できたら true。見つからなければ配列を変えず false。
     */
    bool Remove(const T& value) noexcept {
        if (m_UsingOverflow) return m_Overflow.Remove(value);
        // 直接領域の先頭。
        T* const data = InlineData();
        // 一致を調べる要素位置。
        for (usize i = 0u; i < m_InlineSize; ++i) {
            if (!(data[i] == value)) continue;
            // 削除位置より後から前へ詰める要素位置。
            for (usize k = i + 1u; k < m_InlineSize; ++k) data[k - 1u] = Move(data[k]);
            --m_InlineSize;
            data[m_InlineSize].~T();
            return true;
        }
        return false;
    }

    void Reset() noexcept {
        if (m_UsingOverflow) {
            m_Overflow.Reset();
            return;
        }
        DestroyInline();
    }

private:
    T* InlineData() noexcept {
        return reinterpret_cast<T*>(m_InlineStorage);
    }

    const T* InlineData() const noexcept {
        return reinterpret_cast<const T*>(m_InlineStorage);
    }

    void DestroyInline() noexcept {
        if (m_UsingOverflow) return;
        while (m_InlineSize > 0u) {
            --m_InlineSize;
            InlineData()[m_InlineSize].~T();
        }
    }

    bool MoveToOverflow() noexcept {
        if (!m_Overflow.TryReserve(InlineCapacity * 2u)) return false;
        for (usize i = 0u; i < m_InlineSize; ++i) {
            if (!m_Overflow.TryAdd(Move(InlineData()[i]))) {
                return false;
            }
        }
        DestroyInline();
        m_UsingOverflow = true;
        return true;
    }

    /** 小規模要素を直接構築する領域。 */
    alignas(T) byte m_InlineStorage[sizeof(T) * InlineCapacity]{};
    /** 容量超過後に使用する動的配列。 */
    TArray<T> m_Overflow;
    /** 直接領域に構築済みの要素数。 */
    usize m_InlineSize = 0u;
    /** 動的配列へ移行済みなら true。 */
    bool m_UsingOverflow = false;
};

} // namespace acs
