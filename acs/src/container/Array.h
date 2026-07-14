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

/**
 * 可変長配列 (std::vector 代替、アロケータ注入可能なムーブ専用コンテナ)。
 *
 * @details
 * 連続バッファを FAllocator から確保し、容量不足時に約 1.5 倍ずつ Grow する。
 * コピーは禁止 (高コストな複製を明示させるため、複製は Clone() を使う)。要素型が
 * trivial なら memcpy/memset でバルク処理し、そうでなければ placement new と明示的
 * デストラクタ呼び出しで構築・破棄する。
 * @tparam T 要素型。
 */
template<typename T>
class TArray {
public:
    /** 既定アロケータで空の配列を構築する。 */
    TArray() noexcept : m_Alloc(&DefaultAllocator()) {}

    /**
     * 指定アロケータで空の配列を構築する。
     *
     * @param a 確保に使うアロケータ。
     */
    explicit TArray(FAllocator& a) noexcept : m_Alloc(&a) {}

    /**
     * 初期容量を予約して空の配列を構築する。
     *
     * @param initial_capacity 事前に予約する容量。
     * @param a 確保に使うアロケータ (既定は DefaultAllocator)。
     */
    TArray(usize initial_capacity, FAllocator& a = DefaultAllocator()) noexcept : m_Alloc(&a) {
        Reserve(initial_capacity);
    }

    /** コピー禁止 (明示的な Clone を強制してパフォーマンス事故を防ぐ)。 */
    TArray(const TArray&) = delete;

    /** コピー代入も禁止。 */
    TArray& operator=(const TArray&) = delete;

    /**
     * ムーブ構築する (元の配列からバッファ所有権を奪う)。
     *
     * @param o ムーブ元 (空状態にリセットされる)。
     */
    TArray(TArray&& o) noexcept
        : m_Data(o.m_Data), m_Size(o.m_Size), m_Capacity(o.m_Capacity), m_Alloc(o.m_Alloc) {
        o.m_Data = nullptr; o.m_Size = 0; o.m_Capacity = 0;
    }

    /**
     * ムーブ代入する (既存要素を破棄・解放してから所有権を奪う)。
     *
     * @param o ムーブ元 (空状態にリセットされる)。
     * @return *this。
     */
    TArray& operator=(TArray&& o) noexcept {
        if (this == &o) return *this;
        Clear();
        Free();
        m_Data = o.m_Data; m_Size = o.m_Size; m_Capacity = o.m_Capacity; m_Alloc = o.m_Alloc;
        o.m_Data = nullptr; o.m_Size = 0; o.m_Capacity = 0;
        return *this;
    }

    /** 全要素を破棄しバッファを解放する。 */
    ~TArray() noexcept { Clear(); Free(); }

    /**
     * 要素数を返す。
     *
     * @return 現在の要素数。
     */
    usize Size()     const noexcept { return m_Size; }

    /**
     * 確保済み容量を返す。
     *
     * @return 再確保なしで保持できる要素数。
     */
    usize Capacity() const noexcept { return m_Capacity; }

    /**
     * 空かどうかを返す。
     *
     * @return 要素数が 0 なら true。
     */
    bool  IsEmpty()  const noexcept { return m_Size == 0; }

    /**
     * 容量を予約する (既存要素は保持)。
     *
     * @details 既に十分な容量があれば何もしない。
     * @param new_capacity 確保する最小容量。
     */
    void Reserve(usize new_capacity) noexcept {
        if (new_capacity <= m_Capacity) return;
        Grow(new_capacity);
    }

    /**
     * サイズを変更する。
     *
     * @details 増やす場合は新規要素をデフォルト構築 (trivial なら 0 埋め)、減らす
     * 場合は超過要素をデストラクトする。
     * @param new_size 変更後の要素数。
     */
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

