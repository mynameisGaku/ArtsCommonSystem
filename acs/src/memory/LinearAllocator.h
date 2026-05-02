// ACS Memory — Linear (bump) allocator backed by a single contiguous block.
//
// Thread-safe: cursor advance is an atomic CAS loop. Free is a no-op; the
// only way to release memory is Reset(), which is NOT safe to call while
// other threads are allocating from this allocator.
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class LinearAllocator final : public Allocator {
public:
    // Pre-allocates `capacity` bytes from `backing`. If `backing` is null,
    // bytes are obtained from the engine default allocator and freed in dtor.
    LinearAllocator(usize capacity, Allocator* backing = nullptr) noexcept;
    ~LinearAllocator() noexcept override;

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override; // no-op

    // Reset cursor to 0. NOT thread-safe with concurrent Alloc.
    void Reset() noexcept;

    u64 BytesAllocated() const noexcept override { return _used.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    u64 Capacity()       const noexcept          { return _capacity; }
    const char* Name()   const noexcept override { return "Linear"; }

private:
    u8*           _base     = nullptr;
    u64           _capacity = 0;
    Allocator*    _backing  = nullptr;
    bool          _owns_backing = false;
    Atomic<u64>   _used {0};
    mutable Atomic<u64> _peak {0};
};

} // namespace acs
