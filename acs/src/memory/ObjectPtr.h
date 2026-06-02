// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FObject + TObjectPtr / TWeakObjectPtr / TStrongObjectPtr
//   （UE の UObject + TObjectPtr/TWeakObjectPtr/TStrongObjectPtr に相当）
// -----------------------------------------------------------------------------
// TSharedPtr/TWeakPtr との違い:
//   ・FObject は「自分の制御ブロックへの逆ポインタ」を内部に持つ。そのため
//     生ポインタ (T*) からでも強参照/弱参照を作れる（UE の TWeakObjectPtr の肝）。
//   ・つまり「AActor* を持っていて、そこから弱参照を作る」が書ける。
//
// 使い分け:
//   ・任意の型を共有したい           → TSharedPtr / TWeakPtr（SharedPtr.h）
//   ・エンジンの「オブジェクト」を扱う → FObject 継承 + TObjectPtr / TWeakObjectPtr
//
// 例:
//   class FEnemy : public FObject { public: void Hit(); };
//   TObjectPtr<FEnemy> e = NewObject<FEnemy>();    // 強参照（生かし続ける）
//   TWeakObjectPtr<FEnemy> w = e;                  // 弱参照（生死を監視するだけ）
//   e.Reset();                                     // 強参照ゼロ → FEnemy 破棄
//   if (w.IsValid()) w.Get()->Hit();               // → false。破棄済なので呼ばれない
// =============================================================================
#pragma once

#include "memory/SharedPtr.h"   // sp_detail::ControlBlock / InlineBlock を共有

namespace acs {

template<typename T> class TObjectPtr;
template<typename T> class TWeakObjectPtr;

// =============================================================================
// FObject — 参照カウント管理されるオブジェクトの基底
//   NewObject<T>() で生成すると制御ブロックが割り当てられ、その逆ポインタを保持する。
// =============================================================================
class FObject {
public:
    virtual ~FObject() noexcept = default;

protected:
    FObject() noexcept = default;
    FObject(const FObject&) noexcept {}                          // 逆ポインタはコピーしない
    FObject& operator=(const FObject&) noexcept { return *this; }

private:
    sp_detail::ControlBlock* m_Cb = nullptr;                     // 自分の制御ブロック

    template<typename U> friend class TObjectPtr;
    template<typename U> friend class TWeakObjectPtr;
    template<typename U, typename... A> friend TObjectPtr<U> NewObjectIn(FAllocator&, A&&...) noexcept;
};

// =============================================================================
// TObjectPtr<T> — オブジェクトへの強参照（生かし続ける所有ポインタ）
// =============================================================================
template<typename T>
class TObjectPtr {
public:
    using ElementType = T;

    TObjectPtr() noexcept = default;
    TObjectPtr(decltype(nullptr)) noexcept {}

    // 生ポインタから。NewObject 経由で作られた対象であること（さもなくば空になる）。
    explicit TObjectPtr(T* obj) noexcept {
        static_assert(IsBaseOfV<FObject, T>, "TObjectPtr<T>: T は FObject を継承していること");
        if (obj) {
            sp_detail::ControlBlock* cb = static_cast<FObject*>(obj)->m_Cb;
            if (cb) { m_Ptr = obj; m_Cb = cb; m_Cb->AddStrong(); }
        }
    }

    TObjectPtr(const TObjectPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddStrong(); }
    TObjectPtr(TObjectPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }
    template<typename U> TObjectPtr(const TObjectPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddStrong(); }
    template<typename U> TObjectPtr(TObjectPtr<U>&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }

    TObjectPtr& operator=(const TObjectPtr& o) noexcept { TObjectPtr(o).Swap(*this); return *this; }
    TObjectPtr& operator=(TObjectPtr&& o)      noexcept { TObjectPtr(static_cast<TObjectPtr&&>(o)).Swap(*this); return *this; }
    TObjectPtr& operator=(decltype(nullptr))   noexcept { Reset(); return *this; }

    ~TObjectPtr() noexcept { if (m_Cb) m_Cb->ReleaseStrong(); }

    T*  Get()        const noexcept { return m_Ptr; }
    T&  operator*()  const noexcept { return *m_Ptr; }
    T*  operator->() const noexcept { return m_Ptr; }
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }
    bool IsValid() const noexcept { return m_Ptr != nullptr; }
    u32  UseCount() const noexcept { return m_Cb ? m_Cb->StrongCount() : 0; }

