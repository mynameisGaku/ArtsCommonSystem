// ACS Container — Owning UTF-8 string with small-string optimization (SSO).
//
// Layout: 24-byte struct (on x64). When length < 23, the bytes live inline;
// otherwise data lives on the heap. The discriminator is the most significant
// bit of the heap-flag stored at the last byte (0 = inline length, 1 = heap).
//
// NOT thread-safe.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"
#include "memory/Allocator.h"
#include "container/StringView.h"

namespace acs {

class String {
public:
    static constexpr usize kSsoCapacity = 22;

    String() noexcept;
    explicit String(Allocator& a) noexcept;
    String(const char* cstr, Allocator& a = DefaultAllocator()) noexcept;
    String(StringView v,    Allocator& a = DefaultAllocator()) noexcept;

    String(const String& o) noexcept;
    String(String&& o)      noexcept;
    String& operator=(const String& o) noexcept;
    String& operator=(String&& o)      noexcept;
    ~String() noexcept;

    // ---- Inspect -----------------------------------------------------------
    const char* Data()  const noexcept { return IsHeap() ? _heap.data : _sso.data; }
    char*       Data()        noexcept { return IsHeap() ? _heap.data : _sso.data; }
    usize       Size()  const noexcept { return IsHeap() ? _heap.size : (kSsoCapacity - _sso.remaining); }
    usize       Capacity() const noexcept { return IsHeap() ? _heap.capacity : kSsoCapacity; }
    bool        IsEmpty() const noexcept { return Size() == 0; }
    StringView  View()  const noexcept { return StringView(Data(), Size()); }
    operator StringView() const noexcept { return View(); }

    char&       operator[](usize i)       noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }
    const char& operator[](usize i) const noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }

    // ---- Modify ------------------------------------------------------------
    void Clear() noexcept;
    void Reserve(usize new_capacity) noexcept;
    void Append(StringView v) noexcept;
    void Append(char c)        noexcept;
    void PushBack(char c)      noexcept { Append(c); }

    // printf-style formatted append. Returns final size on success, 0 on
    // formatting error.
    usize AppendFormat(const char* fmt, ...) noexcept;

    Allocator* GetAllocator() const noexcept { return _alloc; }

private:
    bool IsHeap() const noexcept { return (_sso.remaining & 0x80) != 0; }
    void SetHeap() noexcept { _sso.remaining |= 0x80; }
    void SetInlineLen(u8 len) noexcept { _sso.remaining = static_cast<u8>(kSsoCapacity - len); }

    void Grow(usize new_capacity) noexcept;

    union {
        struct {
            char  data[kSsoCapacity + 1]; // +1 for null terminator
            u8    remaining;              // SSO: kSsoCapacity - len; heap-flag bit set if heap
        } _sso;
        struct {
            char* data;
            usize size;
            usize capacity;
            // Note: relies on remaining-byte alignment; we set remaining via _sso.remaining
            // when transitioning. Heap-flag bit lives there.
        } _heap;
    };
    Allocator* _alloc = nullptr;
};

inline bool operator==(const String& a, StringView b) noexcept { return a.View() == b; }
inline bool operator==(StringView a, const String& b) noexcept { return a == b.View(); }
inline bool operator==(const String& a, const String& b) noexcept { return a.View() == b.View(); }

} // namespace acs
