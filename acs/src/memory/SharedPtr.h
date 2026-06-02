// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — TSharedPtr<T> / TWeakPtr<T> / TSharedFromThis<T>
//   （std::shared_ptr / std::weak_ptr / enable_shared_from_this 代替）
// -----------------------------------------------------------------------------
// ・TSharedPtr<T> … 共有所有。コピーで強参照カウントを atomic 増加、破棄で減少、
//   0 で対象を破棄する。MakeShared 経由なら ControlBlock と T を 1 アロケーション
//   に同居させる（make_shared 相当）。
// ・TWeakPtr<T>   … 弱参照。対象の生存を延ばさない。Lock() で生きていれば
//   TSharedPtr を得る（循環参照を断ち切る用途）。
// ・TSharedFromThis<T> … メンバ関数内で自分自身の TSharedPtr を作りたいときの基底。
//
// 旧名 TRc / MakeRc は memory/Rc.h に互換エイリアスとして残してある（非推奨）。
//
// 例:
//   auto p = MakeShared<Mesh>(args...);   // TSharedPtr<Mesh>
//   TWeakPtr<Mesh> w = p;                  // 弱参照
//   if (auto s = w.Lock()) s->Render();    // 生きていれば使える
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "threading/Atomic.h"
#include "foundation/TypeTraits.h"

namespace acs {

template<typename T> class TSharedPtr;
template<typename T> class TWeakPtr;
template<typename T> class TSharedFromThis;

namespace sp_detail {

// 全 TSharedPtr/TWeakPtr 共通の制御ブロック。
//   strong … 強参照（TSharedPtr）の数。0 になったら T を破棄する。
//   weak   … 弱参照（TWeakPtr）の数 + 「強参照グループ全体」を表す 1。
//            0 になったら制御ブロック自体（と同居する T の領域）を解放する。
// この 2 段構えにより、T が破棄されても TWeakPtr が残っている間は制御ブロックが
// 生き続け、Lock()/Expired() を安全に呼べる（std::weak_ptr と同じ仕組み）。
struct ControlBlock {
    TAtomic<u32> strong {1};
    TAtomic<u32> weak   {1};
    FAllocator*  alloc  = nullptr;
    void (*destroy_obj)(ControlBlock*) noexcept = nullptr;  // T のデストラクタを実行
    void (*free_self)  (ControlBlock*) noexcept = nullptr;  // ブロック全体を解放

    void AddStrong() noexcept { strong.FetchAdd(1); }
    void AddWeak()   noexcept { weak.FetchAdd(1); }

    void ReleaseStrong() noexcept {
        if (strong.FetchSub(1) == 1) {     // 強参照 1→0
            destroy_obj(this);             //   T を破棄（領域はまだ残す）
            ReleaseWeak();                 //   強参照グループが保持していた弱参照を返す
        }
    }
    void ReleaseWeak() noexcept {
        if (weak.FetchSub(1) == 1) free_self(this);  // 弱参照 1→0: 領域解放
    }
    // 弱参照→強参照の昇格。生きていれば strong を +1 して true。
    bool TryAddStrong() noexcept {
        u32 s = strong.Load(EMemoryOrder::Acquire);
        while (s != 0) {
            if (strong.CompareExchange(s, s + 1)) return true;  // 失敗時 s は実値に更新される
        }
        return false;
    }
    u32 StrongCount() const noexcept { return strong.Load(EMemoryOrder::Acquire); }
};

// ControlBlock + T を 1 ブロックに置くインライン版（MakeShared 用）。
template<typename T>
struct InlineBlock : ControlBlock {
    alignas(T) byte storage[sizeof(T)];
    T* Ptr() noexcept { return reinterpret_cast<T*>(&storage[0]); }

    static void DestroyObj(ControlBlock* cb) noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>)
            static_cast<InlineBlock*>(cb)->Ptr()->~T();
    }
    static void FreeSelf(ControlBlock* cb) noexcept {
        auto* self = static_cast<InlineBlock*>(cb);
        FAllocator* a = self->alloc;
        a->Free(self);
    }
};

