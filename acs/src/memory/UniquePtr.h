// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — TUniquePtr<T>（std::unique_ptr 代替）
// -----------------------------------------------------------------------------
// 単独所有のスマートポインタ。ムーブのみ可、コピー不可。
// 破棄時に FAllocator::Free を呼んで自動解放する。
//
// 例:
//   auto p = MakeUnique<Mesh>(args...);
//   p->Render();
//   // スコープ脱出で自動的に解放
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"

namespace acs {

template<typename T>
class TUniquePtr {
public:
    TUniquePtr() noexcept = default;

    // 既存ポインタからの構築（所有権を奪う）
    explicit TUniquePtr(T* p, FAllocator* a = nullptr) noexcept
        : m_Ptr(p), m_Alloc(a ? a : &DefaultAllocator()) {}

    // コピー禁止
    TUniquePtr(const TUniquePtr&) = delete;
    TUniquePtr& operator=(const TUniquePtr&) = delete;

    // ムーブ: 所有権を譲渡
    TUniquePtr(TUniquePtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Alloc(o.m_Alloc) {
        o.m_Ptr = nullptr;
    }

    // U → T への変換ムーブ（基底クラスへのアップキャスト等）
    template<typename U>
    TUniquePtr(TUniquePtr<U>&& o) noexcept : m_Ptr(o.Release()), m_Alloc(o.GetAllocator()) {}

    TUniquePtr& operator=(TUniquePtr&& o) noexcept {
        if (this == &o) return *this;
        Reset();
        m_Ptr = o.m_Ptr;
        m_Alloc = o.m_Alloc;
        o.m_Ptr = nullptr;
        return *this;
    }

    ~TUniquePtr() noexcept { Reset(); }

    // ---- アクセス ----
    // std::unique_ptr<T>::get() と同じ意味論。TUniquePtr の const 性と T の const 性は独立。
    T* Get() const noexcept { return m_Ptr; }

    T& operator*()  const noexcept { return *m_Ptr; }
    T* operator->() const noexcept { return m_Ptr; }
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }

    // 所有権を放棄して生ポインタを返す（呼び出し側が解放責任を持つ）
    T* Release() noexcept {
        T* p = m_Ptr;
        m_Ptr = nullptr;
        return p;
    }

    // 既存対象を破棄して新しい対象を保持（または null にする）
    void Reset(T* p = nullptr) noexcept {
        if (m_Ptr) Delete(*m_Alloc, m_Ptr);
        m_Ptr = p;
    }

    FAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    T*         m_Ptr   = nullptr;
    FAllocator* m_Alloc = nullptr;
};

// ---- ファクトリ -----------------------------------------------------------

// デフォルトアロケータで構築
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUnique(Args&&... args) noexcept {
    FAllocator& a = DefaultAllocator();
    T* p = New<T>(a, Forward<Args>(args)...);
    return TUniquePtr<T>(p, &a);
}

// 指定アロケータで構築
template<typename T, typename... Args>
ACS_FORCEINLINE TUniquePtr<T> MakeUniqueIn(FAllocator& a, Args&&... args) noexcept {
    T* p = New<T>(a, Forward<Args>(args)...);
    return TUniquePtr<T>(p, &a);
}

} // namespace acs
