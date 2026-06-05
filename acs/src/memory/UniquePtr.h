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
     * @param p 所有する対象 (この後の解放責任を引き受ける)。
     * @param a 解放に使うアロケータ (nullptr なら DefaultAllocator)。
     */
    explicit TUniquePtr(T* p, FAllocator* a = nullptr) noexcept
        : m_Ptr(p), m_Alloc(a ? a : &DefaultAllocator()) {}

    /** コピー禁止 (対象を単独所有するため)。 */
    TUniquePtr(const TUniquePtr&) = delete;

    /** コピー代入も禁止。 */
    TUniquePtr& operator=(const TUniquePtr&) = delete;

    /**
     * ムーブ構築。所有権を o から奪い、o を空にする。
     *
     * @param o 所有権の移動元 (この後は空になる)。
     */
    TUniquePtr(TUniquePtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Alloc(o.m_Alloc) {
        o.m_Ptr = nullptr;
    }

    /**
     * 別要素型 U からの変換ムーブ構築 (基底クラスへのアップキャスト等)。
     *
     * @details o の所有権を Release で奪い、アロケータも引き継ぐ。
     * @tparam U 移動元の要素型 (T* へ変換可能であること)。
     * @param o 所有権の移動元 (この後は空になる)。
     */
    template<typename U>
    TUniquePtr(TUniquePtr<U>&& o) noexcept : m_Ptr(o.Release()), m_Alloc(o.GetAllocator()) {}

    /**
     * ムーブ代入。現在の対象を破棄してから o の所有権を奪う。
     *
     * @param o 所有権の移動元 (この後は空になる)。
     * @return 自身への参照。
     */
    TUniquePtr& operator=(TUniquePtr&& o) noexcept {
        if (this == &o) return *this;
        Reset();
        m_Ptr = o.m_Ptr;
        m_Alloc = o.m_Alloc;
        o.m_Ptr = nullptr;
        return *this;
    }

    /** 破棄時に保持中の対象を解放する。 */
    ~TUniquePtr() noexcept { Reset(); }

    /**
     * 所有している生ポインタを返す (所有権は手放さない)。
     *
     * @details std::unique_ptr<T>::get() と同じ意味論。TUniquePtr の const 性と T の const 性は独立。
     * @return 保持中の生ポインタ (空なら nullptr)。
     */
    T* Get() const noexcept { return m_Ptr; }

    /**
     * 対象への参照を返す。
     *
     * @return 保持中の対象への参照 (空のとき呼ぶのは未定義)。
     */
    T& operator*()  const noexcept { return *m_Ptr; }

    /**
     * 対象のメンバへアクセスするためのポインタを返す。
     *
     * @return 保持中の生ポインタ。
     */
    T* operator->() const noexcept { return m_Ptr; }

    /**
     * 対象を所有しているかを返す。
     *
     * @return 非 null を所有していれば true。
     */
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }

    /**
     * 所有権を放棄して生ポインタを返す。
     *
     * @details 解放は行わない。呼び出し側が以後の解放責任を持つ。自身は空になる。
     * @return それまで保持していた生ポインタ。
     */
    T* Release() noexcept {
        T* p = m_Ptr;
        m_Ptr = nullptr;
        return p;
    }

    /**
     * 既存の対象を破棄し、新しい対象を保持する (または空にする)。
     *
     * @param p 新たに保持する対象 (既定 nullptr で空にする)。
     */
    void Reset(T* p = nullptr) noexcept {
        if (m_Ptr) Delete(*m_Alloc, m_Ptr);
        m_Ptr = p;
    }

    /**
     * 解放に使うアロケータを返す。
     *
     * @return 保持中のアロケータ。
     */
    FAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    /** 所有している対象 (空なら nullptr)。 */
    T*         m_Ptr   = nullptr;

    /** 対象の解放に使うアロケータ。 */
    FAllocator* m_Alloc = nullptr;
};

/**
 * デフォルトアロケータで T を構築し TUniquePtr に包んで返す。
 *
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を所有する TUniquePtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUnique(Args&&... args) noexcept {
    FAllocator& a = DefaultAllocator();
    T* const p = New<T>(a, Forward<Args>(args)...);
    return TUniquePtr<T>(p, &a);
}

/**
 * 指定アロケータで T を構築し TUniquePtr に包んで返す。
 *
 * @tparam T 構築するオブジェクト型。
 * @tparam Args T のコンストラクタ引数型。
 * @param a 確保・解放に使うアロケータ。
 * @param args T のコンストラクタへ転送する引数。
 * @return 構築した対象を所有する TUniquePtr (確保失敗時は空)。
 */
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUniqueIn(FAllocator& a, Args&&... args) noexcept {
    T* const p = New<T>(a, Forward<Args>(args)...);
    return TUniquePtr<T>(p, &a);
}

} // namespace acs
