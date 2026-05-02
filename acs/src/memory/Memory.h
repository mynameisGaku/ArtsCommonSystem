// ACS Memory — Global allocator access + low-level mem helpers.
#pragma once

#include "foundation/Types.h"
#include "foundation/Compiler.h"
#include "memory/Allocator.h"

namespace acs {

// Returns the engine-wide default allocator. Initialized lazily on first use.
Allocator& DefaultAllocator() noexcept;

// Replace the default allocator. Caller owns the lifetime; pass nullptr to
// restore the built-in SystemAllocator. Calling concurrently with allocations
// is undefined.
void SetDefaultAllocator(Allocator* a) noexcept;

// ---- Raw memory primitives (avoid <cstring> in headers) -------------------
void MemCopy(void* dst, const void* src, usize n) noexcept;
void MemMove(void* dst, const void* src, usize n) noexcept;
void MemSet (void* dst, int   value,    usize n) noexcept;
int  MemCmp (const void* a, const void* b, usize n) noexcept;

} // namespace acs
