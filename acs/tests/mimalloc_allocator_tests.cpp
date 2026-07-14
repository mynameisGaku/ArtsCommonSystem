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

    const MimallocAllocationHistogram Histogram = Allocator.CaptureAllocationHistogram();
    EXPECT_EQ(Histogram.small.allocation_count, 1ull);
    EXPECT_EQ(Histogram.small.requested_bytes, 4096ull);
    EXPECT_EQ(Histogram.medium.allocation_count, 1ull);
    EXPECT_EQ(Histogram.medium.requested_bytes, 4097ull);
    EXPECT_EQ(Histogram.large.allocation_count, 1ull);
    EXPECT_EQ(Histogram.large.requested_bytes, 1024ull * 1024ull + 1ull);

    const MimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
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
    const MimallocHeapInspectionStatistics AlignedInspection = Allocator.InspectHeap();
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

    struct Context {
        FMimallocAllocator* allocator = nullptr;
        void* block_to_free = nullptr;
        void* allocated_block = nullptr;
    } TestContext;
    TestContext.allocator = &Allocator;
    TestContext.block_to_free = Allocator.Alloc(128u, 64u, FSourceLoc::Current());
    EXPECT_TRUE(TestContext.block_to_free != nullptr);

    auto ThreadResult = FThread::Spawn(
        [](void* UserData) {
            auto* WorkerContext = static_cast<Context*>(UserData);
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

    struct Context {
        FMimallocAllocator* allocator = nullptr;
        TAtomic<u32> start{0};
        TAtomic<u32> success_count{0};
        void* blocks[4] = {};
    } TestContext;

    struct WorkerInput {
        Context* context = nullptr;
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
                auto* Input = static_cast<WorkerInput*>(UserData);
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
        const MimallocHeapInspectionStatistics Inspection = Allocator.InspectHeap();
        EXPECT_TRUE(Inspection.visit_succeeded);
        EXPECT_TRUE(Inspection.metadata_valid);
        EXPECT_TRUE(Inspection.matches_authoritative_statistics);
        EXPECT_EQ(Inspection.allocation_count, 0ull);
        Allocator.Shutdown();
        EXPECT_FALSE(Allocator.IsInitialized());
    }
}
