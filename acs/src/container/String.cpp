// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Container — FString 実装
// -----------------------------------------------------------------------------
// SSO の遷移ロジックと vsnprintf によるフォーマット追記。
// =============================================================================
#include "container/String.h"
#include "memory/Memory.h"
#include "foundation/Move.h"

#include <cstdarg>
#include <cstdio>

namespace acs {

// 既定構築: SSO で空文字列
FString::FString() noexcept : _alloc(&DefaultAllocator()) {
    _sso.data[0] = 0;
    SetInlineLen(0);
}

FString::FString(Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
}

// C 文字列から構築
FString::FString(const char* cstr, Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    if (cstr) Append(FStringView(cstr));
}

// FStringView から構築
FString::FString(FStringView v, Allocator& a) noexcept : _alloc(&a) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    Append(v);
}

// コピー: 単に Append（SSO/Heap の遷移は Append 内で適切に処理）
FString::FString(const FString& o) noexcept : _alloc(o._alloc) {
    _sso.data[0] = 0;
    SetInlineLen(0);
    Append(o.View());
}

// ムーブ: ヒープなら所有権移譲、SSO なら memcpy
FString::FString(FString&& o) noexcept : _alloc(o._alloc) {
    if (o.IsHeap()) {
        _heap.data     = o._heap.data;
        _heap.size     = o._heap.size;
        _heap.capacity = o._heap.capacity;
        _sso.remaining = 0x80;  // ヒープフラグ立てる
        // 元を空にリセット
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

FString& FString::operator=(const FString& o) noexcept {
    if (this == &o) return *this;
    Clear();
    Append(o.View());
    return *this;
}

FString& FString::operator=(FString&& o) noexcept {
    if (this == &o) return *this;
    Clear();
    if (IsHeap()) {
        _alloc->Free(_heap.data);
        _sso.remaining = 0;  // インライン空状態にリセット
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

FString::~FString() noexcept {
    if (IsHeap()) _alloc->Free(_heap.data);
}

// 空文字列にリセット（容量は保持）
void FString::Clear() noexcept {
    if (IsHeap()) {
        _heap.size = 0;
        _heap.data[0] = 0;
    } else {
        _sso.data[0] = 0;
        SetInlineLen(0);
    }
}

// 容量拡大: 必要なら SSO → Heap に遷移
void FString::Grow(usize new_capacity) noexcept {
    if (new_capacity <= Capacity()) return;
    usize cap = new_capacity < 32 ? 32 : new_capacity;
    char* p = static_cast<char*>(_alloc->Alloc(cap + 1, alignof(char), FSourceLoc::Current()));
    ACS_ASSERTF(p, "FString::Grow: alloc failed (cap=%zu)", cap);
    usize old_size = Size();
    const char* old = Data();
    // NUL 含めてコピー
    for (usize i = 0; i <= old_size; ++i) p[i] = old[i];
    if (IsHeap()) _alloc->Free(_heap.data);
    _heap.data     = p;
    _heap.size     = old_size;
    _heap.capacity = cap;
    _sso.remaining = 0x80;  // ヒープフラグ
}

void FString::Reserve(usize new_capacity) noexcept {
    if (new_capacity > Capacity()) Grow(new_capacity);
}

// 文字列追記。容量不足なら 1.5 倍ずつ拡大
void FString::Append(FStringView v) noexcept {
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

void FString::Append(char c) noexcept {
    Append(FStringView(&c, 1));
}

// printf スタイルのフォーマット追記
// 1) vsnprintf(nullptr) で必要長を計算
// 2) Reserve して書き込み
usize FString::AppendFormat(const char* fmt, ...) noexcept {
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
