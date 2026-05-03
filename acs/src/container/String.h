// UTF-8 可変長文字列（SSO 22 バイト対応、std::string 代替）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"             // DefaultAllocator()
#include "container/StringView.h"

namespace acs {

class String {
public:
    static constexpr usize kSsoCapacity = 22;  // インライン上限（NUL 含めず）

    String() noexcept;
    explicit String(Allocator& a) noexcept;
    String(const char* cstr, Allocator& a = DefaultAllocator()) noexcept;
    String(StringView v,    Allocator& a = DefaultAllocator()) noexcept;

    String(const String& o) noexcept;
    String(String&& o)      noexcept;
    String& operator=(const String& o) noexcept;
    String& operator=(String&& o)      noexcept;
    ~String() noexcept;

    const char* Data()  const noexcept { return IsHeap() ? _heap.data : _sso.data; }
    char*       Data()        noexcept { return IsHeap() ? _heap.data : _sso.data; }
    usize       Size()  const noexcept { return IsHeap() ? _heap.size : (kSsoCapacity - _sso.remaining); }
    usize       Capacity() const noexcept { return IsHeap() ? _heap.capacity : kSsoCapacity; }
    bool        IsEmpty() const noexcept { return Size() == 0; }
    StringView  View()  const noexcept { return StringView(Data(), Size()); }
    operator StringView() const noexcept { return View(); }

    char&       operator[](usize i)       noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }
    const char& operator[](usize i) const noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }

    void Clear() noexcept;
    void Reserve(usize new_capacity) noexcept;
    void Append(StringView v) noexcept;
    void Append(char c)        noexcept;
    void PushBack(char c)      noexcept { Append(c); }

    // printf 風フォーマット追記。最終サイズを返す（失敗時は 0）
    usize AppendFormat(const char* fmt, ...) noexcept;

    Allocator* GetAllocator() const noexcept { return _alloc; }

private:
    // remaining バイトの MSB がヒープフラグ
    bool IsHeap() const noexcept { return (_sso.remaining & 0x80) != 0; }
    void SetHeap() noexcept { _sso.remaining |= 0x80; }
    void SetInlineLen(u8 len) noexcept { _sso.remaining = static_cast<u8>(kSsoCapacity - len); }

    void Grow(usize new_capacity) noexcept;

    // SSO 領域とヒープポインタを union で共有（24 バイト固定）
    union {
        struct {
            char  data[kSsoCapacity + 1]; // +1 = NUL 終端
            u8    remaining;              // SSO: 残り容量、MSB がヒープフラグ
        } _sso;
        struct {
            char* data;
            usize size;
            usize capacity;
            // remaining フィールドは _sso.remaining と同じバイトに重なる
        } _heap;
    };
    Allocator* _alloc = nullptr;
};

inline bool operator==(const String& a, StringView b) noexcept { return a.View() == b; }
inline bool operator==(StringView a, const String& b) noexcept { return a == b.View(); }
inline bool operator==(const String& a, const String& b) noexcept { return a.View() == b.View(); }

} // namespace acs
