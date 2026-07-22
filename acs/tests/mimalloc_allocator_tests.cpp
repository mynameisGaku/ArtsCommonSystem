// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - FMimallocAllocator 契約テスト
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Move.h"
#include "memory/MimallocAllocator.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

ACS_TEST(MimallocAllocator, BasicStatisticsBudgetAndInspection)
{
    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(2ull * 1024ull * 1024ull).IsOk());
    if (!Allocator.IsInitialized()) {
        return;
    }

    EXPECT_FALSE(Allocator.Init().IsOk());
    EXPECT_TRUE(FMimallocAllocator::RuntimeVersion() >= 300);
    EXPECT_EQ(Allocator.HardBudgetBytes(), 2ull * 1024ull * 1024ull);
    EXPECT_TRUE(Allocator.Alloc(0u, 16u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Alloc(16u, 0u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Alloc(16u, 3u, FSourceLoc::Current()) == nullptr);

    void* SmallAllocation = Allocator.Alloc(4096u, 16u, FSourceLoc::Current());
    void* MediumAllocation = Allocator.Alloc(4097u, 32u, FSourceLoc::Current());
    void* LargeAllocation = Allocator.Alloc(1024u * 1024u + 1u, 64u, FSourceLoc::Current());
    EXPECT_TRUE(SmallAllocation != nullptr);
    EXPECT_TRUE(MediumAllocation != nullptr);
    EXPECT_TRUE(LargeAllocation != nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 3ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 4096ull + 4097ull + 1024ull * 1024ull + 1ull);
    EXPECT_EQ(Allocator.PeakBytes(), Allocator.BytesAllocated());

    const FMimallocAllocationHistogram Histogram = Allocator.CaptureAllocationHistogram();
    EXPECT_EQ(Histogram.small.allocation_count, 1ull);
    EXPECT_EQ(Histogram.small.requested_bytes, 4096ull);
    EXPECT_EQ(Histogram.medium.allocation_count, 1ull);
    EXPECT_EQ(Histogram.medium.requested_bytes, 4097ull);
    EXPECT_EQ(Histogram.large.allocation_count, 1ull);
    EXPECT_EQ(Histogram.large.requested_bytes, 1024ull * 1024ull + 1ull);

    const FMimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
    EXPECT_TRUE(Inspection.visit_succeeded);
    EXPECT_TRUE(Inspection.metadata_valid);
    EXPECT_TRUE(Inspection.matches_authoritative_statistics);
    EXPECT_EQ(Inspection.allocation_count, 3ull);
    EXPECT_EQ(Inspection.requested_bytes, Allocator.BytesAllocated());
    EXPECT_TRUE(Inspection.usable_bytes >= Inspection.requested_bytes);
    EXPECT_TRUE(Inspection.reserved_bytes >= Inspection.committed_bytes);

    void* OverBudgetAllocation = Allocator.Alloc(1024u * 1024u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(OverBudgetAllocation == nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 3ull);

    Allocator.Free(SmallAllocation);
    Allocator.Free(MediumAllocation);
    Allocator.Free(LargeAllocation);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.PeakBytes(), 4096ull + 4097ull + 1024ull * 1024ull + 1ull);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, AlignmentAndReallocationPreserveData)
{
    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(4096u).IsOk());
    if (!Allocator.IsInitialized()) {
        return;
    }

    void* AlignedBlocks[13] = {};
    for (usize Index = 0; Index < 13u; ++Index) {
        const usize Alignment = usize(1) << Index;
        AlignedBlocks[Index] = Allocator.Alloc(17u + Index, Alignment, FSourceLoc::Current());
        EXPECT_TRUE(AlignedBlocks[Index] != nullptr);
        if (AlignedBlocks[Index]) {
            EXPECT_EQ(reinterpret_cast<uptr>(AlignedBlocks[Index]) & (Alignment - 1u), 0ull);
        }
    }
    const FMimallocHeapInspectionStatistics AlignedInspection = Allocator.InspectHeap();
    EXPECT_TRUE(AlignedInspection.visit_succeeded);
    EXPECT_TRUE(AlignedInspection.metadata_valid);
    EXPECT_TRUE(AlignedInspection.matches_authoritative_statistics);
    EXPECT_EQ(AlignedInspection.allocation_count, 13ull);
    for (void* Block : AlignedBlocks) {
        Allocator.Free(Block);
    }

    auto* Bytes = static_cast<u8*>(Allocator.Alloc(64u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(Bytes != nullptr);
    if (!Bytes) {
        Allocator.Shutdown();
        return;
    }
    for (u32 Index = 0; Index < 64u; ++Index) {
        Bytes[Index] = static_cast<u8>(Index ^ 0x5Au);
    }

    Bytes = static_cast<u8*>(Allocator.Realloc(Bytes, 1u, 512u, 256u, FSourceLoc::Current()));
    EXPECT_TRUE(Bytes != nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 512ull);
    EXPECT_EQ(reinterpret_cast<uptr>(Bytes) & 255u, 0ull);
    if (Bytes) {
        for (u32 Index = 0; Index < 64u; ++Index) {
            EXPECT_EQ(Bytes[Index], static_cast<u8>(Index ^ 0x5Au));
        }
    }

    void* FailedReallocation = Allocator.Realloc(Bytes, 512u, 8192u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(FailedReallocation == nullptr);
    EXPECT_TRUE(Allocator.OwnsAllocation(Bytes));
    EXPECT_EQ(Allocator.BytesAllocated(), 512ull);

    Bytes = static_cast<u8*>(Allocator.Realloc(Bytes, 512u, 32u, 8u, FSourceLoc::Current()));
    EXPECT_TRUE(Bytes != nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 32ull);
    if (Bytes) {
        for (u32 Index = 0; Index < 32u; ++Index) {
            EXPECT_EQ(Bytes[Index], static_cast<u8>(Index ^ 0x5Au));
        }
    }

    Allocator.Free(Bytes);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, ReallocationReservesOldAndNewRequestsDuringMove)
{
    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(767u).IsOk());
    if (!Allocator.IsInitialized()) {
        return;
    }

    void* const OriginalAllocation = Allocator.Alloc(512u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(OriginalAllocation != nullptr);
    if (!OriginalAllocation) {
        Allocator.Shutdown();
        return;
    }

    // 移動中は旧 512 + 新 256 = 768 bytes が同時に必要なため、767 bytes 予算では失敗する。
    void* const ReplacementAllocation = Allocator.Realloc(OriginalAllocation, 512u, 256u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(ReplacementAllocation == nullptr);
    EXPECT_TRUE(Allocator.OwnsAllocation(OriginalAllocation));
    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 512ull);

    Allocator.Free(OriginalAllocation);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, CrossThreadAllocationAndFree)
{
    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init().IsOk());
    if (!Allocator.IsInitialized()) {
        return;
    }

    struct FContext {
        FMimallocAllocator* allocator = nullptr;
        void* block_to_free = nullptr;
        void* allocated_block = nullptr;
    } TestContext;
    TestContext.allocator = &Allocator;
    TestContext.block_to_free = Allocator.Alloc(128u, 64u, FSourceLoc::Current());
    EXPECT_TRUE(TestContext.block_to_free != nullptr);

    auto ThreadResult = FThread::Spawn(
        [](void* UserData) {
            auto* WorkerContext = static_cast<FContext*>(UserData);
            WorkerContext->allocator->Free(WorkerContext->block_to_free);
            WorkerContext->allocated_block = WorkerContext->allocator->Alloc(257u, 128u, FSourceLoc::Current());
        },
        &TestContext);
    EXPECT_TRUE(ThreadResult.IsOk());
    if (!ThreadResult.IsOk()) {
        Allocator.Free(TestContext.block_to_free);
        Allocator.Shutdown();
        return;
    } else {
        ThreadResult.Value().Join();
    }

    EXPECT_EQ(Allocator.AllocationCount(), 1ull);
    EXPECT_EQ(Allocator.BytesAllocated(), 257ull);
    EXPECT_TRUE(Allocator.OwnsAllocation(TestContext.allocated_block));
    Allocator.Free(TestContext.allocated_block);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, ConcurrentBudgetReservationNeverExceedsLimit)
{
    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(1024u).IsOk());
    if (!Allocator.IsInitialized()) {
        return;
    }

    struct FContext {
        FMimallocAllocator* allocator = nullptr;
        TAtomic<u32> start{0};
        TAtomic<u32> success_count{0};
        void* blocks[4] = {};
    } TestContext;

    struct FWorkerInput {
        FContext* context = nullptr;
        u32 index = 0;
    } Inputs[4];

    TestContext.allocator = &Allocator;
    FThread Threads[4];
    bool bAllThreadsStarted = true;
    for (u32 Index = 0; Index < 4u; ++Index) {
        Inputs[Index].context = &TestContext;
        Inputs[Index].index = Index;
        auto ThreadResult = FThread::Spawn(
            [](void* UserData) {
                auto* Input = static_cast<FWorkerInput*>(UserData);
                while (Input->context->start.Load(EMemoryOrder::Acquire) == 0u) {
                    Yield();
                }
                void* Block = Input->context->allocator->Alloc(1024u, 16u, FSourceLoc::Current());
                Input->context->blocks[Input->index] = Block;
                if (Block) {
                    Input->context->success_count.FetchAdd(1u);
                }
            },
            &Inputs[Index]);

        EXPECT_TRUE(ThreadResult.IsOk());
        if (ThreadResult.IsOk()) {
            Threads[Index] = Move(ThreadResult.Value());
        } else {
            bAllThreadsStarted = false;
        }
    }

    TestContext.start.Store(1u, EMemoryOrder::Release);
    for (FThread& WorkerThread : Threads) {
        if (WorkerThread.Joinable()) {
            WorkerThread.Join();
        }
    }

    if (bAllThreadsStarted) {
        EXPECT_EQ(TestContext.success_count.Load(EMemoryOrder::Acquire), 1u);
        EXPECT_EQ(Allocator.AllocationCount(), 1ull);
        EXPECT_EQ(Allocator.BytesAllocated(), 1024ull);
    }

    for (void* Block : TestContext.blocks) {
        Allocator.Free(Block);
    }
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, MultipleHeapsKeepOwnershipSeparate)
{
    FMimallocAllocator FirstAllocator;
    FMimallocAllocator SecondAllocator;
    EXPECT_TRUE(FirstAllocator.Init().IsOk());
    EXPECT_TRUE(SecondAllocator.Init().IsOk());
    if (!FirstAllocator.IsInitialized() || !SecondAllocator.IsInitialized()) {
        FirstAllocator.Shutdown();
        SecondAllocator.Shutdown();
        return;
    }

    void* FirstBlock = FirstAllocator.Alloc(96u, 16u, FSourceLoc::Current());
    void* SecondBlock = SecondAllocator.Alloc(192u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(FirstAllocator.OwnsAllocation(FirstBlock));
    EXPECT_FALSE(FirstAllocator.OwnsAllocation(SecondBlock));
    EXPECT_TRUE(SecondAllocator.OwnsAllocation(SecondBlock));
    EXPECT_FALSE(SecondAllocator.OwnsAllocation(FirstBlock));

    SecondAllocator.Free(FirstBlock);
    EXPECT_EQ(FirstAllocator.AllocationCount(), 1ull);
    EXPECT_EQ(SecondAllocator.AllocationCount(), 1ull);

    FirstAllocator.Free(FirstBlock);
    SecondAllocator.Free(SecondBlock);
    FirstAllocator.Shutdown();
    SecondAllocator.Shutdown();
}

ACS_TEST(MimallocAllocator, CollectAndReinitialize)
{
    FMimallocAllocator Allocator;
    for (u32 Cycle = 0; Cycle < 3u; ++Cycle) {
        EXPECT_TRUE(Allocator.Init(1024u).IsOk());
        if (!Allocator.IsInitialized()) {
            return;
        }

        void* Block = Allocator.Alloc(1024u, 32u, FSourceLoc::Current());
        EXPECT_TRUE(Block != nullptr);
        Allocator.Free(Block);
        Allocator.Collect(false);
        Allocator.Collect(true);
        const FMimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
        EXPECT_TRUE(Inspection.visit_succeeded);
        EXPECT_TRUE(Inspection.metadata_valid);
        EXPECT_TRUE(Inspection.matches_authoritative_statistics);
        EXPECT_EQ(Inspection.allocation_count, 0ull);
        Allocator.Shutdown();
        EXPECT_FALSE(Allocator.IsInitialized());
    }
}

ACS_TEST(MimallocAllocator, ConcurrentFreeAndReallocationClaimOwnershipOnce)
{
    constexpr u32 kThreadCount = 4u;
    constexpr u32 kCycleCount = 128u;

    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init().IsOk());
    if (!Allocator.IsInitialized())
    {
        return;
    }

    for (u32 Cycle = 0u; Cycle < kCycleCount; ++Cycle)
    {
        void* const OriginalAllocation = Allocator.Alloc(128u, 16u, FSourceLoc::Current());
        EXPECT_TRUE(OriginalAllocation != nullptr);
        if (!OriginalAllocation)
        {
            break;
        }

        struct FMutationContext
        {
            FMimallocAllocator* Allocator = nullptr;
            void* OriginalAllocation = nullptr;
            TAtomic<u32> Start{0u};
            bool bFreeSucceeded[kThreadCount] = {};
            FMimallocReallocationResult ReallocationResults[kThreadCount] = {};
        } Context;
        Context.Allocator = &Allocator;
        Context.OriginalAllocation = OriginalAllocation;

        struct FWorkerInput
        {
            FMutationContext* Context = nullptr;
            u32 WorkerIndex = 0u;
        } Inputs[kThreadCount];

        FThread Threads[kThreadCount];
        u32 StartedThreadCount = 0u;
        for (u32 WorkerIndex = 0u; WorkerIndex < kThreadCount; ++WorkerIndex)
        {
            Inputs[WorkerIndex].Context = &Context;
            Inputs[WorkerIndex].WorkerIndex = WorkerIndex;
            auto ThreadResult = FThread::Spawn(
                [](void* UserData)
                {
                    auto* const Input = static_cast<FWorkerInput*>(UserData);
                    while (Input->Context->Start.Load(EMemoryOrder::Acquire) == 0u)
                    {
                        Yield();
                    }

                    if ((Input->WorkerIndex & 1u) == 0u)
                    {
                        Input->Context->bFreeSucceeded[Input->WorkerIndex] =
                            Input->Context->Allocator->TryFreeAllocation(Input->Context->OriginalAllocation);
                    }
                    else
                    {
                        Input->Context->ReallocationResults[Input->WorkerIndex] =
                            Input->Context->Allocator->TryReallocateAllocation(
                                Input->Context->OriginalAllocation, 256u + Input->WorkerIndex, 16u);
                    }
                },
                &Inputs[WorkerIndex]);
            EXPECT_TRUE(ThreadResult.IsOk());
            if (ThreadResult.IsOk())
            {
                Threads[WorkerIndex] = Move(ThreadResult.Value());
                ++StartedThreadCount;
            }
        }

        Context.Start.Store(1u, EMemoryOrder::Release);
        for (u32 WorkerIndex = 0u; WorkerIndex < kThreadCount; ++WorkerIndex)
        {
            if (Threads[WorkerIndex].Joinable())
            {
                Threads[WorkerIndex].Join();
            }
        }

        if (StartedThreadCount == 0u)
        {
            Allocator.Free(OriginalAllocation);
            break;
        }

        u32 OwnershipAcceptedCount = 0u;
        void* ReplacementAllocation = nullptr;
        for (u32 WorkerIndex = 0u; WorkerIndex < kThreadCount; ++WorkerIndex)
        {
            if (Context.bFreeSucceeded[WorkerIndex])
            {
                ++OwnershipAcceptedCount;
            }
            if (Context.ReallocationResults[WorkerIndex].bOwnershipAccepted)
            {
                ++OwnershipAcceptedCount;
                EXPECT_TRUE(Context.ReallocationResults[WorkerIndex].bSucceeded);
                ReplacementAllocation = Context.ReallocationResults[WorkerIndex].Pointer;
            }
        }
        EXPECT_EQ(OwnershipAcceptedCount, 1u);

        if (ReplacementAllocation)
        {
            EXPECT_TRUE(Allocator.TryFreeAllocation(ReplacementAllocation));
        }
        else if (Allocator.AllocationCount() != 0u)
        {
            // 極端な OOM で再確保だけが失敗した場合、Active に戻った旧確保を回収する。
            EXPECT_TRUE(Allocator.TryFreeAllocation(OriginalAllocation));
        }
        EXPECT_EQ(Allocator.AllocationCount(), 0ull);
        EXPECT_EQ(Allocator.BytesAllocated(), 0ull);

        // 次サイクルへ古い raw pointer の猶予を持ち越さず、全競合スレッド join 後に物理解放する。
        Allocator.Collect(false);
    }

    Allocator.Collect(true);
    const FMimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
    EXPECT_TRUE(Inspection.visit_succeeded);
    EXPECT_TRUE(Inspection.metadata_valid);
    EXPECT_TRUE(Inspection.matches_authoritative_statistics);
    EXPECT_EQ(Inspection.allocation_count, 0ull);
    Allocator.Shutdown();
}

ACS_TEST(MimallocAllocator, LifecycleMaintenanceAndShutdownDrainConcurrentOperations)
{
    constexpr u32 kWorkerCount = 4u;
    constexpr u64 kRequiredSuccessfulAllocations = 256u;
    constexpr u32 kWaitLimit = 500000u;

    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(8ull * 1024ull * 1024ull).IsOk());
    if (!Allocator.IsInitialized())
    {
        return;
    }

    struct FRaceContext
    {
        FMimallocAllocator* Allocator = nullptr;
        TAtomic<u32> Ready{0u};
        TAtomic<u32> Start{0u};
        TAtomic<u32> Stop{0u};
        TAtomic<u32> QuiesceAllocations{0u};
        TAtomic<u32> QuiescedWorkerMask{0u};
        TAtomic<u32> ShutdownObservedMask{0u};
        TAtomic<u64> SuccessfulAllocations{0u};
        TAtomic<u64> RejectedOperations{0u};
    } Context;
    Context.Allocator = &Allocator;

    struct FWorkerInput
    {
        FRaceContext* Context = nullptr;
        u32 WorkerIndex = 0u;
    } Inputs[kWorkerCount];

    FThread Workers[kWorkerCount];
    u32 SpawnedWorkerCount = 0u;
    for (u32 Attempt = 0u; Attempt < kWorkerCount; ++Attempt)
    {
        Inputs[SpawnedWorkerCount].Context = &Context;
        Inputs[SpawnedWorkerCount].WorkerIndex = SpawnedWorkerCount;
        auto ThreadResult = FThread::Spawn(
            [](void* UserData)
            {
                auto* const Input = static_cast<FWorkerInput*>(UserData);
                FRaceContext* const WorkerContext = Input->Context;
                u32 Iteration = 0u;
                WorkerContext->Ready.FetchAdd(1u);
                while (WorkerContext->Start.Load(EMemoryOrder::Acquire) == 0u)
                {
                    Yield();
                }

                while (WorkerContext->Stop.Load(EMemoryOrder::Acquire) == 0u)
                {
                    if (WorkerContext->QuiesceAllocations.Load(EMemoryOrder::Acquire) != 0u)
                    {
                        WorkerContext->QuiescedWorkerMask.FetchOr(1u << Input->WorkerIndex);
                        if (!WorkerContext->Allocator->IsInitialized())
                        {
                            WorkerContext->RejectedOperations.FetchAdd(1u);
                            WorkerContext->ShutdownObservedMask.FetchOr(1u << Input->WorkerIndex);
                        }
                        (void)WorkerContext->Allocator->BytesAllocated();
                        (void)WorkerContext->Allocator->AllocationCount();
                        (void)WorkerContext->Allocator->LifetimeGeneration();
                        Yield();
                        continue;
                    }

                    void* Allocation = WorkerContext->Allocator->Alloc(
                        64u + (Iteration & 127u), 16u, FSourceLoc::Current());
                    if (!Allocation)
                    {
                        WorkerContext->RejectedOperations.FetchAdd(1u);
                        ++Iteration;
                        continue;
                    }

                    WorkerContext->SuccessfulAllocations.FetchAdd(1u);
                    const FMimallocReallocationResult Reallocation =
                        WorkerContext->Allocator->TryReallocateAllocation(
                            Allocation, 256u + (Iteration & 255u), 16u);
                    void* AllocationToFree = Allocation;
                    if (Reallocation.bSucceeded && Reallocation.Pointer)
                    {
                        AllocationToFree = Reallocation.Pointer;
                    }

                    // Collect / Inspect が呼出間に受付を閉じても、再開後に論理解放を完了させる。
                    while (!WorkerContext->Allocator->TryFreeAllocation(AllocationToFree))
                    {
                        Yield();
                    }

                    (void)WorkerContext->Allocator->BytesAllocated();
                    (void)WorkerContext->Allocator->AllocationCount();
                    (void)WorkerContext->Allocator->PeakBytes();
                    (void)WorkerContext->Allocator->LifetimeGeneration();
                    (void)WorkerContext->Allocator->HardBudgetBytes();
                    (void)WorkerContext->Allocator->CaptureAllocationHistogram();
                    ++Iteration;
                }
            },
            &Inputs[SpawnedWorkerCount]);
        EXPECT_TRUE(ThreadResult.IsOk());
        if (ThreadResult.IsOk())
        {
            Workers[SpawnedWorkerCount] = Move(ThreadResult.Value());
            ++SpawnedWorkerCount;
        }
    }

    EXPECT_TRUE(SpawnedWorkerCount > 0u);
    if (SpawnedWorkerCount == 0u)
    {
        Allocator.Shutdown();
        return;
    }

    while (Context.Ready.Load(EMemoryOrder::Acquire) < SpawnedWorkerCount)
    {
        Yield();
    }
    Context.Start.Store(1u, EMemoryOrder::Release);
    for (u32 WaitIteration = 0u;
         WaitIteration < kWaitLimit &&
         Context.SuccessfulAllocations.Load(EMemoryOrder::Acquire) < kRequiredSuccessfulAllocations;
         ++WaitIteration)
    {
        Yield();
    }
    EXPECT_TRUE(Context.SuccessfulAllocations.Load(EMemoryOrder::Acquire) >=
                kRequiredSuccessfulAllocations);

    for (u32 MaintenanceCycle = 0u; MaintenanceCycle < 16u; ++MaintenanceCycle)
    {
        Allocator.Collect((MaintenanceCycle & 1u) != 0u);
        const FMimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
        EXPECT_TRUE(Inspection.visit_succeeded);
        EXPECT_TRUE(Inspection.metadata_valid);
        EXPECT_TRUE(Inspection.matches_authoritative_statistics);
    }

    const u32 ExpectedShutdownObservedMask = (1u << SpawnedWorkerCount) - 1u;
    Context.QuiesceAllocations.Store(1u, EMemoryOrder::Release);
    for (u32 WaitIteration = 0u;
         WaitIteration < kWaitLimit &&
         Context.QuiescedWorkerMask.Load(EMemoryOrder::Acquire) != ExpectedShutdownObservedMask;
         ++WaitIteration)
    {
        Yield();
    }
    EXPECT_EQ(Context.QuiescedWorkerMask.Load(EMemoryOrder::Acquire), ExpectedShutdownObservedMask);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);

    Context.ShutdownObservedMask.Store(0u, EMemoryOrder::Release);
    Allocator.Shutdown();
    for (u32 WaitIteration = 0u;
         WaitIteration < kWaitLimit &&
         Context.ShutdownObservedMask.Load(EMemoryOrder::Acquire) != ExpectedShutdownObservedMask;
         ++WaitIteration)
    {
        Yield();
    }
    EXPECT_EQ(Context.ShutdownObservedMask.Load(EMemoryOrder::Acquire), ExpectedShutdownObservedMask);
    EXPECT_TRUE(Context.RejectedOperations.Load(EMemoryOrder::Acquire) > 0u);

    Context.Stop.Store(1u, EMemoryOrder::Release);
    for (u32 WorkerIndex = 0u; WorkerIndex < SpawnedWorkerCount; ++WorkerIndex)
    {
        Workers[WorkerIndex].Join();
    }

    EXPECT_FALSE(Allocator.IsInitialized());
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_EQ(Allocator.PeakBytes(), 0ull);
    EXPECT_EQ(Allocator.LifetimeGeneration(), 0ull);
    EXPECT_EQ(Allocator.HardBudgetBytes(), 0ull);
    Allocator.Collect(true);
    const FMimallocHeapInspectionStatistics EmptyInspection = Allocator.InspectHeap();
    EXPECT_FALSE(EmptyInspection.visit_succeeded);

    EXPECT_TRUE(Allocator.Init(1024u).IsOk());
    void* const ReinitializedAllocation = Allocator.Alloc(512u, 32u, FSourceLoc::Current());
    EXPECT_TRUE(ReinitializedAllocation != nullptr);
    EXPECT_TRUE(Allocator.TryFreeAllocation(ReinitializedAllocation));
    const FMimallocHeapInspectionStatistics ReinitializedInspection = Allocator.InspectHeap();
    EXPECT_TRUE(ReinitializedInspection.visit_succeeded);
    EXPECT_TRUE(ReinitializedInspection.metadata_valid);
    EXPECT_TRUE(ReinitializedInspection.matches_authoritative_statistics);
    Allocator.Shutdown();
}

