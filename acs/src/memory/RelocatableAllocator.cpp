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

/** 絶対アドレス基準で整列したアリーナ内オフセットを安全に計算する。 */
bool TryCalculateAlignedOffset(const u8* Base, usize Cursor, usize Alignment,
                               usize Capacity, usize& Offset) noexcept
{
    if (!Base || !IsPow2(Alignment) || Cursor > Capacity) return false;

    const uptr BaseAddress = reinterpret_cast<uptr>(Base);
    if (Cursor > (~uptr(0)) - BaseAddress || BaseAddress + Cursor > (~uptr(0)) - (Alignment - 1u)) {
        return false;
    }

    const uptr AlignedAddress = AlignUp(BaseAddress + Cursor, Alignment);
    Offset = static_cast<usize>(AlignedAddress - BaseAddress);
    return Offset <= Capacity;
}
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

    if (max_handles > (~usize(0)) / sizeof(Entry) || max_handles > (~usize(0)) / sizeof(u32)) {
        m_Backing->Free(m_Base);
        m_Base = nullptr;
        return ACS_ERR(Memory, 63, "FRelocatableAllocator: handle table size overflow");
    }

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

    usize off = 0u;
    if (!TryCalculateAlignedOffset(m_Base, m_Cursor, alignment, m_Capacity, off)) return {};
    if (off > m_Capacity || size > m_Capacity - off) {
        // 末尾に入らない。詰めれば入る (生存量 + 余裕 <= 容量) なら Compact してから再試行。
        if (m_LiveBytes <= m_Capacity && size <= m_Capacity - m_LiveBytes) {
            Compact();
            if (!TryCalculateAlignedOffset(m_Base, m_Cursor, alignment, m_Capacity, off)) return {};
        }
        if (off > m_Capacity || size > m_Capacity - off) return {};   // 真に容量不足
    }

    const u32 idx = m_FreeHead;
    Entry& e = m_Entries[idx];
    m_FreeHead = e.next_free;

    u64 gen = m_NextGeneration++;
    if (gen == 0u) {
        gen = m_NextGeneration++;
    }

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
        // 二重解放や期限切れハンドルは状態を変更せず拒否する。
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
        usize off = 0u;
        if (!TryCalculateAlignedOffset(m_Base, write, e.align, m_Capacity, off) ||
            off > m_Capacity || e.size > m_Capacity - off) {
            return 0u;
        }
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
