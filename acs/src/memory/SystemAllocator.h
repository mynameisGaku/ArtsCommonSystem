// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — システムアロケータ（Win32 プロセスヒープ）
// -----------------------------------------------------------------------------
// HeapAlloc / HeapFree を使った汎用アロケータ。プロセスヒープは OS が
// 内部でロックを取って直列化するため、スレッドセーフ（HEAP_NO_SERIALIZE
// は意図的に未指定）。
//
// アライメント:
//   HeapAlloc 自体は MEMORY_ALLOCATION_ALIGNMENT (x64 で 16B) しか保証しない。
//   それを超えるアライメントが必要な場合は、内部で「アラインド確保 +
//   ヘッダで元ポインタを覚える」方式（AlignedAlloc）を使う。
//
// 性能注意:
//   HeapAlloc は数百 ns のオーバーヘッドがある。フレーム内で何千回も呼ぶ
//   ようなホットパスでは、専用プールやアリーナを別途用意すべき。
// =============================================================================
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

    // 統計取得（アトミックで集計）
    u64 BytesAllocated() const noexcept override { return _bytes.Load(MemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(MemoryOrder::Acquire); }
    const char* Name()   const noexcept override { return "System"; }

private:
    mutable Atomic<u64> _bytes {0};   // 現在の総割当量
    mutable Atomic<u64> _peak  {0};   // 過去ピーク
};

} // namespace acs
