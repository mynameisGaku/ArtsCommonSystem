// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/LinearAllocator.h"
#include "foundation/Move.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

/** LinearAllocator の Reset 競合ストレスで共有する状態。 */
struct FLinearAllocatorResetRaceContext
{
    CLinearAllocator* Allocator = nullptr;
    TAtomic<u32> Start{0u};
    TAtomic<u32> Stop{0u};
    TAtomic<u32> FailureCount{0u};
    TAtomic<u64> SuccessfulAllocationCount{0u};
};

/** Reset と並行して確保を繰り返し、返却アドレスの最小契約を検証する。 */
void AllocateWhileLinearAllocatorResets(void* User) noexcept
{
    auto* const Context = static_cast<FLinearAllocatorResetRaceContext*>(User);
    while (Context->Start.Load(EMemoryOrder::Acquire) == 0u)
    {
        Yield();
    }

    while (Context->Stop.Load(EMemoryOrder::Acquire) == 0u)
    {
        void* const Allocation = Context->Allocator->Alloc(16u, 16u, FSourceLoc::Current());
        if (!Allocation)
        {
            Yield();
            continue;
        }
        if ((reinterpret_cast<uptr>(Allocation) & 15u) != 0u)
        {
            Context->FailureCount.FetchAdd(1u);
        }
        Context->SuccessfulAllocationCount.FetchAdd(1u);
    }
}

/** メインスレッドの Reset と競合させるため、別スレッドからも Reset を繰り返す。 */
void ResetLinearAllocatorRepeatedly(void* User) noexcept
{
    auto* const Context = static_cast<FLinearAllocatorResetRaceContext*>(User);
    while (Context->Start.Load(EMemoryOrder::Acquire) == 0u)
    {
        Yield();
    }

    for (u32 Iteration = 0u; Iteration < 5000u; ++Iteration)
    {
        Context->Allocator->Reset();
    }
}

} // namespace

ACS_TEST(Memory, LinearAllocatorResetDrainsConcurrentAllocations)
{
    constexpr u32 kAllocationWorkerCount = 6u;
    CLinearAllocator Allocator(8192u);
    FLinearAllocatorResetRaceContext Context{};
    Context.Allocator = &Allocator;

    FThread AllocationWorkers[kAllocationWorkerCount];
    u32 SpawnedAllocationWorkerCount = 0u;
    for (u32 Index = 0u; Index < kAllocationWorkerCount; ++Index)
    {
        auto Worker = FThread::Spawn(&AllocateWhileLinearAllocatorResets, &Context);
        EXPECT_TRUE(Worker.IsOk());
        if (Worker.IsOk())
        {
            AllocationWorkers[SpawnedAllocationWorkerCount++] = Move(Worker.Value());
        }
    }

    auto ResetWorker = FThread::Spawn(&ResetLinearAllocatorRepeatedly, &Context);
    EXPECT_TRUE(ResetWorker.IsOk());
    Context.Start.Store(1u, EMemoryOrder::Release);
    for (u32 Iteration = 0u; Iteration < 5000u; ++Iteration)
    {
        Allocator.Reset();
    }
    if (ResetWorker.IsOk())
    {
        ResetWorker.Value().Join();
    }

    Context.Stop.Store(1u, EMemoryOrder::Release);
    for (u32 Index = 0u; Index < SpawnedAllocationWorkerCount; ++Index)
    {
        AllocationWorkers[Index].Join();
    }
    Allocator.Reset();

    EXPECT_TRUE(Context.SuccessfulAllocationCount.Load(EMemoryOrder::Acquire) > 0u);
    EXPECT_EQ(Context.FailureCount.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(Allocator.PeakBytes() <= Allocator.Capacity());
}
