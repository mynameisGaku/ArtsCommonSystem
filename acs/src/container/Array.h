// SPDX-License-Identifier: Apache-2.0
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
 * 連続バッファを IAllocator から確保し、容量不足時に約 1.5 倍ずつ Grow する。
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
    explicit TArray(IAllocator& a) noexcept : m_Alloc(&a) {}

    /**
     * 初期容量を予約して空の配列を構築する。
     *
     * @param initial_capacity 事前に予約する容量。
     * @param a 確保に使うアロケータ (既定は DefaultAllocator)。
     */
    TArray(usize initial_capacity, IAllocator& a = DefaultAllocator()) noexcept : m_Alloc(&a) {
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
        ACS_CHECKF(TryReserve(new_capacity), "TArray::Reserve failed (cap=%zu, T=%zu)",
                   new_capacity, sizeof(T));
    }

    /**
     * 容量予約を試み、失敗時も元の配列を変更しない。
     *
     * @param NewCapacity 確保する最小容量。
     * @return 予約済みまたは予約成功なら true、サイズoverflow/OOMなら false。
     */
    bool TryReserve(usize NewCapacity) noexcept
    {
        if (NewCapacity <= m_Capacity) return true;
        return Grow(NewCapacity);
    }

    /**
     * サイズを変更する。
     *
     * @details 増やす場合は新規要素をデフォルト構築 (trivial なら 0 埋め)、減らす
     * 場合は超過要素をデストラクトする。
     * @param new_size 変更後の要素数。
     */
    void Resize(usize new_size) noexcept {
        ACS_CHECKF(TryResize(new_size), "TArray::Resize failed (size=%zu, T=%zu)",
                   new_size, sizeof(T));
    }

    /**
     * サイズ変更を試み、拡張用確保に失敗した場合は元の配列を変更しない。
     *
     * @param NewSize 変更後の要素数。
     * @return 成功なら true、サイズoverflow/OOMなら false。
     */
    bool TryResize(usize NewSize) noexcept
    {
        if (NewSize > m_Capacity) {
            /** NewSize を収容する拡張後の容量。 */
            usize NewCapacity = 0u;
            if (!TryCalculateNextGrowth(m_Capacity, NewSize, NewCapacity) || !Grow(NewCapacity)) {
                return false;
            }
        }
        if (NewSize > m_Size) {
            if constexpr (IsTriviallyConstructibleV<T>) {
                MemSet(static_cast<void*>(m_Data + m_Size), 0, sizeof(T) * (NewSize - m_Size));
            } else {
                for (usize i = m_Size; i < NewSize; ++i) ::new (&m_Data[i]) T();
            }
        } else if (NewSize < m_Size) {
            if constexpr (!IsTriviallyDestructibleV<T>) {
                for (usize i = NewSize; i < m_Size; ++i) m_Data[i].~T();
            }
        }
        m_Size = NewSize;
        return true;
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
        (void)Grow(m_Size);
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
    T*       end()         noexcept { return m_Size == 0u ? m_Data : m_Data + m_Size; }

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
    const T* end()   const noexcept { return m_Size == 0u ? m_Data : m_Data + m_Size; }

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
        ACS_CHECKF(TryPushBack(v), "TArray::PushBack failed (size=%zu, T=%zu)", m_Size, sizeof(T));
    }

    /**
     * コピー追加を試み、失敗時は配列を変更しない。
     *
     * @details 容量成長時も、追加元が同じ配列内の要素なら旧領域を保持したまま
     * 新しい末尾を先に構築するため、自己参照を安全に扱える。
     */
    bool TryPushBack(const T& Value) noexcept
    {
        if constexpr (IsTriviallyCopyableV<T> && IsCopyConstructibleV<T>) {
            return TryPushBackTrivial(Value);
        }
        return TryEmplaceBack(Value) != nullptr;
    }

    /**
     * 末尾に要素をムーブ追加する。
     *
     * @details 容量不足なら NextGrow で再確保する。
     * @param v 追加する値 (ムーブされる)。
     */
    void PushBack(T&& v) noexcept {
        ACS_CHECKF(TryPushBack(Move(v)), "TArray::PushBack failed (size=%zu, T=%zu)", m_Size, sizeof(T));
    }

    /**
     * ムーブ追加を試み、失敗時は配列と引数を変更しない。
     *
     * @details 容量成長時に追加元が同じ配列内の要素でも、旧要素から新しい末尾へ
     * 先にムーブしてから既存要素を移送する。
     */
    bool TryPushBack(T&& Value) noexcept
    {
        if constexpr (IsTriviallyCopyableV<T> && IsCopyConstructibleV<T>) {
            // move ではなく退避コピーする。確保失敗時は配列と右辺値引数の
            // どちらも変更しない。
            return TryPushBackTrivial(Value);
        }
        return TryEmplaceBack(Move(Value)) != nullptr;
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
        T* const Element = TryEmplaceBack(Forward<Args>(args)...);
        ACS_CHECKF(Element != nullptr, "TArray::EmplaceBack failed (size=%zu, T=%zu)", m_Size, sizeof(T));
        return *Element;
    }

    /**
     * 末尾へのその場構築を試み、失敗時は nullptr を返す。
     *
     * @details 容量内なら直接構築する。容量成長時は新領域を確保した後、旧領域が
     * 生存している間に新しい末尾を構築してから既存要素を移送する。これにより、
     * コンストラクタが同じ配列内の要素やその一部から値を読み取る間は参照を
     * 無効化しない。
     */
    template<typename... Args>
    T* TryEmplaceBack(Args&&... Arguments) noexcept
    {
        if (m_Size < m_Capacity) {
            ::new (&m_Data[m_Size]) T(Forward<Args>(Arguments)...);
            return &m_Data[m_Size++];
        }
        if (m_Size == ~usize(0)) return nullptr;

        /** 追加要素を収容する拡張後の容量。 */
        usize NewCapacity = 0u;
        if (!TryCalculateNextGrowth(m_Capacity, m_Size + 1u, NewCapacity)) {
            return nullptr;
        }
        return TryGrowAndEmplaceBack(NewCapacity, Forward<Args>(Arguments)...);
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
     * i 番目の要素を削除する (後続を前へ詰めて順序を保つ O(n))。
     *
     * @details 順序が要らないなら RemoveAtSwap (O(1)) を使う。範囲外は ACS_ASSERT で検出する。
     * @param i 削除する要素インデックス。
     */
    void RemoveAt(usize i) noexcept {
        ACS_ASSERT(i < m_Size);
        for (usize k = i + 1; k < m_Size; ++k) m_Data[k - 1] = Move(m_Data[k]);
        --m_Size;
        if constexpr (!IsTriviallyDestructibleV<T>) m_Data[m_Size].~T();
    }

    /** IndexOf / IndexOfIf が「見つからない」を表す番兵値。 */
    static constexpr usize kNpos = static_cast<usize>(-1);

    /**
     * value と == で一致する最初の要素のインデックスを返す (線形探索)。
     *
     * @param value 探す値。
     * @return 最初に一致したインデックス (無ければ kNpos)。
     */
    usize IndexOf(const T& value) const noexcept {
        for (usize i = 0; i < m_Size; ++i) {
            if (m_Data[i] == value) return i;
        }
        return kNpos;
    }

    /**
     * pred(要素) が true になる最初の要素のインデックスを返す (線形探索)。
     *
     * @tparam Pred bool(const T&) 相当の呼び出し可能型。
     * @param pred 判定述語 (値で受け取る)。
     * @return 最初に一致したインデックス (無ければ kNpos)。
     */
    template<typename Pred>
    usize IndexOfIf(Pred pred) const noexcept {
        for (usize i = 0; i < m_Size; ++i) {
            if (pred(m_Data[i])) return i;
        }
        return kNpos;
    }

    /**
     * value と == で一致する最初の要素へのポインタを返す (線形探索)。
     *
     * @details 返したポインタは再確保・削除で無効化され得るため、構造変更をまたいで保持しない。
     * @param value 探す値。
     * @return 最初に一致した要素 (無ければ nullptr)。
     */
    T* Find(const T& value) noexcept {
        const usize i = IndexOf(value);
        return i == kNpos ? nullptr : &m_Data[i];
    }

    /**
     * value と == で一致する最初の要素への const ポインタを返す (線形探索)。
     *
     * @param value 探す値。
     * @return 最初に一致した要素 (無ければ nullptr)。
     */
    const T* Find(const T& value) const noexcept {
        const usize i = IndexOf(value);
        return i == kNpos ? nullptr : &m_Data[i];
    }

    /**
     * pred(要素) が true になる最初の要素へのポインタを返す (線形探索)。
     *
     * @details 返したポインタは再確保・削除で無効化され得るため、構造変更をまたいで保持しない。
     * @tparam Pred bool(const T&) 相当の呼び出し可能型。
     * @param pred 判定述語 (値で受け取る)。
     * @return 最初に一致した要素 (無ければ nullptr)。
     */
    template<typename Pred>
    T* FindIf(Pred pred) noexcept {
        const usize i = IndexOfIf(pred);
        return i == kNpos ? nullptr : &m_Data[i];
    }

    /**
     * pred(要素) が true になる最初の要素への const ポインタを返す (線形探索)。
     *
     * @tparam Pred bool(const T&) 相当の呼び出し可能型。
     * @param pred 判定述語 (値で受け取る)。
     * @return 最初に一致した要素 (無ければ nullptr)。
     */
    template<typename Pred>
    const T* FindIf(Pred pred) const noexcept {
        const usize i = IndexOfIf(pred);
        return i == kNpos ? nullptr : &m_Data[i];
    }

    /**
     * value と == で一致する要素が存在するかを返す (線形探索)。
     *
     * @param value 探す値。
     * @return 1 つでも一致すれば true。
     */
    bool Contains(const T& value) const noexcept { return IndexOf(value) != kNpos; }

    /**
     * value と == で一致する最初の要素を、順序を保って削除する。
     *
     * @details 削除時の確保は行わない。削除位置より後の要素は前へ詰めるため、
     * その位置以降を指すポインタと参照は無効になる。
     * @param value 削除する値。配列内の要素自身を渡してもよい。
     * @return 削除できたら true。見つからなければ配列を変えず false。
     */
    bool Remove(const T& value) noexcept {
        // 最初に一致した削除位置。
        const usize i = IndexOf(value);
        if (i == kNpos) return false;
        RemoveAt(i);
        return true;
    }

    /**
     * value と == で一致する最初の要素を swap-remove する (順序は保たれない)。
     *
     * @param value 削除する値。
     * @return 削除できたら true (見つからなければ false)。
     */
    bool RemoveFirstSwap(const T& value) noexcept {
        const usize i = IndexOf(value);
        if (i == kNpos) return false;
        RemoveAtSwap(i);
        return true;
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
     * @return 確保に使っている IAllocator へのポインタ。
     */
    IAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    /**
     * 必要数を満たす次の容量を計算する (約 1.5 倍ずつ、最小 8)。
     *
     * @param required 最低限必要な要素数。
     * @return required 以上の新容量。
     */
    static bool TryCalculateNextGrowth(usize CurrentCapacity, usize Required, usize& Capacity) noexcept
    {
        Capacity = CurrentCapacity < 8u ? 8u : CurrentCapacity;
        while (Capacity < Required) {
            const usize Increment = Capacity / 2u + 1u;
            if (Capacity > (~usize(0)) - Increment) {
                Capacity = Required;
                break;
            }
            Capacity += Increment;
        }
        return Capacity >= Required;
    }

    /**
     * 自己参照と失敗時 rollback を保ったまま trivial-copy 値を追加する。
     * Realloc が配列領域を移動しても、退避コピーは有効なまま残る。
     */
    bool TryPushBackTrivial(const T& Value) noexcept
    {
        if (m_Size < m_Capacity) {
            ::new (&m_Data[m_Size]) T(Value);
            ++m_Size;
            return true;
        }
        if (m_Size == ~usize(0)) return false;

        /** 再確保で追加元が無効化されないよう保持する値。 */
        T Staged(Value);

        /** 追加要素を収容する拡張後の容量。 */
        usize NewCapacity = 0u;
        if (!TryCalculateNextGrowth(m_Capacity, m_Size + 1u, NewCapacity) || !Grow(NewCapacity)) {
            return false;
        }
        ::new (&m_Data[m_Size]) T(Staged);
        ++m_Size;
        return true;
    }

    /**
     * 新領域へ末尾を構築してから既存要素を移送し、成長を確定する。
     *
     * @details 確保が成功するまでは引数にも配列にも触れない。末尾の構築時点では
     * 旧領域が生存しているため、コンストラクタは旧要素やそのメンバから値を
     * 読み取れる。非自己参照の場合も追加の退避や探索を行わない。
     *
     * @tparam Args T のコンストラクタ引数型。
     * @param NewCapacity 確保する新しい容量。
     * @param Arguments T のコンストラクタへ転送する引数。
     * @return 構築した末尾要素。容量オーバーフローまたは確保失敗なら nullptr。
     */
    template<typename... Args>
    T* TryGrowAndEmplaceBack(
        usize NewCapacity,
        Args&&... Arguments) noexcept
    {
        if (NewCapacity <= m_Size ||
            NewCapacity > (~usize(0)) / sizeof(T)) {
            return nullptr;
        }

        T* const NewData = static_cast<T*>(
            m_Alloc->Alloc(
                sizeof(T) * NewCapacity,
                alignof(T),
                FSourceLoc::Current()));
        if (!NewData) return nullptr;

        // 引数が旧領域を参照できるうちに、追加要素を先に完成させる。
        ::new (&NewData[m_Size]) T(Forward<Args>(Arguments)...);

        if (m_Data) {
            if constexpr (IsTriviallyRelocatableV<T>) {
                MemCopy(NewData, m_Data, sizeof(T) * m_Size);
            } else {
                for (usize i = 0; i < m_Size; ++i) {
                    ::new (&NewData[i]) T(Move(m_Data[i]));
                    if constexpr (!IsTriviallyDestructibleV<T>) {
                        m_Data[i].~T();
                    }
                }
            }
            m_Alloc->Free(m_Data);
        }

        m_Data = NewData;
        m_Capacity = NewCapacity;
        return &m_Data[m_Size++];
    }

    /**
     * 新しい容量で再確保し、既存要素をムーブ移送する。
     *
     * @details sizeof(T)*new_capacity の乗算オーバーフローと確保失敗を ACS_ASSERTF で
     * 検出する。trivial 型はバルクコピー、そうでなければムーブ構築 + 旧要素の破棄を行う。
     * @param new_capacity 新しい容量。
     */
    bool Grow(usize new_capacity) noexcept {
        if (new_capacity == m_Capacity) return true;
        if (new_capacity < m_Size) return false;
        if (new_capacity == 0u) {
            Free();
            return true;
        }
        // sizeof(T) * new_capacity の乗算ラップを防ぐ（過小確保 → バッファ外書き込み防止）
        if (new_capacity > (~usize(0)) / sizeof(T)) return false;
        /** 新しい確保領域のバイト数。 */
        const usize NewBytes = sizeof(T) * new_capacity;
        if constexpr (IsTriviallyRelocatableV<T>) {
            // 旧確保領域の全体が使用中か、生存要素数まで縮小する場合だけ
            // Realloc を使う。fallback 実装が未使用容量までコピーすることを
            // 防ぐ。
            if (m_Data != nullptr && (m_Size == m_Capacity || new_capacity <= m_Capacity)) {
                /** 再配置後の領域。失敗時は旧領域を維持する。 */
                T* const Relocated = static_cast<T*>(m_Alloc->Realloc(m_Data, sizeof(T) * m_Capacity, NewBytes, alignof(T), FSourceLoc::Current()));
                if (!Relocated) return false;
                m_Data = Relocated;
                m_Capacity = new_capacity;
                return true;
            }
        }

        /** byte 再配置または要素移動の出力領域。 */
        T* new_data = static_cast<T*>(m_Alloc->Alloc(NewBytes, alignof(T), FSourceLoc::Current()));
        if (!new_data) return false;
        if (m_Data) {
            if constexpr (IsTriviallyRelocatableV<T>) {
                MemCopy(new_data, m_Data, sizeof(T) * m_Size);
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
        return true;
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
    IAllocator* m_Alloc    = nullptr;
};

} // namespace acs
