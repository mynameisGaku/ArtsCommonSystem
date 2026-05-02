// ACS Memory — UniquePtr<T>: movable-only owning smart pointer.
#pragma once

#include "memory/Allocator.h"
#include "memory/New.h"
#include "memory/Memory.h"

namespace acs {

template<typename T>
class UniquePtr {
public:
    UniquePtr() noexcept = default;
    explicit UniquePtr(T* p, Allocator* a = nullptr) noexcept
        : _ptr(p), _alloc(a ? a : &DefaultAllocator()) {}

    UniquePtr(const UniquePtr&) = delete;
    UniquePtr& operator=(const UniquePtr&) = delete;

    UniquePtr(UniquePtr&& o) noexcept : _ptr(o._ptr), _alloc(o._alloc) {
        o._ptr = nullptr;
    }

    template<typename U>
    UniquePtr(UniquePtr<U>&& o) noexcept : _ptr(o.Release()), _alloc(o.GetAllocator()) {}

    UniquePtr& operator=(UniquePtr&& o) noexcept {
        if (this == &o) return *this;
        Reset();
        _ptr = o._ptr;
        _alloc = o._alloc;
        o._ptr = nullptr;
        return *this;
    }

    ~UniquePtr() noexcept { Reset(); }

    T*       Get()       noexcept { return _ptr; }
    const T* Get() const noexcept { return _ptr; }

    T& operator*()  const noexcept { return *_ptr; }
    T* operator->() const noexcept { return _ptr; }
    explicit operator bool() const noexcept { return _ptr != nullptr; }

    T* Release() noexcept {
        T* p = _ptr;
        _ptr = nullptr;
        return p;
    }

    void Reset(T* p = nullptr) noexcept {
        if (_ptr) Delete(*_alloc, _ptr);
        _ptr = p;
    }

    Allocator* GetAllocator() const noexcept { return _alloc; }

private:
    T*         _ptr   = nullptr;
    Allocator* _alloc = nullptr;
};

template<typename T, typename... Args>
ACS_FORCEINLINE UniquePtr<T> MakeUnique(Args&&... args) noexcept {
    Allocator& a = DefaultAllocator();
    T* p = New<T>(a, Forward<Args>(args)...);
    return UniquePtr<T>(p, &a);
}

template<typename T, typename... Args>
ACS_FORCEINLINE UniquePtr<T> MakeUniqueIn(Allocator& a, Args&&... args) noexcept {
    T* p = New<T>(a, Forward<Args>(args)...);
    return UniquePtr<T>(p, &a);
}

} // namespace acs
