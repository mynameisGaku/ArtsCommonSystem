// ACS Container — Dynamic array (vector replacement, no STL).
//
// * Allocator is injected at construction (defaults to engine DefaultAllocator).
// * Move-only by default (copies are explicit via Clone()).
// * Trivially-copyable element fast paths use MemCopy/MemMove instead of
//   per-element ctors/dtors.
//
// Thread-safety: Array<T> itself is NOT thread-safe; use external
// synchronization or a thread-safe wrapper.
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

template<typename T>
class Array {
public:
    Array() noexcept : _alloc(&DefaultAllocator()) {}
    explicit Array(Allocator& a) noexcept : _alloc(&a) {}
    Array(usize initial_capacity, Allocator& a = DefaultAllocator()) noexcept : _alloc(&a) {
        Reserve(initial_capacity);
    }

    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    Array(Array&& o) noexcept
        : _data(o._data), _size(o._size), _capacity(o._capacity), _alloc(o._alloc) {
        o._data = nullptr; o._size = 0; o._capacity = 0;
    }
    Array& operator=(Array&& o) noexcept {
        if (this == &o) return *this;
        Clear();
        Free();
        _data = o._data; _size = o._size; _capacity = o._capacity; _alloc = o._alloc;
        o._data = nullptr; o._size = 0; o._capacity = 0;
        return *this;
    }
    ~Array() noexcept { Clear(); Free(); }

    // ---- Capacity ----------------------------------------------------------
    usize Size()     const noexcept { return _size; }
    usize Capacity() const noexcept { return _capacity; }
    bool  IsEmpty()  const noexcept { return _size == 0; }

    void Reserve(usize new_capacity) noexcept {
        if (new_capacity <= _capacity) return;
        Grow(new_capacity);
    }

    void Resize(usize new_size) noexcept {
        if (new_size > _capacity) Grow(NextGrow(new_size));
        if (new_size > _size) {
            if constexpr (IsTriviallyConstructibleV<T>) {
                MemSet(static_cast<void*>(_data + _size), 0, sizeof(T) * (new_size - _size));
            } else {
                for (usize i = _size; i < new_size; ++i) ::new (&_data[i]) T();
            }
        } else if (new_size < _size) {
            if constexpr (!IsTriviallyDestructibleV<T>) {
                for (usize i = new_size; i < _size; ++i) _data[i].~T();
            }
        }
        _size = new_size;
    }

    void Clear() noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            for (usize i = _size; i-- > 0;) _data[i].~T();
        }
        _size = 0;
    }

    void ShrinkToFit() noexcept {
        if (_size == _capacity) return;
        Grow(_size);
    }

    // ---- Access ------------------------------------------------------------
    T*       Data()       noexcept { return _data; }
    const T* Data() const noexcept { return _data; }
    T&       operator[](usize i)       noexcept { ACS_ASSERT(i < _size); return _data[i]; }
    const T& operator[](usize i) const noexcept { ACS_ASSERT(i < _size); return _data[i]; }
    T&       Front()       noexcept { ACS_ASSERT(_size > 0); return _data[0]; }
    const T& Front() const noexcept { ACS_ASSERT(_size > 0); return _data[0]; }
    T&       Back ()       noexcept { ACS_ASSERT(_size > 0); return _data[_size - 1]; }
    const T& Back () const noexcept { ACS_ASSERT(_size > 0); return _data[_size - 1]; }

    T*       begin()       noexcept { return _data; }
    T*       end()         noexcept { return _data + _size; }
    const T* begin() const noexcept { return _data; }
    const T* end()   const noexcept { return _data + _size; }

    Span<T> AsSpan() noexcept { return Span<T>(_data, _size); }
    Span<const T> AsSpan() const noexcept { return Span<const T>(_data, _size); }

    // ---- Modify ------------------------------------------------------------
    void PushBack(const T& v) noexcept {
        if (_size == _capacity) Grow(NextGrow(_size + 1));
        ::new (&_data[_size]) T(v);
        ++_size;
    }
    void PushBack(T&& v) noexcept {
        if (_size == _capacity) Grow(NextGrow(_size + 1));
        ::new (&_data[_size]) T(Move(v));
        ++_size;
    }
    template<typename... Args>
    T& EmplaceBack(Args&&... args) noexcept {
        if (_size == _capacity) Grow(NextGrow(_size + 1));
        ::new (&_data[_size]) T(Forward<Args>(args)...);
        return _data[_size++];
    }
    void PopBack() noexcept {
        ACS_ASSERT(_size > 0);
        --_size;
        if constexpr (!IsTriviallyDestructibleV<T>) _data[_size].~T();
    }
    void RemoveAtSwap(usize i) noexcept {
        ACS_ASSERT(i < _size);
        --_size;
        if (i != _size) _data[i] = Move(_data[_size]);
        if constexpr (!IsTriviallyDestructibleV<T>) _data[_size].~T();
    }

    // Deep copy is explicit.
    Array Clone() const noexcept {
        Array c(_capacity, *_alloc);
        c.Resize(_size);
        if constexpr (IsTriviallyCopyableV<T>) {
            MemCopy(c._data, _data, sizeof(T) * _size);
        } else {
            for (usize i = 0; i < _size; ++i) c._data[i] = _data[i];
        }
        return c;
    }

    Allocator* GetAllocator() const noexcept { return _alloc; }

private:
    static usize NextGrow(usize required) noexcept {
        usize n = 8;
        while (n < required) n += n / 2 + 1;
        return n;
    }

    void Grow(usize new_capacity) noexcept {
        T* new_data = static_cast<T*>(_alloc->Alloc(sizeof(T) * new_capacity, alignof(T), SourceLoc::Current()));
        ACS_ASSERTF(new_data != nullptr, "Array::Grow: allocator returned null (cap=%zu, T=%zu)",
                    new_capacity, sizeof(T));
        if (_data) {
            if constexpr (IsTriviallyCopyableV<T>) {
                MemCopy(new_data, _data, sizeof(T) * _size);
            } else {
                for (usize i = 0; i < _size; ++i) {
                    ::new (&new_data[i]) T(Move(_data[i]));
                    if constexpr (!IsTriviallyDestructibleV<T>) _data[i].~T();
                }
            }
            _alloc->Free(_data);
        }
        _data = new_data;
        _capacity = new_capacity;
    }

    void Free() noexcept {
        if (_data) { _alloc->Free(_data); _data = nullptr; _capacity = 0; }
    }

    T*         _data     = nullptr;
    usize      _size     = 0;
    usize      _capacity = 0;
    Allocator* _alloc    = nullptr;
};

} // namespace acs
