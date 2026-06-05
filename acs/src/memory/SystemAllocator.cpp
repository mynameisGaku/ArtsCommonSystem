// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FSystemAllocator 実装
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

/** アラインヘッダのサイズ: void* (元ポインタ退避) + usize (確保サイズ退避)。 */
constexpr usize kHeaderSize = sizeof(void*) + sizeof(usize);

/**
 * 任意アライメントで確保する (ヘッダ退避方式)。
 *
 * @details
 * size + alignment + ヘッダ分をまとめて HeapAlloc し、ユーザポインタを境界へ切り上げる。
 * ユーザポインタの手前に元ポインタと総確保サイズを退避し、AlignedFree が回収する。
 * 加算オーバーフロー (過小確保→OOB) を検出したら nullptr。
 * @param size 要求バイト数。
 * @param alignment 要求アライメント (sizeof(void*) 未満なら sizeof(void*) に引き上げ)。
 * @param actual_size 実際にヒープから取った総バイト数を返す (統計用、失敗時 0)。
 * @return アライン済みユーザポインタ (失敗時 nullptr)。
 */
void* AlignedAlloc(usize size, usize alignment, usize& actual_size) noexcept {
    if (alignment < sizeof(void*)) alignment = sizeof(void*);
    // size + alignment + kHeaderSize の加算ラップを防ぐ（過小確保防止）
    if (size > (~usize(0)) - alignment - kHeaderSize) { actual_size = 0; return nullptr; }
    const usize raw_size = size + alignment + kHeaderSize;
    void* raw = ::HeapAlloc(::GetProcessHeap(), 0, raw_size);
    if (!raw) { actual_size = 0; return nullptr; }
    // ヘッダ分とアライメント分を確保した先で切り上げる
    const uptr  base    = reinterpret_cast<uptr>(raw) + kHeaderSize;
    const uptr  aligned = (base + alignment - 1) & ~(alignment - 1);
    void** const stash  = reinterpret_cast<void**>(aligned) - 1;          // ユーザポインタの 8B 手前
    usize* const sstore = reinterpret_cast<usize*>(stash) - 1;            // さらに 8B 手前
    *stash  = raw;       // 元ポインタを保存（Free で HeapFree に渡すため）
    *sstore = raw_size;  // サイズを保存（統計用）
    actual_size = raw_size;
    return reinterpret_cast<void*>(aligned);
}

/**
 * AlignedAlloc で確保した領域を解放する。
 *
 * @details ユーザポインタ手前のヘッダから元ポインタと総サイズを読み、HeapFree に渡す。
 * @param p AlignedAlloc が返したユーザポインタ (nullptr 可)。
 * @param freed_size 解放した総バイト数を返す (統計用、p==nullptr なら 0)。
 */
void AlignedFree(void* p, usize& freed_size) noexcept {
    if (!p) { freed_size = 0; return; }
    void** const stash  = reinterpret_cast<void**>(p) - 1;
    usize* const sstore = reinterpret_cast<usize*>(stash) - 1;
    freed_size = *sstore;
    ::HeapFree(::GetProcessHeap(), 0, *stash);
}

} // namespace

void* FSystemAllocator::Alloc(usize size, usize alignment, FSourceLoc /*loc*/) noexcept {
    if (size == 0) return nullptr;
    usize actual = 0;
    void* const p = AlignedAlloc(size, alignment, actual);
    if (!p) return nullptr;
    // 統計更新（atomic）
    const u64 cur = m_Bytes.FetchAdd(actual) + actual;
    // ピーク更新は CAS で（cur > peak の間ループ）
    u64 peak = m_Peak.Load(EMemoryOrder::Relaxed);
    while (cur > peak && !m_Peak.CompareExchange(peak, cur)) {}
    return p;
}

void FSystemAllocator::Free(void* ptr) noexcept {
    if (!ptr) return;
    usize freed = 0;
    AlignedFree(ptr, freed);
    m_Bytes.FetchSub(freed);
}

// HeapReAlloc はアラインヘッダ方式と相性が悪いので、デフォルト実装を使う
void* FSystemAllocator::Realloc(void* ptr, usize old_size, usize new_size,
                               usize alignment, FSourceLoc loc) noexcept {
    return FAllocator::Realloc(ptr, old_size, new_size, alignment, loc);
}

} // namespace acs
