// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — TRc<T>（std::shared_ptr 代替、アトミック参照カウント）
// -----------------------------------------------------------------------------
// 共有所有のスマートポインタ。コピー時に参照カウントを atomic incr、
// 破棄時に decr して 0 になったら対象を解放する。
//
// レイアウト:
//   MakeRc 経由で構築すると、ControlBlock と T が単一アロケーションに
//   置かれる（make_shared 相当）。foreign ポインタ用の別途確保バリアントは
//   現在未対応。
// =============================================================================
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "threading/Atomic.h"
#include "foundation/TypeTraits.h"

namespace acs {

namespace rc_detail {

// 全 TRc 共通の制御ブロック（参照カウント + アロケータ + デストラクタ）
struct ControlBlock {
    TAtomic<u32> strong {1};                            // 強参照カウント
    Allocator*  alloc  = nullptr;                      // 解放に使うアロケータ
    void (*destroy)(ControlBlock*) noexcept = nullptr; // T 破棄関数（型消去）
};

// ControlBlock + T を 1 ブロックに置くインライン版
template<typename T>
struct InlineBlock : ControlBlock {
    alignas(T) byte storage[sizeof(T)];   // T を埋め込む生バイト領域

    T* Get() noexcept { return reinterpret_cast<T*>(&storage[0]); }

    // 破棄: T のデストラクタ → InlineBlock 自身のデストラクタ → Free
    static void Destroy(ControlBlock* cb) noexcept {
        auto* self = static_cast<InlineBlock*>(cb);
        if constexpr (!IsTriviallyDestructibleV<T>) self->Get()->~T();
        Allocator* a = self->alloc;
        self->~InlineBlock();
        a->Free(self);
    }
};

} // namespace rc_detail

template<typename T>
class TRc {
public:
    TRc() noexcept = default;

    // コピー: 参照カウント increment
    TRc(const TRc& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        if (_cb) _cb->strong.FetchAdd(1);
    }
    // ムーブ: カウントは触らず所有権だけ移譲
    TRc(TRc&& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        o._ptr = nullptr;
        o._cb  = nullptr;
    }

    // U が T の派生型なら TRc<U> から TRc<T> へアップキャスト変換できる
    template<typename U>
    TRc(const TRc<U>& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        if (_cb) _cb->strong.FetchAdd(1);
    }
    template<typename U>
    TRc(TRc<U>&& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        o._ptr = nullptr;
        o._cb  = nullptr;
    }

    template<typename U> friend class TRc;  // U → T 変換のために privates を共有
    TRc& operator=(const TRc& o) noexcept {
        if (this == &o) return *this;
        Release();
        _ptr = o._ptr;
        _cb  = o._cb;
        if (_cb) _cb->strong.FetchAdd(1);
        return *this;
    }
    TRc& operator=(TRc&& o) noexcept {
        if (this == &o) return *this;
        Release();
        _ptr = o._ptr;
        _cb  = o._cb;
        o._ptr = nullptr;
        o._cb  = nullptr;
        return *this;
    }
    ~TRc() noexcept { Release(); }

    // ---- アクセス ----
    T*       Get()       noexcept { return _ptr; }
    const T* Get() const noexcept { return _ptr; }
    T& operator*()  const noexcept { return *_ptr; }
    T* operator->() const noexcept { return _ptr; }
    explicit operator bool() const noexcept { return _ptr != nullptr; }

    // 現在の参照数（デバッグ用、相対値）
    u32 UseCount() const noexcept { return _cb ? _cb->strong.Load(EMemoryOrder::Acquire) : 0; }

    template<typename U, typename... Args> friend TRc<U> MakeRc(Args&&...) noexcept;
    template<typename U, typename... Args> friend TRc<U> MakeRcIn(Allocator&, Args&&...) noexcept;

private:
    TRc(T* p, rc_detail::ControlBlock* cb) noexcept : _ptr(p), _cb(cb) {}

    // 参照カウントを 1 減らし、0 なら対象を解放する
    void Release() noexcept {
        if (!_cb) return;
        if (_cb->strong.FetchSub(1) == 1) {
            _cb->destroy(_cb);  // strong が 1 → 0 になったので破棄
        }
        _ptr = nullptr;
        _cb  = nullptr;
    }

    T*                       _ptr = nullptr;
    rc_detail::ControlBlock* _cb  = nullptr;
};

// ---- ファクトリ -----------------------------------------------------------

// デフォルトアロケータで構築（make_shared 相当）
template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRc(Args&&... args) noexcept {
    return MakeRcIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

// 指定アロケータで構築
template<typename T, typename... Args>
ACS_FORCEINLINE TRc<T> MakeRcIn(Allocator& a, Args&&... args) noexcept {
    using Block = rc_detail::InlineBlock<T>;
    void* mem = a.Alloc(sizeof(Block), alignof(Block), FSourceLoc::Current());
    if (!mem) return TRc<T>();
    auto* blk = ::new (mem) Block();
    blk->alloc = &a;
    blk->destroy = &Block::Destroy;
    ::new (blk->Get()) T(Forward<Args>(args)...);
    return TRc<T>(blk->Get(), blk);
}

} // namespace acs
