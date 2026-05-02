// ACS Memory — Rc<T>: atomic-refcounted shared pointer.
//
// Layout: control block (refcount + allocator + deleter) lives next to T in a
// single allocation when constructed via MakeRc. Foreign pointers (constructor
// from raw T*) get a separately allocated control block.
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"
#include "threading/Atomic.h"
#include "foundation/TypeTraits.h"

namespace acs {

namespace rc_detail {

struct ControlBlock {
    Atomic<u32> strong {1};
    Allocator*  alloc  = nullptr;
    void (*destroy)(ControlBlock*) noexcept = nullptr;
};

template<typename T>
struct InlineBlock : ControlBlock {
    alignas(T) byte storage[sizeof(T)];
    T* Get() noexcept { return reinterpret_cast<T*>(&storage[0]); }
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
class Rc {
public:
    Rc() noexcept = default;

    Rc(const Rc& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        if (_cb) _cb->strong.FetchAdd(1);
    }
    Rc(Rc&& o) noexcept : _ptr(o._ptr), _cb(o._cb) {
        o._ptr = nullptr;
        o._cb  = nullptr;
    }
    Rc& operator=(const Rc& o) noexcept {
        if (this == &o) return *this;
        Release();
        _ptr = o._ptr;
        _cb  = o._cb;
        if (_cb) _cb->strong.FetchAdd(1);
        return *this;
    }
    Rc& operator=(Rc&& o) noexcept {
        if (this == &o) return *this;
        Release();
        _ptr = o._ptr;
        _cb  = o._cb;
        o._ptr = nullptr;
        o._cb  = nullptr;
        return *this;
    }
    ~Rc() noexcept { Release(); }

    T*       Get()       noexcept { return _ptr; }
    const T* Get() const noexcept { return _ptr; }
    T& operator*()  const noexcept { return *_ptr; }
    T* operator->() const noexcept { return _ptr; }
    explicit operator bool() const noexcept { return _ptr != nullptr; }

    u32 UseCount() const noexcept { return _cb ? _cb->strong.Load(MemoryOrder::Acquire) : 0; }

    template<typename U, typename... Args> friend Rc<U> MakeRc(Args&&...) noexcept;
    template<typename U, typename... Args> friend Rc<U> MakeRcIn(Allocator&, Args&&...) noexcept;

private:
    Rc(T* p, rc_detail::ControlBlock* cb) noexcept : _ptr(p), _cb(cb) {}

    void Release() noexcept {
        if (!_cb) return;
        if (_cb->strong.FetchSub(1) == 1) {
            _cb->destroy(_cb);
        }
        _ptr = nullptr;
        _cb  = nullptr;
    }

    T*                       _ptr = nullptr;
    rc_detail::ControlBlock* _cb  = nullptr;
};

template<typename T, typename... Args>
ACS_FORCEINLINE Rc<T> MakeRc(Args&&... args) noexcept {
    return MakeRcIn<T>(DefaultAllocator(), Forward<Args>(args)...);
}

template<typename T, typename... Args>
ACS_FORCEINLINE Rc<T> MakeRcIn(Allocator& a, Args&&... args) noexcept {
    using Block = rc_detail::InlineBlock<T>;
    void* mem = a.Alloc(sizeof(Block), alignof(Block), SourceLoc::Current());
    if (!mem) return Rc<T>();
    auto* blk = ::new (mem) Block();
    blk->alloc = &a;
    blk->destroy = &Block::Destroy;
    ::new (blk->Get()) T(Forward<Args>(args)...);
    return Rc<T>(blk->Get(), blk);
}

} // namespace acs
