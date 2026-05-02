#include "container/String.h"
#include "memory/Memory.h"
#include "foundation/Move.h"

#include <cstdarg>
#include <cstdio>

namespace acs {

String::String() noexcept : _alloc(&DefaultAllocator()) {
    _sso.data[0] = 0;
    SetInlineLen(0);
}

String::String(Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
}

String::String(const char* cstr, Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    if (cstr) Append(StringView(cstr));
}

String::String(StringView v, Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    Append(v);
}

String::String(const String& o) noexcept : _alloc(o._alloc) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    Append(o.View());
}

String::String(String&& o) noexcept : _alloc(o._alloc) {
    if (o.IsHeap()) {
        _heap.data     = o._heap.data;
        _heap.size     = o._heap.size;
        _heap.capacity = o._heap.capacity;
        _sso.remaining = 0x80;
        // Reset other to empty inline.
        o._sso.data[0]   = 0;
        o.SetInlineLen(0);
    } else {
        usize n = o.Size();
        for (usize i = 0; i <= n; ++i) _sso.data[i] = o._sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o._sso.data[0] = 0;
        o.SetInlineLen(0);
    }
}

String& String::operator=(const String& o) noexcept {
    if (this == &o) return *this;
    Clear();
    Append(o.View());
    return *this;
}

String& String::operator=(String&& o) noexcept {
    if (this == &o) return *this;
    Clear();
    if (IsHeap()) {
        _alloc->Free(_heap.data);
        _sso.remaining = 0; // back to inline empty
    }
    _alloc = o._alloc;
    if (o.IsHeap()) {
        _heap.data     = o._heap.data;
        _heap.size     = o._heap.size;
        _heap.capacity = o._heap.capacity;
        _sso.remaining = 0x80;
        o._sso.data[0] = 0;
        o.SetInlineLen(0);
    } else {
        usize n = o.Size();
        for (usize i = 0; i <= n; ++i) _sso.data[i] = o._sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o._sso.data[0] = 0;
        o.SetInlineLen(0);
    }
    return *this;
}

String::~String() noexcept {
    if (IsHeap()) _alloc->Free(_heap.data);
}

void String::Clear() noexcept {
    if (IsHeap()) {
        _heap.size = 0;
        _heap.data[0] = 0;
    } else {
        _sso.data[0] = 0;
        SetInlineLen(0);
    }
}

void String::Grow(usize new_capacity) noexcept {
    if (new_capacity <= Capacity()) return;
    usize cap = new_capacity < 32 ? 32 : new_capacity;
    char* p = static_cast<char*>(_alloc->Alloc(cap + 1, alignof(char), SourceLoc::Current()));
    ACS_ASSERTF(p, "String::Grow: alloc failed (cap=%zu)", cap);
    usize old_size = Size();
    const char* old = Data();
    for (usize i = 0; i <= old_size; ++i) p[i] = old[i];
    if (IsHeap()) _alloc->Free(_heap.data);
    _heap.data     = p;
    _heap.size     = old_size;
    _heap.capacity = cap;
    _sso.remaining = 0x80;
}

void String::Reserve(usize new_capacity) noexcept {
    if (new_capacity > Capacity()) Grow(new_capacity);
}

void String::Append(StringView v) noexcept {
    if (v.IsEmpty()) return;
    usize cur = Size();
    usize req = cur + v.Size();
    if (req > Capacity()) {
        usize n = Capacity() == 0 ? kSsoCapacity : Capacity();
        while (n < req) n = n + n / 2 + 1;
        Grow(n);
    }
    char* d = Data();
    for (usize i = 0; i < v.Size(); ++i) d[cur + i] = v[i];
    d[req] = 0;
    if (IsHeap()) _heap.size = req;
    else          SetInlineLen(static_cast<u8>(req));
}

void String::Append(char c) noexcept {
    Append(StringView(&c, 1));
}

usize String::AppendFormat(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int needed = ::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return 0; }
    usize cur = Size();
    Reserve(cur + static_cast<usize>(needed));
    char* d = Data();
    int wrote = ::vsnprintf(d + cur, static_cast<usize>(needed) + 1, fmt, ap2);
    va_end(ap2);
    if (wrote < 0) return 0;
    if (IsHeap()) _heap.size = cur + static_cast<usize>(wrote);
    else          SetInlineLen(static_cast<u8>(cur + static_cast<usize>(wrote)));
    return Size();
}

} // namespace acs
