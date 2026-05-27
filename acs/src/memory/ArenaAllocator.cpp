// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FArenaAllocator 実装
// -----------------------------------------------------------------------------
// 通常確保はアトミック CAS 1 回で完了する。ページが満杯になった時だけ
// FMutex で排他して新ページを確保する。
// =============================================================================
#include "memory/ArenaAllocator.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"

namespace acs {

FArenaAllocator::FArenaAllocator(usize page_size, FAllocator* backing) noexcept
    : _backing(backing ? backing : &DefaultAllocator())
    , _page_size(page_size) {}

FArenaAllocator::~FArenaAllocator() noexcept {
    Reset(/*release*/ true);
}

// 新ページ確保（ヘッダ + データ + 64B 整列の余裕を 1 回で取る）
FArenaAllocator::Page* FArenaAllocator::AllocPage(usize size) noexcept {
    usize total = sizeof(Page) + size + 64;
    void* raw = _backing->Alloc(total, alignof(Page), FSourceLoc::Current());
    if (!raw) return nullptr;
    auto* p = static_cast<Page*>(raw);
    p->next = nullptr;
    // データ領域はヘッダ直後を 64B 整列した位置から始める（SIMD 安全）
    p->base = reinterpret_cast<u8*>(AlignUp(static_cast<u8*>(raw) + sizeof(Page), 64));
    p->size = size;
    p->used.Store(0, EMemoryOrder::Release);
    return p;
}

// 確保
void* FArenaAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    if (alignment < 1) alignment = 1;

    while (true) {
        Page* p = _current.Load(EMemoryOrder::Acquire);
        if (p) {
            // 現在ページに収まるか確認
            u64 cur = p->used.Load(EMemoryOrder::Relaxed);
            u64 base_addr = reinterpret_cast<u64>(p->base);
            u64 aligned   = AlignUp(base_addr + cur, alignment) - base_addr;
            u64 next      = aligned + size;
            if (next <= p->size) {
                // CAS でカーソルを進める
                u64 expected = cur;
                if (p->used.CompareExchange(expected, next)) {
                    // ピーク値を CAS で更新
                    u64 b = _bytes.FetchAdd(size) + size;
                    u64 pk = _peak.Load(EMemoryOrder::Relaxed);
                    while (b > pk && !_peak.CompareExchange(pk, b)) {}
                    return p->base + aligned;
                }
                continue;  // 競合 — 同ページで再試行
            }
        }

        // 新ページが必要 — Grow ロックを取る
        FScopedLock lk(_grow_lock);
        // ロック取得中に他スレッドが既に Grow している可能性をチェック
        Page* nowp = _current.Load(EMemoryOrder::Acquire);
        if (nowp != p) continue;
        // 要求サイズが page_size より大きければ専用ページを作る
        usize ps = size > _page_size ? size : _page_size;
        Page* np = AllocPage(ps);
        if (!np) return nullptr;
        np->next = _pages;
        _pages = np;
        _current.Store(np, EMemoryOrder::Release);
        // 新ページに対して Alloc を再試行
    }
}

void FArenaAllocator::Free(void* /*ptr*/) noexcept {
    // 個別解放はサポートしない（Reset で全体破棄）
}

// 巻き戻し or 全解放
void FArenaAllocator::Reset(bool release_pages) noexcept {
    FScopedLock lk(_grow_lock);
    if (release_pages) {
        // 全ページを backing に返却
        Page* p = _pages;
        while (p) {
            Page* nx = p->next;
            _backing->Free(p);
            p = nx;
        }
        _pages = nullptr;
        _current.Store(nullptr, EMemoryOrder::Release);
    } else {
        // ページを保持してカーソルだけ巻き戻し
        for (Page* p = _pages; p; p = p->next) p->used.Store(0, EMemoryOrder::Release);
        _current.Store(_pages, EMemoryOrder::Release);
    }
    _bytes.Store(0, EMemoryOrder::Release);
}

} // namespace acs
