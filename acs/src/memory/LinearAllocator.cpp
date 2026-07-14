// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — FLinearAllocator 実装
// -----------------------------------------------------------------------------
// アトミック CAS でカーソル位置を進めるだけ。多スレッド競合下でも
// O(1)（CAS リトライ次第で実測 1-3 倍）。Free は何もしない。
// =============================================================================
#include "memory/LinearAllocator.h"
#include "memory/Memory.h"

namespace acs {

FLinearAllocator::FLinearAllocator(usize BufferCapacity, FAllocator* BackingAllocator) noexcept
    : m_Capacity(BufferCapacity),
      m_Backing(BackingAllocator ? BackingAllocator : &DefaultAllocator()),
      m_bOwnsBacking(false)
{
    m_Base = static_cast<u8*>(m_Backing->Alloc(BufferCapacity, kDefaultAlignment, FSourceLoc::Current()));
}

FLinearAllocator::~FLinearAllocator() noexcept
{
    if (m_Base) m_Backing->Free(m_Base);
}

// CAS ループでカーソルを進める：
//   1. 現在カーソル cur を読む
//   2. アライン後の位置を計算
//   3. 必要バイトを足した next を求める
//   4. 容量超過なら nullptr
//   5. CompareExchange で m_Used を CurrentUsed → NextUsed に交換
//   6. 競合した場合は 1 へ戻ってリトライ
void* FLinearAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0 || !m_Base) return nullptr;
    if (Alignment < 1) Alignment = 1;
    while (true) {
        const u64 CurrentUsed = m_Used.Load(EMemoryOrder::Relaxed);
        const u64 BaseAddress = reinterpret_cast<u64>(m_Base);
        const u64 AlignedOffset = AlignUp(BaseAddress + CurrentUsed, Alignment) - BaseAddress;
        const u64 NextUsed = AlignedOffset + Size;
        if (NextUsed > m_Capacity) return nullptr; // 予算超過
        u64 ExpectedUsed = CurrentUsed;
        if (m_Used.CompareExchange(ExpectedUsed, NextUsed)) {
            // ピーク更新
            m_AllocationCount.FetchAdd(1u);
            u64 RecordedPeakBytes = m_Peak.Load(EMemoryOrder::Relaxed);
            while (NextUsed > RecordedPeakBytes && !m_Peak.CompareExchange(RecordedPeakBytes, NextUsed)) {}
            return m_Base + AlignedOffset;
        }
        // 他スレッドが先に進めた — 再試行
    }
}

void FLinearAllocator::Free(void* /*Pointer*/) noexcept
{
    // リニアアロケータは個別解放をサポートしない（仕様）。
    // 全体の解放は Reset() で行う。
}

void FLinearAllocator::Reset() noexcept
{
    m_Used.Store(0, EMemoryOrder::Release);
    m_AllocationCount.Store(0, EMemoryOrder::Release);
}

} // namespace acs
