// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FRelocatableAllocator 実装
// =============================================================================
#include "memory/RelocatableAllocator.h"
#include "memory/Allocator.h"   // FAllocator / AlignUp / IsPow2
#include "memory/Memory.h"      // DefaultAllocator / MemMove
#include "foundation/Assert.h"

namespace acs {

namespace {
/** 空きエントリリストの終端を表す番兵インデックス。 */
constexpr u32 kNoIndex = 0xFFFFFFFFu;
}

FRelocatableAllocator::~FRelocatableAllocator() noexcept { Shutdown(); }

TResult<void> FRelocatableAllocator::Init(usize capacity_bytes, u32 max_handles,
                                          FAllocator* backing) noexcept {
    if (m_Base) return ACS_ERR(Memory, 60, "FRelocatableAllocator: already initialized");
    if (capacity_bytes == 0 || max_handles == 0)
        return ACS_ERR(Memory, 61, "FRelocatableAllocator: invalid capacity/max_handles");

    m_Backing = backing ? backing : &DefaultAllocator();

    m_Base = static_cast<u8*>(m_Backing->Alloc(capacity_bytes, 16, FSourceLoc::Current()));
    if (!m_Base) return ACS_ERR(Memory, 62, "FRelocatableAllocator: arena alloc failed");

    m_Entries = static_cast<Entry*>(
        m_Backing->Alloc(sizeof(Entry) * max_handles, alignof(Entry), FSourceLoc::Current()));
    if (!m_Entries) {
        m_Backing->Free(m_Base); m_Base = nullptr;
        return ACS_ERR(Memory, 63, "FRelocatableAllocator: handle table alloc failed");
    }
    m_Order = static_cast<u32*>(
        m_Backing->Alloc(sizeof(u32) * max_handles, alignof(u32), FSourceLoc::Current()));
    if (!m_Order) {
        m_Backing->Free(m_Entries); m_Entries = nullptr;
        m_Backing->Free(m_Base);    m_Base = nullptr;
        return ACS_ERR(Memory, 64, "FRelocatableAllocator: order array alloc failed");
    }

    m_Capacity   = capacity_bytes;
    m_MaxHandles = max_handles;
    m_Cursor     = 0;
    m_LiveBytes  = 0;
    m_LiveCount  = 0;

    // 空きエントリリストを構築 (generation は 0 始まり、Alloc で ++ して 1 から払い出す)。
    for (u32 i = 0; i < max_handles; ++i) {
        m_Entries[i].ptr        = nullptr;
        m_Entries[i].size       = 0;
        m_Entries[i].align      = 0;
        m_Entries[i].generation = 0;
        m_Entries[i].live       = false;
        m_Entries[i].next_free  = (i + 1 < max_handles) ? (i + 1) : kNoIndex;
    }
    m_FreeHead = 0;
    return Ok();
}

void FRelocatableAllocator::Shutdown() noexcept {
    if (m_Backing) {
        if (m_Order)   { m_Backing->Free(m_Order);   m_Order = nullptr; }
        if (m_Entries) { m_Backing->Free(m_Entries); m_Entries = nullptr; }
        if (m_Base)    { m_Backing->Free(m_Base);    m_Base = nullptr; }
    }
    m_Capacity = 0; m_Cursor = 0; m_LiveBytes = 0;
    m_MaxHandles = 0; m_FreeHead = kNoIndex; m_LiveCount = 0;
    m_Backing = nullptr;
}

bool FRelocatableAllocator::ResolveEntry(FRelocHandle h, Entry*& out) const noexcept {
    if (!m_Base || h.generation == 0u || h.index >= m_MaxHandles) return false;
    Entry& e = m_Entries[h.index];
    if (!e.live || e.generation != h.generation) return false;
    out = &e;
    return true;
}

FRelocHandle FRelocatableAllocator::Alloc(usize size, usize alignment) noexcept {
    if (!m_Base || size == 0) return {};
    if (alignment < 1) alignment = 1;
    if (!IsPow2(alignment)) return {};
    if (size > 0xFFFFFFFFull || alignment > 0xFFFFFFFFull) return {};
    if (m_FreeHead == kNoIndex) return {};        // ハンドル枯渇

    usize off = AlignUp(m_Cursor, alignment);
    if (off + size > m_Capacity) {
        // 末尾に入らない。詰めれば入る (生存量 + 余裕 <= 容量) なら Compact してから再試行。
        if (m_LiveBytes + size + alignment <= m_Capacity) {
            Compact();
            off = AlignUp(m_Cursor, alignment);
        }
        if (off + size > m_Capacity) return {};   // 真に容量不足
    }

    const u32 idx = m_FreeHead;
    Entry& e = m_Entries[idx];
    m_FreeHead = e.next_free;

    u32 gen = e.generation + 1u;
    if (gen == 0u) gen = 1u;                       // wrap 時 0 (無効) を避ける

    e.ptr        = m_Base + off;
    e.size       = static_cast<u32>(size);
    e.align      = static_cast<u32>(alignment);
    e.generation = gen;
    e.live       = true;

    m_Cursor     = off + size;
    m_LiveBytes += size;
    ++m_LiveCount;
    return FRelocHandle{ idx, gen };
}

void FRelocatableAllocator::Free(FRelocHandle h) noexcept {
    Entry* e = nullptr;
    if (!ResolveEntry(h, e)) {
        ACS_ASSERT(h.generation == 0 || true);     // 二重 free / 無効ハンドルは静かに無視
        return;
    }
    e->live = false;
    m_LiveBytes -= e->size;
    --m_LiveCount;
    // generation は据え置き。再利用 (Alloc) 時に ++ して旧ハンドルを無効化する。
    e->next_free = m_FreeHead;
    m_FreeHead   = h.index;
    // ブロックは Compact まで garbage として残る (high-water は下がらない)。
}

void* FRelocatableAllocator::Resolve(FRelocHandle h) const noexcept {
    Entry* e = nullptr;
    return ResolveEntry(h, e) ? e->ptr : nullptr;
}

usize FRelocatableAllocator::SizeOf(FRelocHandle h) const noexcept {
    Entry* e = nullptr;
    return ResolveEntry(h, e) ? e->size : 0u;
}

bool FRelocatableAllocator::ValidateHandle(FRelocHandle h) const noexcept {
    Entry* e = nullptr;
    return ResolveEntry(h, e);
}

usize FRelocatableAllocator::Compact() noexcept {
    if (!m_Base) return 0;
    const usize old_cursor = m_Cursor;

    // 生存エントリを集める。
    u32 cnt = 0;
    for (u32 i = 0; i < m_MaxHandles; ++i) {
        if (m_Entries[i].live) m_Order[cnt++] = i;
    }

    // payload アドレス昇順に挿入ソート。bump 確保由来でほぼ整列済なので実質 O(n)。
    // アドレス順に処理しないとスライド先が未処理ブロックを踏み潰すため、整列は必須。
    for (u32 a = 1; a < cnt; ++a) {
        const u32 key = m_Order[a];
        u8* const kp  = m_Entries[key].ptr;
        u32 b = a;
        while (b > 0 && m_Entries[m_Order[b - 1]].ptr > kp) {
            m_Order[b] = m_Order[b - 1];
            --b;
        }
        m_Order[b] = key;
    }

    // 前方へスライドして gap を詰める。dst <= src なので MemMove (overlap 安全)。
    usize write = 0;
    for (u32 k = 0; k < cnt; ++k) {
        Entry& e = m_Entries[m_Order[k]];
        const usize off = AlignUp(write, e.align);
        u8* const dst = m_Base + off;
        if (dst != e.ptr) {
            MemMove(dst, e.ptr, e.size);
            e.ptr = dst;
        }
        write = off + e.size;
    }
    m_Cursor = write;
    return old_cursor - m_Cursor;   // 回収バイト
}

} // namespace acs
