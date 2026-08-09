// SPDX-License-Identifier: Apache-2.0
// ACS Container — FString 実装
//
// SSO の遷移ロジックと vsnprintf によるフォーマット追記。
#include "container/String.h"
#include "memory/Memory.h"
#include "foundation/Move.h"

#include <cstdarg>
#include <cstdio>

namespace acs {

/** 既定アロケータで SSO の空文字列を構築する。 */
FString::FString() noexcept : m_Alloc(&DefaultAllocator()) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
}

/**
 * 指定アロケータで SSO の空文字列を構築する。
 *
 * @param a ヒープ確保に使うアロケータ。
 */
FString::FString(IAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
}

/**
 * C 文字列から構築する (空状態から Append でコピーする)。
 *
 * @param cstr NUL 終端文字列 (nullptr なら空文字列)。
 * @param a ヒープ確保に使うアロケータ。
 */
FString::FString(const char* cstr, IAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    if (cstr) Append(FStringView(cstr));
}

/**
 * FStringView から構築する (空状態から Append でコピーする)。
 *
 * @param v コピー元のビュー。
 * @param a ヒープ確保に使うアロケータ。
 */
FString::FString(FStringView v, IAllocator& a) noexcept : m_Alloc(&a) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    Append(v);
}

/**
 * コピー構築する (Append で内容を複製、SSO/Heap 遷移は Append が処理)。
 *
 * @param o コピー元。
 */
FString::FString(const FString& o) noexcept : m_Alloc(o.m_Alloc) {
    m_Sso.data[0] = 0;
    SetInlineLen(0);
    Append(o.View());
}

/**
 * ムーブ構築する (ヒープなら所有権移譲、SSO なら memcpy)。
 *
 * @param o ムーブ元 (空文字列にリセットされる)。
 */
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
        const usize n = o.Size();
        for (usize i = 0; i <= n; ++i) m_Sso.data[i] = o.m_Sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o.m_Sso.data[0] = 0;
        o.SetInlineLen(0);
    }
}

/**
 * コピー代入する (既存内容をクリアして Append で複製する)。
 *
 * @param o コピー元。
 * @return *this。
 */
FString& FString::operator=(const FString& o) noexcept {
    if (this == &o) return *this;
    Clear();
    Append(o.View());
    return *this;
}

/**
 * ムーブ代入する (既存ヒープを解放し、ヒープなら所有権移譲・SSO なら memcpy)。
 *
 * @param o ムーブ元 (空文字列にリセットされる)。
 * @return *this。
 */
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
        const usize n = o.Size();
        for (usize i = 0; i <= n; ++i) m_Sso.data[i] = o.m_Sso.data[i];
        SetInlineLen(static_cast<u8>(n));
        o.m_Sso.data[0] = 0;
        o.SetInlineLen(0);
    }
    return *this;
}

/** ヒープを使っていれば解放する。 */
FString::~FString() noexcept {
    if (IsHeap()) m_Alloc->Free(m_Heap.data);
}

/** 空文字列にリセットする (容量は保持)。 */
void FString::Clear() noexcept {
    if (IsHeap()) {
        m_Heap.size = 0;
        m_Heap.data[0] = 0;
    } else {
        m_Sso.data[0] = 0;
        SetInlineLen(0);
    }
}

/** 空文字列にし、ヒープ容量も解放する。 */
void FString::ReleaseStorage() noexcept
{
    if (IsHeap()) {
        m_Alloc->Free(m_Heap.data);
        m_Sso.remaining = 0;
    }
    m_Sso.data[0] = 0;
    SetInlineLen(0);
}

/**
 * 容量を拡大する (必要なら SSO からヒープへ遷移し、NUL 含め内容をコピーする)。
 *
 * @details 最小確保量は 32 バイト。確保失敗は ACS_ASSERTF で検出する。
 * @param new_capacity 確保する最小容量。
 */
void FString::Grow(usize new_capacity) noexcept {
    ACS_CHECKF(TryGrow(new_capacity), "FString::Grow failed (cap=%zu)", new_capacity);
}

/**
 * 容量拡大を試みる。確保に失敗したら文字列を変えず false を返す。
 *
 * @param new_capacity 確保する最小容量。
 * @return 成功なら true、OOM なら false。
 */
