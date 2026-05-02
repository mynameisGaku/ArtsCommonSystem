// ACS Memory — System allocator (Win32 process heap).
//
// Thread-safe: HeapAlloc / HeapFree from the process heap are inherently
// serialized by the OS (HEAP_NO_SERIALIZE is NOT used).
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class SystemAllocator final : public Allocator {
public:
    SystemAllocator() noexcept = default;
    ~SystemAllocator() noexcept override = default;

    void* Alloc  (usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free   (void* ptr)                                  noexcept override;
    void* Realloc(void* ptr, usize old_size, usize new_size,
                  usize alignment, SourceLoc loc)             noexcept override;

    u64 BytesAllocated() const noexcept override { return _bytes.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "System"; }

private:
    mutable Atomic<u64> _bytes {0};
    mutable Atomic<u64> _peak  {0};
};

} // namespace acs
