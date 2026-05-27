// SPDX-License-Identifier: Apache-2.0
// 単一連続バッファのバンプアロケータ（Free は no-op、Reset で全体巻き戻し）
#pragma once

#include "memory/Allocator.h"
#include "threading/Atomic.h"

namespace acs {

class FLinearAllocator final : public FAllocator {
public:
    // capacity バイトのバッファを backing から確保（null なら DefaultAllocator）
    FLinearAllocator(usize capacity, FAllocator* backing = nullptr) noexcept;
    ~FLinearAllocator() noexcept override;

    FLinearAllocator(const FLinearAllocator&) = delete;
    FLinearAllocator& operator=(const FLinearAllocator&) = delete;

    void* Alloc(usize size, usize alignment, FSourceLoc loc) noexcept override;
    void  Free (void* ptr) noexcept override;  // no-op（個別解放は不可）

    // カーソルを 0 に戻す（並行 Alloc 中に呼ぶと UB）
    void Reset() noexcept;

    u64 BytesAllocated() const noexcept override { return m_Used.Load(EMemoryOrder::Acquire); }
    u64 PeakBytes()      const noexcept override { return m_Peak.Load(EMemoryOrder::Acquire); }
    u64 Capacity()       const noexcept          { return m_Capacity; }
    const char* Name()   const noexcept override { return "Linear"; }

private:
    u8*           m_Base     = nullptr;
    u64           m_Capacity = 0;
    FAllocator*    m_Backing  = nullptr;
    bool          m_bOwnsBacking = false;
    TAtomic<u64>   m_Used {0};                  // 現在のカーソル位置
    mutable TAtomic<u64> m_Peak {0};            // ピーク位置
};

} // namespace acs