bool FString::TryGrow(usize new_capacity) noexcept {
    if (new_capacity <= Capacity()) return true;
    if (new_capacity >= kHeapFlagBit) return false;

    /** 最低32 byteを確保する実容量。 */
    const usize cap = new_capacity < 32 ? 32 : new_capacity;
    if (cap >= kHeapFlagBit || cap == ~usize(0)) return false;

    /** 再確保後も維持する現在の文字列長。 */
    const usize old_size = Size();
    if (IsHeap() && old_size == Capacity()) {
        /** NUL終端を含めて再確保した文字列領域。 */
        char* const p = static_cast<char*>(m_Alloc->Realloc(m_Heap.data, Capacity() + 1u, cap + 1u, alignof(char), FSourceLoc::Current()));
        if (!p) return false;
        m_Heap.data = p;
        m_Heap.size = old_size;
        m_Heap.capacity = cap;
        m_Sso.remaining = 0x80;
        return true;
    }

    char* p = static_cast<char*>(m_Alloc->Alloc(cap + 1, alignof(char), FSourceLoc::Current()));
    if (!p) return false;  // OOM: 文字列を変更しない
    const char* old = Data();
    // NUL 含めてコピー
    for (usize i = 0; i <= old_size; ++i) p[i] = old[i];
    if (IsHeap()) m_Alloc->Free(m_Heap.data);
    m_Heap.data     = p;
    m_Heap.size     = old_size;
    m_Heap.capacity = cap;
    m_Sso.remaining = 0x80;  // ヒープフラグ
    return true;
}

/**
 * 容量を予約する (現容量より大きい場合のみ Grow する)。
 *
 * @param new_capacity 確保する最小容量。
 */
void FString::Reserve(usize new_capacity) noexcept {
    ACS_CHECKF(TryReserve(new_capacity), "FString::Reserve failed (cap=%zu)", new_capacity);
}

/**
 * 容量予約を試みる。確保に失敗したら文字列を変えず false を返す。
 *
 * @param new_capacity 確保する最小容量。
 * @return 予約済みまたは予約成功なら true、OOM なら false。
 */
bool FString::TryReserve(usize new_capacity) noexcept {
    return new_capacity <= Capacity() ? true : TryGrow(new_capacity);
}

/**
 * 文字列を追記する (容量不足なら幾何級数的に拡大、self-aliasing も安全に扱う)。
 *
 * @param v 追記するビュー。
 */
void FString::Append(FStringView v) noexcept {
    ACS_CHECKF(TryAppend(v), "FString::Append failed (add=%zu)", v.Size());
}

/**
 * 追記を試みる。拡張確保に失敗したら文字列を変えず false を返す。
 *
 * @param v 追記するビュー。
 * @return 成功なら true、OOM なら false。
 */