    /** サイズを 0 にする (全要素をデストラクトするが容量は保持)。 */
    void Clear() noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            for (usize i = m_Size; i-- > 0;) m_Data[i].~T();
        }
        m_Size = 0;
    }

    /**
     * 全要素を破棄し、確保済みの連続バッファも解放する。
     *
     * @details
     * Clear() と異なり容量を 0 に戻す。配列に設定されたアロケータは維持するため、
     * 次回の Reserve/Resize/PushBack も同じ確保元を使用する。大きな一時データを
     * 明示的に手放したい長寿命オブジェクト向け。
     */
    void ReleaseStorage() noexcept
    {
        Clear();
        Free();
    }

    /** 余剰容量を解放してサイズちょうどに縮める。 */
    void ShrinkToFit() noexcept {
        if (m_Size == m_Capacity) return;
        Grow(m_Size);
    }

    /**
     * 先頭ポインタを返す。
     *
     * @return 内部バッファの先頭ポインタ。
     */
    T*       Data()       noexcept { return m_Data; }

    /**
     * 先頭ポインタを const で返す。
     *
     * @return 内部バッファの先頭 const ポインタ。
     */
    const T* Data() const noexcept { return m_Data; }

    /**
     * i 番目の要素への参照を返す。
     *
     * @details 範囲外は ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i 要素インデックス。
     * @return i 番目の要素への参照。
     */
    T&       operator[](usize i)       noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    /**
     * i 番目の要素への const 参照を返す。
     *
     * @details 範囲外は ACS_ASSERT で検出する (Debug ビルドのみ)。
     * @param i 要素インデックス。
     * @return i 番目の要素への const 参照。
     */
    const T& operator[](usize i) const noexcept { ACS_ASSERT(i < m_Size); return m_Data[i]; }

    /**
     * 先頭要素への参照を返す。
     *
     * @details 空配列での呼び出しは ACS_ASSERT で検出する。
     * @return 先頭要素への参照。
     */
    T&       Front()       noexcept { ACS_ASSERT(m_Size > 0); return m_Data[0]; }

    /**
     * 先頭要素への const 参照を返す。
     *
     * @details 空配列での呼び出しは ACS_ASSERT で検出する。
     * @return 先頭要素への const 参照。
     */
    const T& Front() const noexcept { ACS_ASSERT(m_Size > 0); return m_Data[0]; }

    /**
     * 末尾要素への参照を返す。
     *
     * @details 空配列での呼び出しは ACS_ASSERT で検出する。
     * @return 末尾要素への参照。
     */
    T&       Back ()       noexcept { ACS_ASSERT(m_Size > 0); return m_Data[m_Size - 1]; }

    /**
     * 末尾要素への const 参照を返す。
     *
     * @details 空配列での呼び出しは ACS_ASSERT で検出する。
     * @return 末尾要素への const 参照。
     */
    const T& Back () const noexcept { ACS_ASSERT(m_Size > 0); return m_Data[m_Size - 1]; }

    /**
     * 先頭イテレータを返す。
     *
     * @return 先頭要素へのポインタ。
     */
    T*       begin()       noexcept { return m_Data; }

    /**
     * 終端イテレータを返す。
     *
     * @return 末尾の次を指すポインタ。
     */
    T*       end()         noexcept { return m_Data + m_Size; }

    /**
     * 先頭 const イテレータを返す。
     *
     * @return 先頭要素への const ポインタ。
     */
    const T* begin() const noexcept { return m_Data; }

    /**
     * 終端 const イテレータを返す。
     *
     * @return 末尾の次を指す const ポインタ。
     */
    const T* end()   const noexcept { return m_Data + m_Size; }

    /**
     * 全要素を指す TSpan ビューを返す。
     *
     * @return 内部バッファ全体を指す TSpan。
     */
    TSpan<T>       AsSpan()       noexcept { return TSpan<T>(m_Data, m_Size); }

    /**
     * 全要素を指す読み取り専用 TSpan ビューを返す。
     *
     * @return 内部バッファ全体を指す const 要素の TSpan。
     */
    TSpan<const T> AsSpan() const noexcept { return TSpan<const T>(m_Data, m_Size); }

    /**
     * 末尾に要素をコピー追加する。
     *
     * @details 容量不足なら NextGrow で再確保する。
     * @param v 追加する値 (コピーされる)。
     */
    void PushBack(const T& v) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(v);
        ++m_Size;
    }

    /**
     * 末尾に要素をムーブ追加する。
     *
     * @details 容量不足なら NextGrow で再確保する。
     * @param v 追加する値 (ムーブされる)。
     */
    void PushBack(T&& v) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(Move(v));
        ++m_Size;
    }

    /**
     * 末尾に要素をその場構築する。
     *
     * @details 容量不足なら NextGrow で再確保し、引数を T のコンストラクタへ転送する。
     * @tparam Args T のコンストラクタ引数型。
     * @param args T のコンストラクタへ転送する引数。
     * @return 構築した末尾要素への参照。
     */
    template<typename... Args>
    T& EmplaceBack(Args&&... args) noexcept {
        if (m_Size == m_Capacity) Grow(NextGrow(m_Size + 1));
        ::new (&m_Data[m_Size]) T(Forward<Args>(args)...);
        return m_Data[m_Size++];
    }

    /**
     * 末尾要素を 1 つ削除する。
     *
     * @details 空配列での呼び出しは ACS_ASSERT で検出する。
     */
    void PopBack() noexcept {
        ACS_ASSERT(m_Size > 0);
        --m_Size;
        if constexpr (!IsTriviallyDestructibleV<T>) m_Data[m_Size].~T();
    }

    /**
     * i 番目の要素を削除する (末尾要素をその位置へムーブして縮める)。
     *
     * @details 順序は保たれないが O(1) で削除できる。範囲外は ACS_ASSERT で検出する。
     * @param i 削除する要素インデックス。
     */
    void RemoveAtSwap(usize i) noexcept {
        ACS_ASSERT(i < m_Size);
        --m_Size;
        if (i != m_Size) m_Data[i] = Move(m_Data[m_Size]);
        if constexpr (!IsTriviallyDestructibleV<T>) m_Data[m_Size].~T();
    }

    /**
     * 配列を明示的に複製する。
     *
     * @details コピーが高コストなため名前付きで意識させる。trivial 型はバルクコピー、
     * そうでなければ要素ごとにコピー代入する。同じアロケータを使う。
     * @return 同一内容の新しい配列。
     */
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

    /**
     * この配列が使うアロケータを返す。
     *
     * @return 確保に使っている FAllocator へのポインタ。
     */
    FAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    /**
     * 必要数を満たす次の容量を計算する (約 1.5 倍ずつ、最小 8)。
     *
     * @param required 最低限必要な要素数。
     * @return required 以上の新容量。
     */
    static usize NextGrow(usize required) noexcept {
        usize n = 8;
        while (n < required) n += n / 2 + 1;
        return n;
    }

    /**
     * 新しい容量で再確保し、既存要素をムーブ移送する。
     *
     * @details sizeof(T)*new_capacity の乗算オーバーフローと確保失敗を ACS_ASSERTF で
     * 検出する。trivial 型はバルクコピー、そうでなければムーブ構築 + 旧要素の破棄を行う。
     * @param new_capacity 新しい容量。
     */
    void Grow(usize new_capacity) noexcept {
        // sizeof(T) * new_capacity の乗算ラップを防ぐ（過小確保 → バッファ外書き込み防止）
        ACS_ASSERTF(new_capacity == 0 || new_capacity <= (~usize(0)) / sizeof(T),
                    "TArray::Grow: size overflow (cap=%zu)", new_capacity);
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

    /** 内部バッファを解放してポインタ・容量をリセットする。 */
    void Free() noexcept {
        if (m_Data) { m_Alloc->Free(m_Data); m_Data = nullptr; m_Capacity = 0; }
    }

    /** 要素を格納する連続バッファの先頭ポインタ。 */
    T*         m_Data     = nullptr;

    /** 現在の要素数。 */
    usize      m_Size     = 0;

    /** 確保済み容量。 */
    usize      m_Capacity = 0;

    /** 確保に使うアロケータ。 */
    FAllocator* m_Alloc    = nullptr;
};

} // namespace acs
