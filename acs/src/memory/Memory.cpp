// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — グローバルアロケータ実装 + FAllocator::Realloc デフォルト
// -----------------------------------------------------------------------------
// CRT (memcpy/memmove/memset/memcmp) は SSE/AVX で最適化されているため、
// そのまま委譲する。自前実装するメリットは皆無に近い。
// =============================================================================
#include "memory/Memory.h"
#include "memory/SystemAllocator.h"

#include <cstring>

namespace acs {

namespace {
/** プロセス全体のシステムアロケータ (起動時から有効、SetDefaultAllocator の fallback)。 */
FSystemAllocator g_system_allocator;

/** 現在のデフォルトアロケータを指すポインタ (起動時は g_system_allocator)。 */
FAllocator*      g_default = &g_system_allocator;
}

FAllocator& DefaultAllocator() noexcept { return *g_default; }

void SetDefaultAllocator(FAllocator* a) noexcept {
    g_default = a ? a : &g_system_allocator;
}

void MemCopy(void* dst, const void* src, usize n) noexcept { ::memcpy(dst, src, n); }
void MemMove(void* dst, const void* src, usize n) noexcept { ::memmove(dst, src, n); }
void MemSet (void* dst, int v, usize n)            noexcept { ::memset(dst, v, n); }
int  MemCmp (const void* a, const void* b, usize n) noexcept { return ::memcmp(a, b, n); }

/**
 * Realloc のデフォルト実装 (Alloc + MemCopy + Free)。
 *
 * @details
 * 派生アロケータが override しない場合のフォールバック。new_size==0 なら Free して
 * nullptr を返す。新規確保に失敗したら旧領域を保持したまま nullptr を返す (旧データは無傷)。
 * 効率的な in-place realloc を持つ実装 (FTlsfAllocator 等) は override すべき。
 * @param ptr 既存の確保 (nullptr なら新規 Alloc 相当)。
 * @param old_size 旧サイズ (コピーするバイト数の決定に使う)。
 * @param new_size 新サイズ (0 なら解放のみ)。
 * @param alignment 新領域のアライメント。
 * @param loc 診断用の呼び出し位置。
 * @return 新しい確保 (失敗時や new_size==0 のとき nullptr)。
 */
void* FAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                         usize alignment, FSourceLoc loc) noexcept {
    if (new_size == 0) { Free(ptr); return nullptr; }
    void* const p = Alloc(new_size, alignment, loc);
    if (!p) return nullptr;
    if (ptr) {
        const usize copy = old_size < new_size ? old_size : new_size;
        MemCopy(p, ptr, copy);
        Free(ptr);
    }
    return p;
}

} // namespace acs