// T が TSharedFromThis<T> を継承していれば、生成直後に weak-this を仕込む。
template<typename T> void HookSharedFromThis(const TSharedPtr<T>& sp) noexcept;

} // namespace sp_detail

// =============================================================================
// TSharedPtr<T>
// =============================================================================
template<typename T>
class TSharedPtr {
public:
    using ElementType = T;

    TSharedPtr() noexcept = default;
    TSharedPtr(decltype(nullptr)) noexcept {}

    TSharedPtr(const TSharedPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb) m_Cb->AddStrong();
    }
    TSharedPtr(TSharedPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }
    // 派生 U → 基底 T へのアップキャスト変換
    template<typename U>
    TSharedPtr(const TSharedPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb) m_Cb->AddStrong();
    }
    template<typename U>
    TSharedPtr(TSharedPtr<U>&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }

    TSharedPtr& operator=(const TSharedPtr& o) noexcept { TSharedPtr(o).Swap(*this); return *this; }
    TSharedPtr& operator=(TSharedPtr&& o)      noexcept { TSharedPtr(static_cast<TSharedPtr&&>(o)).Swap(*this); return *this; }
    TSharedPtr& operator=(decltype(nullptr))   noexcept { Reset(); return *this; }

    ~TSharedPtr() noexcept { if (m_Cb) m_Cb->ReleaseStrong(); }

    // ---- アクセス ----
    T*  Get()        const noexcept { return m_Ptr; }
    T&  operator*()  const noexcept { return *m_Ptr; }
    T*  operator->() const noexcept { return m_Ptr; }
    explicit operator bool() const noexcept { return m_Ptr != nullptr; }
    bool IsValid() const noexcept { return m_Ptr != nullptr; }

    // 現在の強参照数（デバッグ用）
    u32 UseCount() const noexcept { return m_Cb ? m_Cb->StrongCount() : 0; }

    void Reset() noexcept { TSharedPtr().Swap(*this); }
    void Swap(TSharedPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    T*                       m_Ptr = nullptr;
    sp_detail::ControlBlock* m_Cb  = nullptr;

    // 既に +1 済みの強参照を「採用」する（MakeShared / TWeakPtr::Lock 用、追加で +1 しない）
    TSharedPtr(T* p, sp_detail::ControlBlock* cb) noexcept : m_Ptr(p), m_Cb(cb) {}

    template<typename U> friend class TSharedPtr;
    template<typename U> friend class TWeakPtr;
    template<typename U, typename... A> friend TSharedPtr<U> MakeSharedIn(FAllocator&, A&&...) noexcept;
};

// =============================================================================
// TWeakPtr<T>
// =============================================================================
template<typename T>
class TWeakPtr {
public:
    TWeakPtr() noexcept = default;

    TWeakPtr(const TSharedPtr<T>& s) noexcept : m_Ptr(s.m_Ptr), m_Cb(s.m_Cb) {
        if (m_Cb) m_Cb->AddWeak();
    }
    TWeakPtr(const TWeakPtr& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb) m_Cb->AddWeak();
    }
    TWeakPtr(TWeakPtr&& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        o.m_Ptr = nullptr; o.m_Cb = nullptr;
    }
    template<typename U>
    TWeakPtr(const TSharedPtr<U>& s) noexcept : m_Ptr(s.m_Ptr), m_Cb(s.m_Cb) {
        if (m_Cb) m_Cb->AddWeak();
    }
    template<typename U>
    TWeakPtr(const TWeakPtr<U>& o) noexcept : m_Ptr(o.m_Ptr), m_Cb(o.m_Cb) {
        if (m_Cb) m_Cb->AddWeak();
    }

    TWeakPtr& operator=(const TWeakPtr& o)      noexcept { TWeakPtr(o).Swap(*this); return *this; }
    TWeakPtr& operator=(TWeakPtr&& o)           noexcept { TWeakPtr(static_cast<TWeakPtr&&>(o)).Swap(*this); return *this; }
    TWeakPtr& operator=(const TSharedPtr<T>& s) noexcept { TWeakPtr(s).Swap(*this); return *this; }

    ~TWeakPtr() noexcept { if (m_Cb) m_Cb->ReleaseWeak(); }

    // 対象が既に破棄されていれば true
    bool Expired() const noexcept { return !m_Cb || m_Cb->StrongCount() == 0; }
    bool IsValid() const noexcept { return !Expired(); }

    // 生きていれば強参照（TSharedPtr）を得る。死んでいれば空を返す。
    TSharedPtr<T> Lock() const noexcept {
        if (m_Cb && m_Cb->TryAddStrong()) return TSharedPtr<T>(m_Ptr, m_Cb);
        return TSharedPtr<T>();
    }

    void Reset() noexcept { TWeakPtr().Swap(*this); }
    void Swap(TWeakPtr& o) noexcept {
        T* p = m_Ptr; m_Ptr = o.m_Ptr; o.m_Ptr = p;
        sp_detail::ControlBlock* c = m_Cb; m_Cb = o.m_Cb; o.m_Cb = c;
    }

