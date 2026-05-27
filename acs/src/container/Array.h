// SPDX-License-Identifier: Apache-2.0
// 可変長配列（std::vector 代替、アロケータ注入可能、ムーブ専用）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"
#include "foundation/TypeTraits.h"
#include "foundation/Assert.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"
#include "container/Span.h"

namespace acs {

template<typename T>
class TArray {
public:
    TArray() noexcept : m_Alloc(&DefaultAllocator()) {}
    explicit TArray(FAllocator& a) noexcept : m_Alloc(&a) {}
    TArray(usize initial_capacity, FAllocator& a = DefaultAllocator()) noexcept : m_Alloc(&a) {
        Reserve(initial_capacity);
    }

    // 明示的な Clone を強制してパフォーマンス事故を防ぐためコピー禁止
    TArray(const TArray&) = delete;
    TArray& operator=(const TArray&) = delete;

    // ムーブで所有権を奪う
    TArray(TArray&& o) noexcept
        : m_Data(o.m_Data), m_Size(o.m_Size), m_Capacity(o.m_Capacity), m_Alloc(o.m_Alloc) {
        o.m_Data = nullptr; o.m_Size = 0; o.m_Capacity = 0;
    }
    TArray& operator=(TArray&& o) noexcept {
        if (this == &o) return *this;
        Clear();
        Free();
        m_Data = o.m_Data; m_Size = o.m_Size; m_Capacity = o.m_Capacity; m_Alloc = o.m_Alloc;
        o.m_Data = nullptr; o.m_Size = 0; o.m_Capacity = 0;
        return *this;
    }
    ~TArray() noexcept { Clear(); Free(); }

    usize Size()     const noexcept { return m_Size; }
    usize Capacity() const noexcept { return m_Capacity; }
    bool  IsEmpty()  const noexcept { return m_Size == 0; }

    // 容量を予約（既存要素は保持）
    void Reserve(usize new_capacity) noexcept {
        if (new_capacity <= m_Capacity) return;
        Grow(new_capacity);
    }

    // サイズを変更（増やす場合はデフォルト構築、減らす場合はデストラクト）
    void Resize(usize new_size) noexcept {
        if (new_size > m_Capacity) Grow(NextGrow(new_size));
        if (new_size > m_Size) {
            if constexpr (IsTriviallyConstructibleV<T>) {
                MemSet(static_cast<void*>(m_Data + m_Size), 0, sizeof(T) * (new_size - m_Size));
            } else {
                for (usize i = m_Size; i < new_size; ++i) ::new (&m_Data[i]) T();
            }
        } else if (new_size < m_Size) {
            if constexpr (!IsTriviallyDestructibleV<T>) {
                for (usize i = new_size; i < m_Size; ++i) m_Data[i].~T();
            }
        }
        m_Size = new_size;
    }

    // サイズを 0 に（容量は保持）
    void Clear() noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            for (usize i = m_Size; i-- > 0;) m_Data[i].~T();
        }
        m_Size = 0;
    }

    // 余剰容量を解放
    void ShrinkToFit() noexcept {
        if (m_Size == m_Capacity) return;
        Grow(m_Size);
    }

    T*       Data()       noexcept { return m_Data; }
    const T* Data() const noexcept { return m_Data; }
    T&       operator[](usize i)       noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }
    const T& operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }
    T&       Front()       noexcept { ACS_ASSERT(m_Size > 0); return m_Data[0]; }
    const T& Front() const noexcept { ACS_ASSERT(m_Size > 0); return m_Data[0]; }
    T&       Back ()       noexcept { ACS_ASSERT(m_Size > 0); return m_Data[m_Size - 1]; }
    const T& Back () const noexcept { ACS_ASSERT(m_Size > 0); return m_Data[m_Size - 1]; }

    T*       begin()       noexcept { return m_Data; }
    T*       end()         noexcept { return m_Data + m_Size; }
    const T* begin() const noexcept { return m_Data; }
    const T* end()   const noexcept { return m_Data + m_Size; }

    TSpan<T>       AsSpan()       noexcept { return TSpan<T>(m_Data, m_Size); }
    TSpan<const T> AsSpan() const noexcept { return TSpan<const T>(m_Data, m_Size); }

    void PushBack(const T& v) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(v);
        ++m_Size;
    }
    void PushBack(T&& v) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(Move(v));
        ++m_Size;
    }
    template<typename... Args>
    T& EmplaceBack(Args&&... args) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(Forward<Args>(args)...);
        return m_Data[m_Size++];
    }
    void PopBack() noexcept {
        ACS_ASSERT(m_Size > 0);
        --m_Size;
        if constexpr (!IsTriviallyDestructibleV<T>) m_Data[m_Size].~T();
    }

    // i 番に末尾要素をムーブして縮める（順序は保たれない、削除が高速）
    void RemoveAtSwap(usize i) noexcept {
        ACS_ASSERT(i < m_Size);
        --m_Size;
        if (i != m_Size) m_Data[i] = Move(m_Data[m_Size]);
        if constexpr (!IsTriviallyDestructibleV<T>) m_Data[m_Size].~T();
    }

    // 明示的なコピー（高コストなので名前で意識させる）
    TArray Clone() const noexcept {
        TArray c(m_Capacity, *m_Alloc);
        c.Resize(m_Size);
        if constexpr (IsTriviallyCopyableV<T>) {
            MemCopy(c.m_Data, m_Data, sizeof(T) * m_Size);
        } else {
            for (usize i = 0; i < m_Size; ++i) c.m_Data[i] = m_Data[i];
        }
        return c;
    }

    FAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    // 容量増加戦略（約 1.5 倍ずつ）
    static usize NextGrow(usize required) noexcept {
        usize n = 8;
        while (n < required) n += n / 2 + 1;
        return n;
    }

    // 新容量で再確保し、既存要素をムーブ移送
    void Grow(usize new_capacity) noexcept {
        T* new_data = static_cast<T*>(m_Alloc->Alloc(sizeof(T) * new_capacity, alignof(T), FSourceLoc::Current()));
        ACS_ASSERTF(new_data != nullptr, "TArray::Grow: allocator returned null (cap=%zu, T=%zu)",
                    new_capacity, sizeof(T));
        if (m_Data) {
            if constexpr (IsTriviallyCopyableV<T>) {
                MemCopy(new_data, m_Data, sizeof(T) * m_Size);  // POD はバルクコピー
            } else {
                for (usize i = 0; i < m_Size; ++i) {
                    ::new (&new_data[i]) T(Move(m_Data[i]));
                    if constexpr (!IsTriviallyDestructibleV<T>) m_Data[i].~T();
                }
            }
            m_Alloc->Free(m_Data);
        }
        m_Data = new_data;
        m_Capacity = new_capacity;
    }

    void Free() noexcept {
        if (m_Data) { m_Alloc->Free(m_Data); m_Data = nullptr; m_Capacity = 0; }
    }

    T*         m_Data     = nullptr;
    usize      m_Size     = 0;
    usize      m_Capacity = 0;
    FAllocator* m_Alloc    = nullptr;
};

} // namespace acs
