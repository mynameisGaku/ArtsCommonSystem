// SPDX-License-Identifier: Apache-2.0
// UTF-8 可変長文字列（SSO 22 バイト対応、std::string 代替）
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/Assert.h"
#include "memory/Allocator.h"
#include "memory/Memory.h"             // DefaultAllocator()
#include "container/StringView.h"

namespace acs {

class FString {
public:
    static constexpr usize kSsoCapacity = 22;  // インライン上限（NUL 含めず）

    FString() noexcept;
    explicit FString(FAllocator& a) noexcept;
    FString(const char* cstr, FAllocator& a = DefaultAllocator()) noexcept;
    FString(FStringView v,    FAllocator& a = DefaultAllocator()) noexcept;

    FString(const FString& o) noexcept;
    FString(FString&& o)      noexcept;
    FString& operator=(const FString& o) noexcept;
    FString& operator=(FString&& o)      noexcept;
    ~FString() noexcept;

    const char* Data()  const noexcept { return IsHeap() ? m_Heap.data : m_Sso.data; }
    char*       Data()        noexcept { return IsHeap() ? m_Heap.data : m_Sso.data; }
    usize       Size()  const noexcept { return IsHeap() ? m_Heap.size : (kSsoCapacity - m_Sso.remaining); }
    // ヒープフラグ (m_Sso.remaining の MSB) は m_Heap.capacity の最上位バイト
    // (LE x64 で bit 63) に union で重なる。容量として読む際は必ずフラグ bit を
    // マスクすること。さもないと Capacity() が 0x8000... 0000 を足した巨大値を返し、
    // Append の拡大判定 (req > Capacity()) が常に偽となって heap バッファを溢れる。
    usize       Capacity() const noexcept { return IsHeap() ? (m_Heap.capacity & ~kHeapFlagBit) : kSsoCapacity; }
    bool        IsEmpty() const noexcept { return Size() == 0; }
    FStringView  View()  const noexcept { return FStringView(Data(), Size()); }
    operator FStringView() const noexcept { return View(); }

    char&       operator[](usize i)       noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }
    const char& operator[](usize i) const noexcept { ACS_ASSERT(i < Size()); return Data()[i]; }

    void Clear() noexcept;
    void Reserve(usize new_capacity) noexcept;
    void Append(FStringView v) noexcept;
    void Append(char c)        noexcept;
    void PushBack(char c)      noexcept { Append(c); }

    // printf 風フォーマット追記。最終サイズを返す（失敗時は 0）
    usize AppendFormat(const char* fmt, ...) noexcept;

    FAllocator* GetAllocator() const noexcept { return m_Alloc; }

private:
    // ヒープフラグ bit。m_Sso.remaining の MSB (= LE x64 で m_Heap.capacity の bit 63)。
    static constexpr usize kHeapFlagBit = usize{1} << 63;

    // remaining バイトの MSB がヒープフラグ
    bool IsHeap() const noexcept { return (m_Sso.remaining & 0x80) != 0; }
    void SetHeap() noexcept { m_Sso.remaining |= 0x80; }
    void SetInlineLen(u8 len) noexcept { m_Sso.remaining = static_cast<u8>(kSsoCapacity - len); }

    void Grow(usize new_capacity) noexcept;

    // SSO 領域とヒープポインタを union で共有（24 バイト固定）
    union {
        struct {
            char  data[kSsoCapacity + 1]; // +1 = NUL 終端
            u8    remaining;              // SSO: 残り容量、MSB がヒープフラグ
        } m_Sso;
        struct {
            char* data;
            usize size;
            usize capacity;
            // remaining フィールドは m_Sso.remaining と同じバイトに重なる
        } m_Heap;
    };
    FAllocator* m_Alloc = nullptr;
};

inline bool operator==(const FString& a, FStringView b) noexcept { return a.View() == b; }
inline bool operator==(FStringView a, const FString& b) noexcept { return a == b.View(); }
inline bool operator==(const FString& a, const FString& b) noexcept { return a.View() == b.View(); }

} // namespace acs