bool FString::TryAppend(FStringView v) noexcept {
    if (v.IsEmpty()) return true;

    /** 追記前の文字列長。 */
    const usize cur  = Size();

    /** 追記元ビューのバイト数。 */
    const usize vlen = v.Size();
    if (vlen > (~usize(0)) - cur || cur + vlen >= kHeapFlagBit) {
        return false;
    }

    /** 追記成功後の文字列長。 */
    const usize req  = cur + vlen;

    // self-aliasing 検出: v が自分のバッファ内容を指しているか (s += s /
    // s.Append(s.View()) / 自分の部分文字列の append)。Grow() は旧バッファを
    // Free して内容を新バッファへコピーするため、aliasing したまま後で v.Data()
    // を読むと use-after-free / overwritten-union 読みになる。
    /** 追記前の自領域先頭。 */
    const char* const old_data = Data();

    /** 追記元ビューの先頭。 */
    const char* const vd = v.Data();

    /** 追記前の自領域先頭アドレス。 */
    const uptr OldAddress = reinterpret_cast<uptr>(old_data);

    /** 追記元ビューの先頭アドレス。 */
    const uptr ViewAddress = reinterpret_cast<uptr>(vd);

    /** 追記元が現在の文字列内容から始まるかを表す値。 */
    const bool StartsInside = cur != 0u && ViewAddress >= OldAddress && ViewAddress - OldAddress < cur;

    /** 自己参照時の追記元位置。 */
    const usize voff = StartsInside ? static_cast<usize>(ViewAddress - OldAddress) : 0u;
    if (StartsInside && vlen > cur - voff) return false;

    /** 再確保後に追記元位置を復元する必要があるかを表す値。 */
    const bool aliases = StartsInside;

    if (req > Capacity()) {
        /** req 以上へ幾何級数で拡張する候補容量。 */
        usize n = Capacity() == 0 ? kSsoCapacity : Capacity();
        // 幾何級数拡大。n + n/2 + 1 が usize を wrap したら req で打ち切る
        // (異常な巨大 req でも under-allocate しない防御)。
        while (n < req) {
            /** 約1.5倍へ拡張した次の候補容量。 */
            const usize next = n + n / 2 + 1;
            if (next <= n) { n = req; break; }   // overflow → req に確定
            n = next;
        }
        if (!TryGrow(n)) return false;   // OOM: 文字列を変更しない (旧バッファも保持)
    }

    /** 追記時点の書き込み先バッファ。 */
    char* d = Data();
    // aliasing していたら Grow がコピー済みの新バッファ内同オフセットから読む。
    // v は content の部分文字列 (voff+vlen <= cur) なので src=[voff,voff+vlen) は
    // dst=[cur,req) と重ならない (src 終端 <= cur = dst 先頭) → 前方コピーで安全。
    /** 再確保後も有効な追記元位置。 */
    const char* src = aliases ? (d + voff) : vd;
    MemMove(d + cur, src, vlen);   // 非重なりだが防御的に MemMove
    d[req] = 0;
    if (IsHeap()) m_Heap.size = req;
    else          SetInlineLen(static_cast<u8>(req));
    return true;
}

/**
 * 1 文字を追記する (Append(FStringView) へ委譲)。
 *
 * @param c 追記する文字。
 */
void FString::Append(char c) noexcept {
    ACS_CHECKF(TryAppend(c), "FString::Append failed (single byte)");
}

/**
 * 1 文字の追記を試みる (TryAppend(FStringView) へ委譲)。
 *
 * @param c 追記する文字。
 * @return 成功なら true、OOM なら false。
 */
bool FString::TryAppend(char c) noexcept {
    /** 追記前の文字列長。 */
    const usize CurrentSize = Size();
    if (CurrentSize + 1u >= kHeapFlagBit) return false;
    if (CurrentSize == Capacity()) {
        /** 拡張後の文字列容量。 */
        usize NewCapacity = Capacity();

        /** 現容量の約半分に相当する増分。 */
        const usize Increment = NewCapacity / 2u + 1u;
        if (NewCapacity > (~usize(0)) - Increment) return false;
        NewCapacity += Increment;
        if (!TryGrow(NewCapacity)) return false;
    }

    /** 追記先の現在バッファ。 */
    char* const Destination = Data();
    Destination[CurrentSize] = c;
    Destination[CurrentSize + 1u] = '\0';
    if (IsHeap()) {
        m_Heap.size = CurrentSize + 1u;
    } else {
        SetInlineLen(static_cast<u8>(CurrentSize + 1u));
    }
    return true;
}

/**
 * printf 風フォーマットで追記する。
 *
 * @details vsnprintf(nullptr) で必要長を計算してから容量を確保し、本体へ書き込む。
 * @param fmt printf 形式の書式文字列。
 * @return 追記後の文字列サイズ (書式エラーまたは OOM の失敗時は 0、文字列は不変)。
 */
usize FString::AppendFormat(const char* fmt, ...) noexcept {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    const int needed = ::vsnprintf(nullptr, 0, fmt, ap);
    va_end(ap);
    if (needed < 0) { va_end(ap2); return 0; }
    const usize cur = Size();

    /** 書式展開で追加するバイト数。 */
    const usize Added = static_cast<usize>(needed);
    if (Added > (~usize(0)) - cur || cur + Added >= kHeapFlagBit || !TryReserve(cur + Added)) {
        va_end(ap2);
        return 0;
    }
    char* d = Data();
    const int wrote = ::vsnprintf(d + cur, static_cast<usize>(needed) + 1, fmt, ap2);
    va_end(ap2);
    if (wrote < 0) return 0;
    if (IsHeap()) m_Heap.size = cur + static_cast<usize>(wrote);
    else          SetInlineLen(static_cast<u8>(cur + static_cast<usize>(wrote)));
    return Size();
}

} // namespace acs
