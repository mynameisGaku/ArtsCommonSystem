// SPDX-License-Identifier: Apache-2.0
// 単一連続バッファのバンプアロケータ（Free は no-op、Reset で全体巻き戻し）
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class LinearAllocator final : public Allocator {
public:
    // capacity バイトのバッファを backing から確保（null なら DefaultAllocator）
    LinearAllocator(usize capacity, Allocator* backing = nullptr) noexcept;
    ~LinearAllocator() noexcept override;

    LinearAllocator(const LinearAllocator&) = delete;
    LinearAllocator& operator=(const LinearAllocator&) = delete;

    void* Alloc(usize size, usize alignment, SourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override;  // no-op（個別解放は不可）

    // カーソルを 0 に戻す（並行 Alloc 中に呼ぶと UB）
    void Reset() noexcept;

    u64 BytesAllocated() const noexcept override { return _used.Load(EMemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return _peak.Load(EMemoryOrder::Acquire); }
    u64 Capacity()       const noexcept          { return _capacity; }
    const char* Name()   const noexcept override { return "Linear"; }

private:
    u8*           _base     = nullptr;
    u64           _capacity = 0;
    Allocator*    _backing  = nullptr;
    bool          _owns_backing = false;
    Atomic<u64>   _used {0};                  // 現在のカーソル位置
    mutable Atomic<u64> _peak {0};            // ピーク位置
};

} // namespace acs
