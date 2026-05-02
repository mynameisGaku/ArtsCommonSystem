// ACS Memory — Page-backed arena allocator.
//
// Like LinearAllocator but grows by allocating additional fixed-size pages
// from the backing allocator when the current page is exhausted. Reset()
// rolls back the cursor and optionally retains pages for reuse.
//
// Thread-safe: a Mutex protects the page list growth path; per-page bumps use
// an atomic cursor.
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"

namespace acs {

class ArenaAllocator final : public Allocator {
public:
    // `page_size` is the size of each backing page (>= largest expected alloc).
    ArenaAllocator(usize page_size = 64 * 1024,
                   Allocator* backing = nullptr) noexcept;
    ~ArenaAllocator() noexcept override;

    ArenaAllocator(const ArenaAllocator&) = delete;
    ArenaAllocator& operator=(const ArenaAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override; // no-op

    // Recycle all allocations. Pages are retained for reuse; pass `release=true`
    // to free them back to the backing allocator.
    void Reset(bool release_pages = false) noexcept;

    u64 BytesAllocated() const noexcept override { return _bytes.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "Arena"; }

private:
    struct Page {
        Page*       next;
        u8*         base;
        u64         size;
        Atomic<u64> used;
    };

    Page* AllocPage(usize size) noexcept;

    Allocator*    _backing  = nullptr;
    usize         _page_size = 0;
    Atomic<Page*> _current  {nullptr};
    Page*         _pages    = nullptr; // singly linked, head of full+free list
    Mutex         _grow_lock;
    Atomic<u64>   _bytes {0};
    mutable Atomic<u64> _peak  {0};
};

} // namespace acs
