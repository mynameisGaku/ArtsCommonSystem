#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cstring>

namespace acs {

namespace {
SystemAllocator g_system_allocator;
Allocator*      g_default = &g_system_allocator;
}

Allocator& DefaultAllocator() noexcept { return *g_default; }

void SetDefaultAllocator(Allocator* a) noexcept {
    g_default = a ? a : &g_system_allocator;
}

void MemCopy(void* dst, const void* src, usize n) noexcept { ::memcpy(dst, src, n); }
void MemMove(void* dst, const void* src, usize n) noexcept { ::memmove(dst, src, n); }
void MemSet (void* dst, int v, usize n)            noexcept { ::memset(dst, v, n); }
int  MemCmp (const void* a, const void* b, usize n) noexcept { return ::memcmp(a, b, n); }

// Default Realloc — implemented here so it can use MemCopy.
void* Allocator::Realloc(void* ptr, usize old_size, usize new_size,
                         usize alignment, SourceLoc loc) noexcept {
    if (new_size == 0) { Free(ptr); return nullptr; }
    void* p = Alloc(new_size, alignment, loc);
    if (!p) return nullptr;
    if (ptr) {
        usize copy = old_size < new_size ? old_size : new_size;
        MemCopy(p, ptr, copy);
        Free(ptr);
    }
    return p;
}

} // namespace acs
