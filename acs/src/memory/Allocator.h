// ACS Memory — Allocator interface.
//
// Every container, smart pointer, and engine subsystem that performs dynamic
// allocation goes through an Allocator pointer. This makes it trivial to
// inject pools, arenas, or sandbox allocators without re-templating call sites.
//
// All concrete allocators MUST be thread-safe by contract.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "foundation/SourceLoc.h"

namespace acs {

inline constexpr usize kDefaultAlignment = alignof(void*) > 8 ? alignof(void*) : 16;

class Allocator {
public:
    virtual ~Allocator() noexcept = default;

    // Allocate `size` bytes aligned to `alignment` (must be a power of two).
    // Returns nullptr on failure. `loc` is for diagnostics only.
    virtual void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept = 0;

    // Free a previously-allocated pointer. nullptr is a no-op.
    virtual void Free(void* ptr) noexcept = 0;

    // Reallocate. The default implementation falls back to Alloc + memcpy + Free.
    virtual void* Realloc(void* ptr, usize old_size, usize new_size,
                          usize alignment, SourceLoc loc) noexcept;

    virtual u64 BytesAllocated() const noexcept { return 0; }
    virtual u64 PeakBytes()      const noexcept { return 0; }
    virtual const char* Name()   const noexcept { return "Allocator"; }

    // Convenience overloads with defaults.
    ACS_FORCEINLINE void* Alloc(usize size, SourceLoc loc = SourceLoc::Current()) noexcept {
        return Alloc(size, kDefaultAlignment, loc);
    }
};

ACS_FORCEINLINE bool IsPow2(usize v) noexcept { return v != 0 && (v & (v - 1)) == 0; }

ACS_FORCEINLINE usize AlignUp(usize n, usize a) noexcept {
    return (n + (a - 1)) & ~(a - 1);
}

ACS_FORCEINLINE void* AlignUp(void* p, usize a) noexcept {
    return reinterpret_cast<void*>(AlignUp(reinterpret_cast<uptr>(p), a));
}

} // namespace acs
