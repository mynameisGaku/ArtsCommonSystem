#include "memory/ArenaAllocator.h"
#include "memory/Memory.h"
#include "threading/ScopedLock.h"

namespace acs {

ArenaAllocator::ArenaAllocator(usize page_size, Allocator* backing) noexcept
    : _backing(backing ? backing : &DefaultAllocator())
    , _page_size(page_size) {}

ArenaAllocator::~ArenaAllocator() noexcept {
    Reset(/*release*/ true);
}

ArenaAllocator::Page* ArenaAllocator::AllocPage(usize size) noexcept {
    usize total = sizeof(Page) + size + 64; // 64 = alignment headroom
    void* raw = _backing->Alloc(total, alignof(Page), SourceLoc::Current());
    if (!raw) return nullptr;
    auto* p = static_cast<Page*>(raw);
    p->next = nullptr;
    p->base = reinterpret_cast<u8*>(AlignUp(static_cast<u8*>(raw) + sizeof(Page), 64));
    p->size = size;
    p->used.Store(0, MemoryOrder::Release);
    return p;
}

void* ArenaAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    if (alignment < 1) alignment = 1;

    while (true) {
        Page* p = _current.Load(MemoryOrder::Acquire);
        if (p) {
            u64 cur = p->used.Load(MemoryOrder::Relaxed);
            u64 base_addr = reinterpret_cast<u64>(p->base);
            u64 aligned   = AlignUp(base_addr + cur, alignment) - base_addr;
            u64 next      = aligned + size;
            if (next <= p->size) {
                u64 expected = cur;
                if (p->used.CompareExchange(expected, next)) {
                    u64 b = _bytes.FetchAdd(size) + size;
                    u64 pk = _peak.Load(MemoryOrder::Relaxed);
                    while (b > pk && !_peak.CompareExchange(pk, b)) {}
                    return p->base + aligned;
                }
                continue; // race lost, retry
            }
        }

        // Need a new page — grow under lock.
        ScopedLock lk(_grow_lock);
        // Re-check: another thread may have grown for us.
        Page* nowp = _current.Load(MemoryOrder::Acquire);
        if (nowp != p) continue;
        usize ps = size > _page_size ? size : _page_size;
        Page* np = AllocPage(ps);
        if (!np) return nullptr;
        np->next = _pages;
        _pages = np;
        _current.Store(np, MemoryOrder::Release);
        // loop back to attempt allocation in the new page
    }
}

void ArenaAllocator::Free(void* /*ptr*/) noexcept {}

void ArenaAllocator::Reset(bool release_pages) noexcept {
    ScopedLock lk(_grow_lock);
    if (release_pages) {
        Page* p = _pages;
        while (p) {
            Page* nx = p->next;
            _backing->Free(p);
            p = nx;
        }
        _pages = nullptr;
        _current.Store(nullptr, MemoryOrder::Release);
    } else {
        for (Page* p = _pages; p; p = p->next) p->used.Store(0, MemoryOrder::Release);
        _current.Store(_pages, MemoryOrder::Release);
    }
    _bytes.Store(0, MemoryOrder::Release);
}

} // namespace acs
