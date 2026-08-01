// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — CLinearAllocator 実装
// -----------------------------------------------------------------------------
// アトミック CAS でカーソル位置を進めるだけ。多スレッド競合下でも
// O(1)（CAS リトライ次第で実測 1-3 倍）。Free は何もしない。
// =============================================================================
#include "memory/LinearAllocator.h"
#include "memory/Memory.h"
#include "threading/Thread.h"

namespace acs {

CLinearAllocator::CLinearAllocator(usize BufferCapacity, IAllocator* BackingAllocator) noexcept
    : m_Capacity(BufferCapacity),
      m_Backing(BackingAllocator ? BackingAllocator : &DefaultAllocator()),
      m_bOwnsBacking(false)
{
    m_Base = static_cast<u8*>(m_Backing->Alloc(BufferCapacity, kDefaultAlignment, FSourceLoc::Current()));
}

CLinearAllocator::~CLinearAllocator() noexcept
{
    LockLifecycleControl();
    CloseAllocationGateAndWait();
    if (m_Base)
    {
        m_Backing->Free(m_Base);
        m_Base = nullptr;
    }
}

bool CLinearAllocator::TryBeginAllocation() noexcept
{
    u64 GateValue = m_AllocationGate.Load(EMemoryOrder::Acquire);
    const u64 EntryGeneration = GateValue & kAllocationGateGenerationMask;
    while ((GateValue & kAllocationGateAcceptingBit) != 0u)
    {
        if ((GateValue & kAllocationGateGenerationMask) != EntryGeneration)
        {
            return false;
        }

        const u64 OperationCount = GateValue & kAllocationGateOperationCountMask;
        if (OperationCount == kAllocationGateOperationCountMask)
        {
            return false;
        }

        const u64 DesiredGateValue = GateValue + 1u;
        if (m_AllocationGate.CompareExchange(GateValue, DesiredGateValue))
        {
            return true;
        }
    }
    return false;
}

void CLinearAllocator::EndAllocation() noexcept
{
    m_AllocationGate.FetchSub(1u);
}

void CLinearAllocator::CloseAllocationGateAndWait() noexcept
{
    m_AllocationGate.FetchAnd(~kAllocationGateAcceptingBit);
    u32 SpinCount = 0u;
    while ((m_AllocationGate.Load(EMemoryOrder::Acquire) & kAllocationGateOperationCountMask) != 0u)
    {
        if (SpinCount < 64u)
        {
            ++SpinCount;
            SpinHint();
        }
        else
        {
            Yield();
        }
    }
}

void CLinearAllocator::OpenAllocationGate() noexcept
{
    const u64 ClosedGateValue = m_AllocationGate.Load(EMemoryOrder::Acquire);
    ACS_ASSERT((ClosedGateValue & kAllocationGateAcceptingBit) == 0u);
    ACS_ASSERT((ClosedGateValue & kAllocationGateOperationCountMask) == 0u);

    u64 NextGeneration = (ClosedGateValue + kAllocationGateGenerationIncrement) &
                         kAllocationGateGenerationMask;
    if (NextGeneration == 0u)
    {
        NextGeneration = kAllocationGateGenerationIncrement;
    }
    m_AllocationGate.Store(kAllocationGateAcceptingBit | NextGeneration, EMemoryOrder::Release);
}

void CLinearAllocator::LockLifecycleControl() noexcept
{
    u32 SpinCount = 0u;
    for (;;)
    {
        u32 ExpectedControl = 0u;
        if (m_LifecycleControl.CompareExchange(ExpectedControl, 1u))
        {
            return;
        }

        if (SpinCount < 64u)
        {
            ++SpinCount;
            SpinHint();
        }
        else
        {
            Yield();
        }
    }
}

void CLinearAllocator::UnlockLifecycleControl() noexcept
{
    m_LifecycleControl.Store(0u, EMemoryOrder::Release);
}

// CAS ループでカーソルを進める：
//   1. 現在カーソル cur を読む
//   2. アライン後の位置を計算
//   3. 必要バイトを足した next を求める
//   4. 容量超過なら nullptr
//   5. CompareExchange で m_Used を CurrentUsed → NextUsed に交換
//   6. 競合した場合は 1 へ戻ってリトライ
void* CLinearAllocator::Alloc(usize Size, usize Alignment, FSourceLoc /*Location*/) noexcept
{
    if (Size == 0u || Alignment == 0u || (Alignment & (Alignment - 1u)) != 0u)
    {
        return nullptr;
    }
    if (!TryBeginAllocation())
    {
        return nullptr;
    }

    struct FActiveAllocationScope
    {
        CLinearAllocator* Allocator = nullptr;

        ~FActiveAllocationScope() noexcept
        {
            Allocator->EndAllocation();
        }
    } AllocationScope{this};

    if (!m_Base)
    {
        return nullptr;
    }

    while (true)
    {
        const u64 CurrentUsed = m_Used.Load(EMemoryOrder::Relaxed);
        const u64 BaseAddress = reinterpret_cast<u64>(m_Base);
        if (CurrentUsed > m_Capacity || CurrentUsed > (~u64(0)) - BaseAddress ||
            BaseAddress + CurrentUsed > (~u64(0)) - (Alignment - 1u))
        {
            return nullptr;
        }
        const u64 AlignedOffset = AlignUp(BaseAddress + CurrentUsed, Alignment) - BaseAddress;
        if (AlignedOffset > m_Capacity || Size > m_Capacity - AlignedOffset)
        {
            return nullptr;
        }
        const u64 NextUsed = AlignedOffset + Size;
        u64 ExpectedUsed = CurrentUsed;
        if (m_Used.CompareExchange(ExpectedUsed, NextUsed))
        {
            // ピーク更新
            m_AllocationCount.FetchAdd(1u);
            u64 RecordedPeakBytes = m_Peak.Load(EMemoryOrder::Relaxed);
            while (NextUsed > RecordedPeakBytes && !m_Peak.CompareExchange(RecordedPeakBytes, NextUsed))
            {
            }
            return m_Base + AlignedOffset;
        }
        // 他スレッドが先に進めた — 再試行
    }
}

void CLinearAllocator::Free(void* /*Pointer*/) noexcept
{
    // リニアアロケータは個別解放をサポートしない（仕様）。
    // 全体の解放は Reset() で行う。
}

void CLinearAllocator::Reset() noexcept
{
    LockLifecycleControl();
    CloseAllocationGateAndWait();
    m_Used.Store(0, EMemoryOrder::Release);
    m_AllocationCount.Store(0, EMemoryOrder::Release);
    OpenAllocationGate();
    UnlockLifecycleControl();
}

} // namespace acs
