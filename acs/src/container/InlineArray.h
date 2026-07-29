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
 * 一度動的領域へ移行した後は Clear 後も確保済み容量を再利用する。これにより、
 * フレームごとに同じピークへ達するキューで確保と解放を繰り返さない。
 */
template<typename T, usize InlineCapacity>
class TInlineArray {
    static_assert(InlineCapacity > 0u);

public:
    TInlineArray() noexcept = default;

    explicit TInlineArray(FAllocator& allocator) noexcept : m_Overflow(allocator) {
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

    usize Size() const noexcept {
        return m_UsingOverflow ? m_Overflow.Size() : m_InlineSize;
    }

    bool IsEmpty() const noexcept {
        return Size() == 0u;
    }

    bool UsesInlineStorage() const noexcept {
        return !m_UsingOverflow;
    }

    T& operator[](usize index) noexcept {
        ACS_ASSERT(index < Size());
        return m_UsingOverflow ? m_Overflow[index] : InlineData()[index];
    }

    const T& operator[](usize index) const noexcept {
        ACS_ASSERT(index < Size());
        return m_UsingOverflow ? m_Overflow[index] : InlineData()[index];
    }

    bool TryPushBack(const T& value) noexcept {
        if (m_UsingOverflow) return m_Overflow.TryPushBack(value);
        if (m_InlineSize < InlineCapacity) {
            ::new (static_cast<void*>(InlineData() + m_InlineSize)) T(value);
            ++m_InlineSize;
            return true;
        }
        if (!MoveToOverflow()) return false;
        return m_Overflow.TryPushBack(value);
    }

    bool TryPushBack(T&& value) noexcept {
        if (m_UsingOverflow) return m_Overflow.TryPushBack(Move(value));
        if (m_InlineSize < InlineCapacity) {
            ::new (static_cast<void*>(InlineData() + m_InlineSize)) T(Move(value));
            ++m_InlineSize;
            return true;
        }
        if (!MoveToOverflow()) return false;
        return m_Overflow.TryPushBack(Move(value));
    }

    void PushBack(const T& value) noexcept {
        ACS_CHECKF(TryPushBack(value), "TInlineArray::PushBack failed (size=%zu, T=%zu)", Size(), sizeof(T));
    }

    void PushBack(T&& value) noexcept {
        ACS_CHECKF(TryPushBack(Move(value)), "TInlineArray::PushBack failed (size=%zu, T=%zu)", Size(), sizeof(T));
    }

    void PopBack() noexcept {
        ACS_ASSERT(!IsEmpty());
        if (m_UsingOverflow) {
            m_Overflow.PopBack();
            return;
        }
        --m_InlineSize;
        InlineData()[m_InlineSize].~T();
    }

    void Clear() noexcept {
        if (m_UsingOverflow) {
            m_Overflow.Clear();
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
            if (!m_Overflow.TryPushBack(Move(InlineData()[i]))) {
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
