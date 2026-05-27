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
    : m_Backing(backing ? backing : &DefaultAllocator())
    , m_PageSize(page_size) {}

FArenaAllocator::~FArenaAllocator() noexcept {
    Reset(/*release*/ true);
}

// 新ページ確保（ヘッダ + データ + 64B 整列の余裕を 1 回で取る）
FArenaAllocator::Page* FArenaAllocator::AllocPage(usize size) noexcept {
    usize total = sizeof(Page) + size + 64;
    void* raw = m_Backing->Alloc(total, alignof(Page), FSourceLoc::Current());
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
        Page* p = m_Current.Load(EMemoryOrder::Acquire);
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
                    u64 b = m_Bytes.FetchAdd(size) + size;
                    u64 pk = m_Peak.Load(EMemoryOrder::Relaxed);
                    while (b > pk && !m_Peak.CompareExchange(pk, b)) {}
                    return p->base + aligned;
                }
                continue;  // 競合 — 同ページで再試行
            }
        }

        // 新ページが必要 — Grow ロックを取る
        FScopedLock lk(m_GrowLock);
        // ロック取得中に他スレッドが既に Grow している可能性をチェック
        Page* nowp = m_Current.Load(EMemoryOrder::Acquire);
        if (nowp != p) continue;
        // 要求サイズが page_size より大きければ専用ページを作る
        usize ps = size > m_PageSize ? size : m_PageSize;
        Page* np = AllocPage(ps);
        if (!np) return nullptr;
        np->next = m_Pages;
        m_Pages = np;
        m_Current.Store(np, EMemoryOrder::Release);
        // 新ページに対して Alloc を再試行
    }
}

void FArenaAllocator::Free(void* /*ptr*/) noexcept {
    // 個別解放はサポートしない（Reset で全体破棄）
}

// 巻き戻し or 全解放
void FArenaAllocator::Reset(bool release_pages) noexcept {
    FScopedLock lk(m_GrowLock);
    if (release_pages) {
        // 全ページを backing に返却
        Page* p = m_Pages;
        while (p) {
            Page* nx = p->next;
            m_Backing->Free(p);
            p = nx;
        }
        m_Pages = nullptr;
        m_Current.Store(nullptr, EMemoryOrder::Release);
    } else {
        // ページを保持してカーソルだけ巻き戻し
        for (Page* p = m_Pages; p; p = p->next) p->used.Store(0, EMemoryOrder::Release);
        m_Current.Store(m_Pages, EMemoryOrder::Release);
    }
    m_Bytes.Store(0, EMemoryOrder::Release);
}

} // namespace acs