// FMimallocAllocator の並行 Alloc/Free 契約を、スレッド churn を強めて検査する。
// alloc-heavy な worker を生成 → 大量 Alloc/Free → 全 join を複数サイクル繰り返し、
// スレッド終了 (_mi_thread_done) を多数回起こす。churn 後も件数が 0 に戻り、
// ゲート閉塞下の走査 (InspectHeap) と統計が整合することを確認する。
//
// NOTE: mimalloc first-class heap を無保護で複数スレッドから同時 Alloc すると内部
// 状態が壊れるが、そのデータ競合はタイミング依存で単一実行では確率的にしか顕在化
// しない。よって本テストは「並行契約が成立すること」の検査であり、直列化ロック
// 除去に対する確実な回帰ガードではない (データ競合の確実な検出はストレスループ
// 反復か ThreadSanitizer を要する)。
ACS_TEST(MimallocAllocator, ConcurrentAllocationAcrossThreadChurnKeepsHeapConsistent)
{
    constexpr u32 kWorkerCount = 8u;
    constexpr u32 kCycleCount = 5u;

    FMimallocAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(64ull * 1024ull * 1024ull).IsOk());
    if (!Allocator.IsInitialized())
    {
        return;
    }

    struct FWorkerInput
    {
        FMimallocAllocator* Allocator = nullptr;
        TAtomic<u32>* Ready = nullptr;
        TAtomic<u32>* Start = nullptr;
        TAtomic<u64>* FailureCount = nullptr;
        u32 WorkerIndex = 0u;
        u32 IterationCount = 0u;
    };

    for (u32 Cycle = 0u; Cycle < kCycleCount; ++Cycle)
    {
        TAtomic<u32> Ready{0u};
        TAtomic<u32> Start{0u};
        TAtomic<u64> FailureCount{0u};

        FWorkerInput Inputs[kWorkerCount];
        FThread Workers[kWorkerCount];
        u32 SpawnedWorkerCount = 0u;
        for (u32 Index = 0u; Index < kWorkerCount; ++Index)
        {
            Inputs[SpawnedWorkerCount].Allocator = &Allocator;
            Inputs[SpawnedWorkerCount].Ready = &Ready;
            Inputs[SpawnedWorkerCount].Start = &Start;
            Inputs[SpawnedWorkerCount].FailureCount = &FailureCount;
            Inputs[SpawnedWorkerCount].WorkerIndex = SpawnedWorkerCount;
            Inputs[SpawnedWorkerCount].IterationCount = 2000u;
            auto ThreadResult = FThread::Spawn(
                [](void* User)
                {
                    auto* const Input = static_cast<FWorkerInput*>(User);
                    Input->Ready->FetchAdd(1u);
                    while (Input->Start->Load(EMemoryOrder::Acquire) == 0u)
                    {
                        Yield();
                    }
                    for (u32 Iteration = 0u; Iteration < Input->IterationCount; ++Iteration)
                    {
                        const usize Size = 16u + ((Iteration * 24u + Input->WorkerIndex) & 1023u);
                        const usize Alignment = ((Iteration & 3u) == 0u) ? 32u : 16u;
                        // 猶予回収が受付を閉じている間は Alloc / Free が拒否される (設計どおり)。
                        // 全確保を必ず解放してリークさせないよう、成功するまでリトライする。
                        void* Allocation = nullptr;
                        while ((Allocation = Input->Allocator->Alloc(Size, Alignment, FSourceLoc::Current())) == nullptr)
                        {
                            Input->FailureCount->FetchAdd(1u);
                            Yield();
                        }
                        // 実メモリの端まで書き込み、過小確保でないことも確認する。
                        u8* const Bytes = static_cast<u8*>(Allocation);
                        Bytes[0] = static_cast<u8>(Iteration);
                        Bytes[Size - 1u] = static_cast<u8>(Input->WorkerIndex);
                        while (!Input->Allocator->TryFreeAllocation(Allocation))
                        {
                            Yield();
                        }
                    }
                },
                &Inputs[SpawnedWorkerCount]);
            EXPECT_TRUE(ThreadResult.IsOk());
            if (ThreadResult.IsOk())
            {
                Workers[SpawnedWorkerCount] = Move(ThreadResult.Value());
                ++SpawnedWorkerCount;
            }
        }

        while (Ready.Load(EMemoryOrder::Acquire) < SpawnedWorkerCount)
        {
            Yield();
        }
        Start.Store(1u, EMemoryOrder::Release);
        for (u32 Index = 0u; Index < SpawnedWorkerCount; ++Index)
        {
            Workers[Index].Join();
        }

        // FailureCount は猶予回収中の一時拒否回数 (0 でなくてよい。契約どおりリトライで解決)。
        (void)FailureCount.Load(EMemoryOrder::Acquire);
        // 全 worker が自分の確保をリトライ付きで必ず解放したので生存件数は 0 に戻る。
        EXPECT_EQ(Allocator.AllocationCount(), 0ull);
        EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
        // churn 後も保守経路 (ゲート閉塞下の走査) と統計が整合しているか確認する。
        const FMimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
        EXPECT_TRUE(Inspection.visit_succeeded);
        EXPECT_TRUE(Inspection.metadata_valid);
        EXPECT_TRUE(Inspection.matches_authoritative_statistics);
    }

    Allocator.Shutdown();
}
