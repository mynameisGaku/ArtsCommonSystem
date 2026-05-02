// =============================================================================
// ACS Container — Array<T>（std::vector 代替、可変長配列）
// -----------------------------------------------------------------------------
// 主要特徴:
//   - アロケータをコンストラクタで注入可能（DefaultAllocator もしくは指定）
//   - ムーブ専用（コピーしたい場合は明示的に Clone() を呼ぶ）
//   - トリビアルコピー可能型は MemCopy/MemMove で高速処理
//
// スレッド安全性:
//   Array<T> 自体は単一スレッド前提。並行読み取りは OK だが書き込みを
//   含む場合は外部 Mutex / RwLock で保護すること。
//
// 性能注意:
//   _alloc->Alloc() は仮想呼び出し → vtable 1 段間接。Grow は cold path
//   なのでホットパスへの影響は限定的だが、頻繁に Grow する場合は事前に
//   Reserve() で予約しておくこと。
// =============================================================================
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
    // ---- コンストラクタ ----
    Array() noexcept : _alloc(&DefaultAllocator()) {}
    explicit Array(Allocator& a) noexcept : _alloc(&a) {}
    Array(usize initial_capacity, Allocator& a = DefaultAllocator()) noexcept : _alloc(&a) {
        Reserve(initial_capacity);
    }

    // コピー禁止（明示的な Clone を強制してパフォーマンス事故を防ぐ）
    Array(const Array&) = delete;
    Array& operator=(const Array&) = delete;

    // ムーブ可
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

    // ---- 容量／サイズ ----
    usize Size()     const noexcept { return _size; }       // 要素数
    usize Capacity() const noexcept { return _capacity; }   // 確保済み容量
    bool  IsEmpty()  const noexcept { return _size == 0; }

    // 容量を予約（既存要素は保持）
    void Reserve(usize new_capacity) noexcept {
        if (new_capacity <= _capacity) return;
        Grow(new_capacity);
    }

    // サイズを変更（増やす場合はデフォルト構築、減らす場合はデストラクト）
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

    // サイズを 0 に（容量は保持、デストラクタは呼ぶ）
    void Clear() noexcept {
        if constexpr (!IsTriviallyDestructibleV<T>) {
            for (usize i = _size; i-- > 0;) _data[i].~T();
        }
        _size = 0;
    }

    // 余剰容量を解放
    void ShrinkToFit() noexcept {
        if (_size == _capacity) return;
        Grow(_size);
    }

    // ---- アクセス ----
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

    Span<T>       AsSpan()       noexcept { return Span<T>(_data, _size); }
    Span<const T> AsSpan() const noexcept { return Span<const T>(_data, _size); }

    // ---- 変更 ----
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

    // 高速削除: 末尾要素を i 番にムーブして縮める（順序は保たれない）
    void RemoveAtSwap(usize i) noexcept {
        ACS_ASSERT(i < _size);
        --_size;
        if (i != _size) _data[i] = Move(_data[_size]);
        if constexpr (!IsTriviallyDestructibleV<T>) _data[_size].~T();
    }

    // 明示的なコピー（高コストなので名前で意識させる）
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
    // 容量増加戦略: 8 → 12 → 18 → 27 → ...（約 1.5 倍）
    static usize NextGrow(usize required) noexcept {
        usize n = 8;
        while (n < required) n += n / 2 + 1;
        return n;
    }

    // 新容量で再確保し、既存要素をムーブ移送
    void Grow(usize new_capacity) noexcept {
        T* new_data = static_cast<T*>(_alloc->Alloc(sizeof(T) * new_capacity, alignof(T), SourceLoc::Current()));
        ACS_ASSERTF(new_data != nullptr, "Array::Grow: allocator returned null (cap=%zu, T=%zu)",
                    new_capacity, sizeof(T));
        if (_data) {
            if constexpr (IsTriviallyCopyableV<T>) {
                MemCopy(new_data, _data, sizeof(T) * _size);  // POD はバルクコピー
            } else {
                for (usize i = 0; i < _size; ++i) {
                    ::new (&new_data[i]) T(Move(_data[i]));    // ムーブコンストラクト
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