private:
    T*                       m_Ptr = nullptr;
    sp_detail::ControlBlock* m_Cb  = nullptr;

    template<typename U> friend class TWeakPtr;
    template<typename U> friend class TSharedPtr;
};

// =============================================================================
// TSharedFromThis<T> — enable_shared_from_this 相当
//   T を public 継承させると、メンバ関数内で AsShared() / AsWeak() が使える。
//   必ず MakeShared 経由で生成すること（生ポインタからの構築では機能しない）。
// =============================================================================
template<typename T>
class TSharedFromThis {
public:
    TSharedPtr<T> AsShared() noexcept { return m_WeakThis.Lock(); }
    TWeakPtr<T>   AsWeak()   const noexcept { return m_WeakThis; }

protected:
    TSharedFromThis() noexcept = default;
    TSharedFromThis(const TSharedFromThis&) noexcept {}                 // weak-this はコピーしない
    TSharedFromThis& operator=(const TSharedFromThis&) noexcept { return *this; }
    ~TSharedFromThis() noexcept = default;

private:
    mutable TWeakPtr<T> m_WeakThis;
    template<typename U> friend void sp_detail::HookSharedFromThis(const TSharedPtr<U>&) noexcept;
};

namespace sp_detail {
template<typename T>
void HookSharedFromThis(const TSharedPtr<T>& sp) noexcept {
    if constexpr (IsBaseOfV<TSharedFromThis<T>, T>) {
        static_cast<TSharedFromThis<T>*>(sp.Get())->m_WeakThis = sp;
    }
}
} // namespace sp_detail

// =============================================================================
// ファクトリ
// =============================================================================

// 指定アロケータで構築（ControlBlock と T を 1 アロケーションに同居）
template<typename T, typename... Args>
ACS_FORCEINLINE TSharedPtr<T> MakeSharedIn(FAllocator& a, Args&&... args) noexcept {
    using Block = sp_detail::InlineBlock<T>;
    void* mem = a.Alloc(sizeof(Block), alignof(Block), FSourceLoc::Current());
    if (!mem) return TSharedPtr<T>();
    auto* blk = ::new (mem) Block();
    blk->alloc       = &a;
    blk->destroy_obj = &Block::DestroyObj;
    blk->free_self   = &Block::FreeSelf;
    ::new (blk->Ptr()) T(Forward<Args>(args)...);
    TSharedPtr<T> sp(blk->Ptr(), blk);          // strong=1 を採用
    sp_detail::HookSharedFromThis(sp);          // TSharedFromThis 継承時のみ weak-this を仕込む
    return sp;
}

// デフォルトアロケータで構築（make_shared 相当）
template<typename T, typename... Args>
ACS_FORCEINLINE TSharedPtr<T> MakeShared(Args&&... args) noexcept {
    return MakeSharedIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

} // namespace acs
