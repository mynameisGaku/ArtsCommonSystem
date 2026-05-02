#include "memory/LinearAllocator.h"
#include "memory/Memory.h"

namespace acs {

LinearAllocator::LinearAllocator(usize capacity, Allocator* backing) noexcept
    : _capacity(capacity)
    , _backing(backing ? backing : &DefaultAllocator())
    , _owns_backing(false) {
    _base = static_cast<u8*>(_backing->Alloc(capacity, kDefaultAlignment, SourceLoc::Current()));
}

LinearAllocator::~LinearAllocator() noexcept {
    if (_base) _backing->Free(_base);
}

void* LinearAllocator::Alloc(usize size, usize alignment, SourceLoc /*loc*/) noexcept {
    if (size == 0 || !_base) return nullptr;
    if (alignment < 1) alignment = 1;
    while (true) {
        u64 cur = _used.Load(MemoryOrder::Relaxed);
        u64 base_addr = reinterpret_cast<u64>(_base);
        u64 aligned   = AlignUp(base_addr + cur, alignment) - base_addr;
        u64 next      = aligned + size;
        if (next > _capacity) return nullptr;
        u64 expected = cur;
        if (_used.CompareExchange(expected, next)) {
            // Update peak.
            u64 peak = _peak.Load(MemoryOrder::Relaxed);
            while (next > peak && !_peak.CompareExchange(peak, next)) {}
            return _base + aligned;
        }
    }
}

void LinearAllocator::Free(void* /*ptr*/) noexcept {
    // Linear allocator does not support per-object free.
}

void LinearAllocator::Reset() noexcept {
    _used.Store(0, MemoryOrder::Release);
}

} // namespace acs
