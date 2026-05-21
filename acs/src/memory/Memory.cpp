// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — グローバルアロケータ実装 + Allocator::Realloc デフォルト
// -----------------------------------------------------------------------------
// CRT (memcpy/memmove/memset/memcmp) は SSE/AVX で最適化されているため、
// そのまま委譲する。自前実装するメリットは皆無に近い。
// =============================================================================
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cstring>

namespace acs {

namespace {
// プロセス全体のシステムアロケータ（起動時から有効）
SystemAllocator g_system_allocator;
// 現在のデフォルト。起動時は SystemAllocator を指す。
Allocator*      g_default = &g_system_allocator;
}

Allocator& DefaultAllocator() noexcept { return *g_default; }

void SetDefaultAllocator(Allocator* a) noexcept {
    g_default = a ? a : &g_system_allocator;
}

// ---- 薄い CRT ラッパ ----------------------------------------------------
void MemCopy(void* dst, const void* src, usize n) noexcept { ::memcpy(dst, src, n); }
void MemMove(void* dst, const void* src, usize n) noexcept { ::memmove(dst, src, n); }
void MemSet (void* dst, int v, usize n)            noexcept { ::memset(dst, v, n); }
int  MemCmp (const void* a, const void* b, usize n) noexcept { return ::memcmp(a, b, n); }

// =============================================================================
// Allocator::Realloc — デフォルト実装（Alloc + MemCopy + Free）
// -----------------------------------------------------------------------------
// 派生アロケータがオーバーライドしない場合のフォールバック。
// 効率的な realloc を持つ実装（HeapReAlloc 等）はオーバーライドすべき。
// =============================================================================
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
