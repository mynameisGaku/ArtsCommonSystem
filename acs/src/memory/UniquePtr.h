// SPDX-License-Identifier: Apache-2.0
// ACS Memory — TUniquePtr<T>（std::unique_ptr 代替）
//
// 単独所有のスマートポインタ。ムーブのみ可、コピー不可。
// 破棄時に FAllocator::Free を呼んで自動解放する。
//
// 例:
//   auto p = MakeUnique<Mesh>(args...);
//   p->Render();
//   // スコープ脱出で自動的に解放
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"

namespace acs {

/**
 * 対象を単独所有するスマートポインタ (std::unique_ptr 代替)。
 *
 * @details
 * ムーブのみ可・コピー不可で、破棄時に保持アロケータの Delete (デストラクタ + Free) を
 * 呼んで自動解放する。生成は MakeUnique / MakeUniqueIn が標準パターン。
 * @tparam T 所有する対象の型。
 */
template<typename T>
class TUniquePtr {
public:
    /** 空の (何も所有しない) スマートポインタを構築する。 */
    TUniquePtr() noexcept = default;

    /**
     * 既存の生ポインタから構築し所有権を奪う。
     *
     * @param Pointer 所有する対象 (この後の解放責任を引き受ける)。
     * @param Allocator 解放に使うアロケータ (nullptr なら DefaultAllocator)。
     */
    explicit TUniquePtr(T* Pointer, FAllocator* Allocator = nullptr) noexcept
        : m_Ptr(Pointer), m_Alloc(Allocator ? Allocator : &DefaultAllocator())
    {
    }

    /** コピー禁止 (対象を単独所有するため)。 */
    TUniquePtr(const TUniquePtr&) = delete;

    /** コピー代入も禁止。 */
    TUniquePtr& operator=(const TUniquePtr&) = delete;

    /**
     * ムーブ構築。所有権を Other から奪い、Other を空にする。
     *
     * @param Other 所有権の移動元 (この後は空になる)。
     */
    TUniquePtr(TUniquePtr&& Other) noexcept : m_Ptr(Other.m_Ptr), m_Alloc(Other.m_Alloc)
    {
        Other.m_Ptr = nullptr;
    }

    /**
     * 別要素型 U からの変換ムーブ構築 (基底クラスへのアップキャスト等)。
     *
     * @details Other の所有権を Release で奪い、アロケータも引き継ぐ。
     * @tparam U 移動元の要素型 (T* へ変換可能であること)。
     * @param Other 所有権の移動元 (この後は空になる)。
     */
    template<typename U>
    TUniquePtr(TUniquePtr<U>&& Other) noexcept : m_Ptr(Other.Release()), m_Alloc(Other.GetAllocator())
    {
    }

    /**
     * ムーブ代入。現在の対象を破棄してから Other の所有権を奪う。
     *
     * @param Other 所有権の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TUniquePtr& operator=(TUniquePtr&& Other) noexcept
    {
        if (this == &Other) return *this;
        Reset();
        m_Ptr = Other.m_Ptr;
        m_Alloc = Other.m_Alloc;
        Other.m_Ptr = nullptr;
        return *this;
    }

    /** 破棄時に保持中の対象を解放する。 */
    ~TUniquePtr() noexcept
    {
        Reset();
    }

    /**
     * 所有している生ポインタを返す (所有権は手放さない)。
     *
     * @details std::unique_ptr<T>::get() と同じ意味論。TUniquePtr の const 性と T の const 性は独立。
     * @return 保持中の生ポインタ (空なら nullptr)。
     */
    T* Get() const noexcept
    {
        return m_Ptr;
    }

    /**
     * 対象への参照を返す。
     *
     * @return 保持中の対象への参照 (空のとき呼ぶのは未定義)。
     */
    T& operator*() const noexcept
    {
        return *m_Ptr;
    }

    /**
     * 対象のメンバへアクセスするためのポインタを返す。
     *
     * @return 保持中の生ポインタ。
     */
    T* operator->() const noexcept
    {
        return m_Ptr;
    }

    /**
     * 対象を所有しているかを返す。
     *
     * @return 非 null を所有していれば true。
     */
    explicit operator bool() const noexcept
    {
        return m_Ptr != nullptr;
    }

    /**
     * 所有権を放棄して生ポインタを返す。
     *
     * @details 解放は行わない。呼び出し側が以後の解放責任を持つ。自身は空になる。
     * @return それまで保持していた生ポインタ。
     */
    T* Release() noexcept
    {
        T* Pointer = m_Ptr;
        m_Ptr = nullptr;
        return Pointer;
    }

    /**
     * 既存の対象を破棄し、新しい対象を保持する (または空にする)。
     *
     * @param Pointer 新たに保持する対象 (既定 nullptr で空にする)。
     * @param Allocator Pointer の解放に使うアロケータ。省略時は現在の保持先、空なら DefaultAllocator。
     */
    void Reset(T* Pointer = nullptr, FAllocator* Allocator = nullptr) noexcept
    {
        // 同じポインタを渡した場合に、解放済みポインタを再保持しない。
        if (Pointer == m_Ptr) {
            if (Pointer == nullptr && Allocator != nullptr) m_Alloc = Allocator;
            return;
        }

        if (m_Ptr) {
            ACS_ASSERT(m_Alloc != nullptr);
            Delete(*m_Alloc, m_Ptr);
        }

        m_Ptr = Pointer;
        if (Pointer != nullptr) {
            // 既定構築した空ポインタから Reset(raw) した場合も、解放元を必ず記録する。
            // 別アロケータ由来の raw pointer は第2引数で明示する。
            m_Alloc = Allocator ? Allocator : (m_Alloc ? m_Alloc : &DefaultAllocator());
        } else if (Allocator != nullptr) {
            m_Alloc = Allocator;
        }
    }

    /**
     * 解放に使うアロケータを返す。
     *
     * @return 保持中のアロケータ。
     */
    FAllocator* GetAllocator() const noexcept
    {
        return m_Alloc;
    }

private:
    /** 所有している対象 (空なら nullptr)。 */
    T* m_Ptr = nullptr;

    /** 対象の解放に使うアロケータ。 */
    FAllocator* m_Alloc = nullptr;
};

/**
 * デフォルトアロケータで T を構築し TUniquePtr に包んで返す。
 *
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param Arguments T のコンストラクタへ転送する引数。
 * @return 構築した対象を所有する TUniquePtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUnique(Args&&... Arguments) noexcept
{
    FAllocator& Allocator = DefaultAllocator();
    T* const Pointer = New<T>(Allocator, Forward<Args>(Arguments)...);
    return TUniquePtr<T>(Pointer, &Allocator);
}

/**
 * 指定アロケータで T を構築し TUniquePtr に包んで返す。
 *
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param Allocator 確保・解放に使うアロケータ。
 * @param Arguments T のコンストラクタへ転送する引数。
 * @return 構築した対象を所有する TUniquePtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUniqueIn(FAllocator& Allocator, Args&&... Arguments) noexcept
{
    T* const Pointer = New<T>(Allocator, Forward<Args>(Arguments)...);
    return TUniquePtr<T>(Pointer, &Allocator);
}

} // namespace acs