    void Reset() noexcept { TObjectPtr().Swap(*this); }
    void Swap(TObjectPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    T*                       m_Ptr = nullptr;
    sp_detail::ControlBlock* m_Cb  = nullptr;

    TObjectPtr(T* p, sp_detail::ControlBlock* cb) noexcept : m_Ptr(p), m_Cb(cb) {}  // 採用（+1 済）

    template<typename U> friend class TObjectPtr;
    template<typename U> friend class TWeakObjectPtr;
    template<typename U, typename... A> friend TObjectPtr<U> NewObjectIn(FAllocator&, A&&...) noexcept;
};

// 「強参照である」ことを明示したいとき用の別名（UE の TStrongObjectPtr に相当）
template<typename T> using TStrongObjectPtr = TObjectPtr<T>;

// =============================================================================
// TWeakObjectPtr<T> — オブジェクトへの弱参照（生死を監視、生存は延ばさない）
// =============================================================================
template<typename T>
class TWeakObjectPtr {
public:
    TWeakObjectPtr() noexcept = default;
    TWeakObjectPtr(decltype(nullptr)) noexcept {}

    explicit TWeakObjectPtr(T* obj) noexcept {
        static_assert(IsBaseOfV<FObject, T>, "TWeakObjectPtr<T>: T は FObject を継承していること");
        if (obj) {
            sp_detail::ControlBlock* cb = static_cast<FObject*>(obj)->m_Cb;
            if (cb) { m_Ptr = obj; m_Cb = cb; m_Cb->AddWeak(); }
        }
    }
    TWeakObjectPtr(const TObjectPtr<T>& s) noexcept : m_Ptr(s.Get()), m_Cb(s.m_Cb) { if (m_Cb) m_Cb->AddWeak(); }
    TWeakObjectPtr(const TWeakObjectPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { if (m_Cb) m_Cb->AddWeak(); }
    TWeakObjectPtr(TWeakObjectPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) { o.m_Ptr = nullptr; o.m_Cb = nullptr; }

    TWeakObjectPtr& operator=(const TWeakObjectPtr& o) noexcept { TWeakObjectPtr(o).Swap(*this); return *this; }
    TWeakObjectPtr& operator=(TWeakObjectPtr&& o)      noexcept { TWeakObjectPtr(static_cast<TWeakObjectPtr&&>(o)).Swap(*this); return *this; }
    TWeakObjectPtr& operator=(const TObjectPtr<T>& s)  noexcept { TWeakObjectPtr(s).Swap(*this); return *this; }

    ~TWeakObjectPtr() noexcept { if (m_Cb) m_Cb->ReleaseWeak(); }

    // 対象がまだ生きていれば true
    bool IsValid() const noexcept { return m_Cb && m_Cb->StrongCount() > 0; }
    bool IsStale() const noexcept { return !IsValid(); }

    // 生きていれば対象ポインタ、死んでいれば nullptr（簡易アクセス。別スレッドが同時に
    // 破棄し得る状況では Pin() を使うこと）。
    T* Get() const noexcept { return IsValid() ? m_Ptr : nullptr; }

    // 生きていれば強参照を獲得して返す（破棄と競合しても安全）。死んでいれば空。
    TObjectPtr<T> Pin() const noexcept {
        if (m_Cb && m_Cb->TryAddStrong()) return TObjectPtr<T>(m_Ptr, m_Cb);
        return TObjectPtr<T>();
    }

    void Reset() noexcept { TWeakObjectPtr().Swap(*this); }
    void Swap(TWeakObjectPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    T*                       m_Ptr = nullptr;
    sp_detail::ControlBlock* m_Cb  = nullptr;

    template<typename U> friend class TWeakObjectPtr;
    template<typename U> friend class TObjectPtr;
};

// =============================================================================
// ファクトリ
// =============================================================================

// 指定アロケータでオブジェクトを生成（強参照 1 を返す）
template<typename T, typename... Args>
ACS_FORCEINLINE TObjectPtr<T> NewObjectIn(FAllocator& a, Args&&... args) noexcept {
    static_assert(IsBaseOfV<FObject, T>, "NewObject<T>: T は FObject を継承していること");
    using Block = sp_detail::InlineBlock<T>;
    void* mem = a.Alloc(sizeof(Block), alignof(Block), FSourceLoc::Current());
    if (!mem) return TObjectPtr<T>();
    auto* blk = ::new (mem) Block();
    blk->alloc       = &a;
    blk->destroy_obj = &Block::DestroyObj;
    blk->free_self   = &Block::FreeSelf;
    T* obj = ::new (blk->Ptr()) T(Forward<Args>(args)...);
    static_cast<FObject*>(obj)->m_Cb = blk;          // 逆ポインタを仕込む
    return TObjectPtr<T>(obj, blk);                  // strong=1 を採用
}

// デフォルトアロケータでオブジェクトを生成
template<typename T, typename... Args>
ACS_FORCEINLINE TObjectPtr<T> NewObject(Args&&... args) noexcept {
    return NewObjectIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

} // namespace acs
