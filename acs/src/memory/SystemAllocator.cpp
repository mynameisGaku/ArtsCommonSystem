// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — SystemAllocator 実装
// -----------------------------------------------------------------------------
// HeapAlloc は最大で MEMORY_ALLOCATION_ALIGNMENT (16B on x64) しか保証しない。
// 任意アライメントを得るため、要求サイズ + アライメント余裕 + ヘッダ分を
// 確保し、ポインタを切り上げて返す。Free 時は元ポインタをヘッダから取得。
// =============================================================================
#include "memory/SystemAllocator.h"
#include "memory/Memory.h"
#include "foundation/Platform.h"

namespace acs {

namespace {

// アラインヘッダ: void* (元ポインタ) + usize (確保サイズ)
constexpr usize kHeaderSize = sizeof(void*) + sizeof(usize);

// アライン確保。
// 戻り値: アライン済みユーザポインタ
// actual_size: 実際にヒープから取った総バイト数（解放時に統計を引くために必要）
void* AlignedAlloc(usize size, usize alignment, usize& actual_size) noexcept {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    usize raw_size = size + alignment + kHeaderSize;
    void* raw = ::HeapAlloc(::GetProcessHeap(), 0, raw_size);
    if (!raw) { actual_size = 0; return nullptr; }
    // ヘッダ分とアライメント分を確保した先で切り上げる
    uptr  base    = reinterpret_cast<uptr>(raw) + kHeaderSize;
    uptr  aligned = (base + alignment - 1) & ~(alignment - 1);
    void** stash  = reinterpret_cast<void**>(aligned) - 1;          // ユーザポインタの 8B 手前
    usize* sstore = reinterpret_cast<usize*>(stash) - 1;            // さらに 8B 手前
    *stash  = raw;       // 元ポインタを保存（Free で HeapFree に渡すため）
    *sstore = raw_size;  // サイズを保存（統計用）
    actual_size = raw_size;
    return reinterpret_cast<void*>(aligned);
}

// アライン解放。
// freed_size: 解放したバイト数を返す（統計用）
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
    // 統計更新（atomic）
    u64 cur = _bytes.FetchAdd(actual) + actual;
    // ピーク更新は CAS で（cur > peak の間ループ）
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

// HeapReAlloc はアラインヘッダ方式と相性が悪いので、デフォルト実装を使う
void* SystemAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                               usize alignment, SourceLoc loc) noexcept {
    return Allocator::Realloc(ptr, old_size, new_size, alignment, loc);
}

} // namespace acs
