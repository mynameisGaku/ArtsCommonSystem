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
FString::FString() noexcept : m_Alloc(&DefaultAllocator()) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
}

FString::FString(FAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
}

// C 文字列から構築
FString::FString(const char* cstr, FAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    if (cstr) Append(FStringView(cstr));
}

// FStringView から構築
FString::FString(FStringView v, FAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    Append(v);
}

// コピー: 単に Append（SSO/Heap の遷移は Append 内で適切に処理）
FString::FString(const FString& o) noexcept : m_Alloc(o.m_Alloc) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    Append(o.View());
}

// ムーブ: ヒープなら所有権移譲、SSO なら memcpy
FString::FString(FString&& o) noexcept : m_Alloc(o.m_Alloc) {
    if (o.IsHeap()) {
        m_Heap.data     = o.m_Heap.data;
        m_Heap.size     = o.m_Heap.size;
        m_Heap.capacity = o.m_Heap.capacity;
        m_Sso.remaining = 0x80;  // ヒープフラグ立てる
        // 元を空にリセット
        o.m_Sso.data[0]   = 0;
        o.SetInlineLen(0);
    } else {
        usize n = o.Size();
        for (usize i = 0; i <= n; ++i) m_Sso.data[i] = o.m_Sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o.m_Sso.data[0] = 0;
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
        m_Alloc->Free(m_Heap.data);
        m_Sso.remaining = 0;  // インライン空状態にリセット
    }
    m_Alloc = o.m_Alloc;
    if (o.IsHeap()) {
        m_Heap.data     = o.m_Heap.data;
        m_Heap.size     = o.m_Heap.size;
        m_Heap.capacity = o.m_Heap.capacity;
        m_Sso.remaining = 0x80;
        o.m_Sso.data[0] = 0;
        o.SetInlineLen(0);
    } else {
        usize n = o.Size();
        for (usize i = 0; i <= n; ++i) m_Sso.data[i] = o.m_Sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o.m_Sso.data[0] = 0;
        o.SetInlineLen(0);
    }
    return *this;
}

FString::~FString() noexcept {
    if (IsHeap()) m_Alloc->Free(m_Heap.data);
}

// 空文字列にリセット（容量は保持）
void FString::Clear() noexcept {
    if (IsHeap()) {
        m_Heap.size = 0;
        m_Heap.data[0] = 0;
    } else {
        m_Sso.data[0] = 0;
        SetInlineLen(0);
    }
}

// 容量拡大: 必要なら SSO → Heap に遷移
void FString::Grow(usize new_capacity) noexcept {
    if (new_capacity <= Capacity()) return;
    usize cap = new_capacity < 32 ? 32 : new_capacity;
    char* p = static_cast<char*>(m_Alloc->Alloc(cap + 1, alignof(char), FSourceLoc::Current()));
    ACS_ASSERTF(p, "FString::Grow: alloc failed (cap=%zu)", cap);
    usize old_size = Size();
    const char* old = Data();
    // NUL 含めてコピー
    for (usize i = 0; i <= old_size; ++i) p[i] = old[i];
    if (IsHeap()) m_Alloc->Free(m_Heap.data);
    m_Heap.data     = p;
    m_Heap.size     = old_size;
    m_Heap.capacity = cap;
    m_Sso.remaining = 0x80;  // ヒープフラグ
}

void FString::Reserve(usize new_capacity) noexcept {
    if (new_capacity > Capacity()) Grow(new_capacity);
}

// 文字列追記。容量不足なら 1.5 倍ずつ拡大
void FString::Append(FStringView v) noexcept {
    if (v.IsEmpty()) return;
    const usize cur  = Size();
    const usize vlen = v.Size();
    const usize req  = cur + vlen;

    // self-aliasing 検出: v が自分のバッファ内容を指しているか (s += s /
    // s.Append(s.View()) / 自分の部分文字列の append)。Grow() は旧バッファを
    // Free して内容を新バッファへコピーするので、aliasing したまま後で v.Data()
    // を読むと use-after-free / overwritten-union 読みになる (auditor 指摘)。
    const char* const old_data = Data();
    const char* const vd       = v.Data();
    const bool  aliases = (vd >= old_data && vd < old_data + cur);
    const usize voff    = aliases ? static_cast<usize>(vd - old_data) : 0;

    if (req > Capacity()) {
        usize n = Capacity() == 0 ? kSsoCapacity : Capacity();
        // 幾何級数拡大。n + n/2 + 1 が usize を wrap したら req で打ち切る
        // (異常な巨大 req でも under-allocate しない防御)。
        while (n < req) {
            const usize next = n + n / 2 + 1;
            if (next <= n) { n = req; break; }   // overflow → req に確定
            n = next;
        }
        Grow(n);   // 旧内容 [0,cur)+NUL を新バッファへコピー (aliasing バイトも移動)
    }

    char* d = Data();
    // aliasing していたら Grow がコピー済みの新バッファ内同オフセットから読む。
    // v は content の部分文字列 (voff+vlen <= cur) なので src=[voff,voff+vlen) は
    // dst=[cur,req) と重ならない (src 終端 <= cur = dst 先頭) → 前方コピーで安全。
    const char* src = aliases ? (d + voff) : vd;
    MemMove(d + cur, src, vlen);   // 非重なりだが防御的に MemMove
    d[req] = 0;
    if (IsHeap()) m_Heap.size = req;
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
    if (IsHeap()) m_Heap.size = cur + static_cast<usize>(wrote);
    else          SetInlineLen(static_cast<u8>(cur + static_cast<usize>(wrote)));
    return Size();
}

} // namespace acs
