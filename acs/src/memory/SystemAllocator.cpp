#include "memory/SystemAllocator.h"
#include "memory/Memory.h"
#include "foundation/Platform.h"

namespace acs {

namespace {

// Aligned allocation. We over-allocate by `alignment + sizeof(void*)` bytes,
// then store the original pointer in the gap right before the returned address.
constexpr usize kHeaderSize = sizeof(void*) + sizeof(usize);

void* AlignedAlloc(usize size, usize alignment, usize& actual_size) noexcept {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    usize raw_size = size + alignment + kHeaderSize;
    void* raw = ::HeapAlloc(::GetProcessHeap(), 0, raw_size);
    if (!raw) { actual_size = 0; return nullptr; }
    uptr  base    = reinterpret_cast<uptr>(raw) + kHeaderSize;
    uptr  aligned = (base + alignment - 1) & ~(alignment - 1);
    void** stash  = reinterpret_cast<void**>(aligned) - 1;
    usize* sstore = reinterpret_cast<usize*>(stash) - 1;
    *stash  = raw;
    *sstore = raw_size;
    actual_size = raw_size;
    return reinterpret_cast<void*>(aligned);
}

void AlignedFree(void* p, usize& freed_size) noexcept {
    if (!p) { freed_size = 0; return; }
    void** stash  = reinterpret_cast<void**>(p) - 1;
    usize* sstore = reinterpret_cast<usize*>(stash) - 1;
    freed_size = *sstore;
    ::HeapFree(::GetProcessHeap(), 0, *stash);
}

} // namespace

void* SystemAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    usize actual = 0;
    void* p = AlignedAlloc(size, alignment, actual);
    if (!p) return nullptr;
    u64 cur = _bytes.FetchAdd(actual) + actual;
    u64 peak = _peak.Load(MemoryOrder::Relaxed);
    while (cur > peak && !_peak.CompareExchange(peak, cur)) {}
    return p;
}

void SystemAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    usize freed = 0;
    AlignedFree(ptr, freed);
    _bytes.FetchSub(freed);
}

void* SystemAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                               usize alignment, SourceLoc loc) noexcept {
    // Heap supports realloc but not for our aligned wrapping — fall back to
    // the base implementation (alloc + memcpy + free).
    return Allocator::Realloc(ptr, old_size, new_size, alignment, loc);
}

} // namespace acs
