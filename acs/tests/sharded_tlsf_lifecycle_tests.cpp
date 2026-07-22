// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - FShardedTlsfAllocator ライフサイクル競合テスト
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Move.h"
#include "memory/ShardedTlsf.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

ACS_TEST(ShardedTlsfAllocator, LifecycleGateRejectsShutdownRacesAndSupportsReinitialization)
{
    constexpr u32 kWorkerCount = 4u;
    constexpr u64 kRequiredSuccessfulAllocations = 256u;
    constexpr u32 kWaitLimit = 500000u;

    FShardedTlsfAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(64ull * 1024ull * 1024ull, 4ull * 1024ull * 1024ull, 4u).IsOk());
    Allocator.EnableThreadCache();
    EXPECT_TRUE(Allocator.ThreadCacheEnabled());

    struct FRaceContext
    {
        FShardedTlsfAllocator* Allocator = nullptr;
        TAtomic<u32> Ready{0u};
        TAtomic<u32> Start{0u};
        TAtomic<u32> Stop{0u};
        TAtomic<u32> ShutdownObservedMask{0u};
        TAtomic<u64> SuccessfulAllocations{0u};
        TAtomic<u64> RejectedAllocations{0u};
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
        auto WorkerResult = FThread::Spawn(
            [](void* User)
            {
                auto* Input = static_cast<FWorkerInput*>(User);
                FRaceContext* const WorkerContext = Input->Context;
                u32 Iteration = 0u;
                WorkerContext->Ready.FetchAdd(1u);
                while (WorkerContext->Start.Load(EMemoryOrder::Acquire) == 0u)
                {
                    Yield();
                }

                while (WorkerContext->Stop.Load(EMemoryOrder::Acquire) == 0u)
                {
                    void* Allocation = WorkerContext->Allocator->Alloc(
                        64u + (Iteration & 127u), 16u, FSourceLoc::Current());
                    if (!Allocation)
                    {
                        WorkerContext->RejectedAllocations.FetchAdd(1u);
                        WorkerContext->ShutdownObservedMask.FetchOr(1u << Input->WorkerIndex);
                        WorkerContext->Allocator->FlushCurrentThreadCache();
                        ++Iteration;
                        continue;
                    }

                    WorkerContext->SuccessfulAllocations.FetchAdd(1u);
                    void* const Reallocated = WorkerContext->Allocator->Realloc(
                        Allocation, 64u + (Iteration & 127u), 256u + (Iteration & 255u),
                        16u, FSourceLoc::Current());
                    if (Reallocated)
                    {
                        WorkerContext->Allocator->Free(Reallocated);
                    }
                    else
                    {
                        // Shutdown が呼出間で受付を閉じた場合、この Free も安全に拒否される。
                        WorkerContext->Allocator->Free(Allocation);
                    }

                    if ((Iteration & 15u) == 0u)
                    {
                        WorkerContext->Allocator->FlushCurrentThreadCache();
                    }
                    (void)WorkerContext->Allocator->BytesAllocated();
                    (void)WorkerContext->Allocator->AllocationCount();
                    (void)WorkerContext->Allocator->PeakBytes();
                    (void)WorkerContext->Allocator->ShardCount();
                    (void)WorkerContext->Allocator->ThreadCacheEnabled();
                    (void)WorkerContext->Allocator->Epoch();
                    (void)WorkerContext->Allocator->ValidateHeap();
                    ++Iteration;
                }
            },
            &Inputs[SpawnedWorkerCount]);

        EXPECT_TRUE(WorkerResult.IsOk());
        if (WorkerResult.IsOk())
        {
            Workers[SpawnedWorkerCount] = Move(WorkerResult.Value());
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

    // ワーカーを止めずに受付を閉じ、開始済み操作の完了後だけ VM が解放されることを検証する。
    Allocator.Shutdown();
    const u32 ExpectedShutdownObservedMask = (1u << SpawnedWorkerCount) - 1u;
    for (u32 WaitIteration = 0u;
         WaitIteration < kWaitLimit &&
         Context.ShutdownObservedMask.Load(EMemoryOrder::Acquire) != ExpectedShutdownObservedMask;
         ++WaitIteration)
    {
        Yield();
    }

    EXPECT_EQ(Context.ShutdownObservedMask.Load(EMemoryOrder::Acquire), ExpectedShutdownObservedMask);
    EXPECT_TRUE(Context.RejectedAllocations.Load(EMemoryOrder::Acquire) > 0u);

    alignas(16) u8 ForeignStorage[64]{};
    EXPECT_TRUE(Allocator.Alloc(64u, 16u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Realloc(nullptr, 0u, 64u, 16u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(Allocator.Realloc(ForeignStorage, sizeof(ForeignStorage), 0u, 16u,
                                  FSourceLoc::Current()) == ForeignStorage);
    Allocator.Free(ForeignStorage);
    EXPECT_EQ(Allocator.BytesAllocated(), 0ull);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_EQ(Allocator.PeakBytes(), 0ull);
    EXPECT_EQ(Allocator.ShardCount(), 0u);
    EXPECT_EQ(Allocator.Epoch(), 0ull);
    EXPECT_FALSE(Allocator.ThreadCacheEnabled());
    EXPECT_FALSE(Allocator.ValidateHeap());

    // 同じアドレスのアロケータを新世代で再開し、各ワーカーの旧世代 TLS キャッシュを破棄させる。
    const u64 SuccessfulAllocationsBeforeReinitialization =
        Context.SuccessfulAllocations.Load(EMemoryOrder::Acquire);
    EXPECT_TRUE(Allocator.Init(32ull * 1024ull * 1024ull, 2ull * 1024ull * 1024ull, 2u).IsOk());
    for (u32 WaitIteration = 0u;
         WaitIteration < kWaitLimit &&
         Context.SuccessfulAllocations.Load(EMemoryOrder::Acquire) <
             SuccessfulAllocationsBeforeReinitialization + kRequiredSuccessfulAllocations;
         ++WaitIteration)
    {
        Yield();
    }
    EXPECT_TRUE(Context.SuccessfulAllocations.Load(EMemoryOrder::Acquire) >=
                SuccessfulAllocationsBeforeReinitialization + kRequiredSuccessfulAllocations);

    // ワーカー終了時の TLS デストラクタと Shutdown の寿命レジストリ操作を競合させる。
    Context.Stop.Store(1u, EMemoryOrder::Release);
    Allocator.Shutdown();
    for (u32 WorkerIndex = 0u; WorkerIndex < SpawnedWorkerCount; ++WorkerIndex)
    {
        Workers[WorkerIndex].Join();
    }

    EXPECT_TRUE(Allocator.Init(32ull * 1024ull * 1024ull, 2ull * 1024ull * 1024ull, 2u).IsOk());
    EXPECT_EQ(Allocator.ShardCount(), 2u);
    EXPECT_TRUE(Allocator.Epoch() != 0u);
    EXPECT_TRUE(Allocator.ThreadCacheEnabled());
    void* const ReinitializedAllocation = Allocator.Alloc(512u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(ReinitializedAllocation != nullptr);
    Allocator.Free(ReinitializedAllocation);
    Allocator.FlushCurrentThreadCache();
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(Allocator.ValidateHeap());
    Allocator.Shutdown();
}

ACS_TEST(ShardedTlsfAllocator, ConcurrentInitializationAndShutdownAreSerialized)
{
    constexpr u32 kThreadCount = 4u;
    constexpr u32 kLifecycleCycles = 16u;

    FShardedTlsfAllocator Allocator;
    struct FControlContext
    {
        FShardedTlsfAllocator* Allocator = nullptr;
        TAtomic<u32> Ready{0u};
        TAtomic<u32> Start{0u};
        TAtomic<u32> SuccessfulInitializations{0u};
    } Context;
    Context.Allocator = &Allocator;

    FThread Initializers[kThreadCount];
    u32 InitializerCount = 0u;
    for (u32 Index = 0u; Index < kThreadCount; ++Index)
    {
        auto ThreadResult = FThread::Spawn(
            [](void* User)
            {
                auto* ThreadContext = static_cast<FControlContext*>(User);
                ThreadContext->Ready.FetchAdd(1u);
                while (ThreadContext->Start.Load(EMemoryOrder::Acquire) == 0u)
                {
                    Yield();
                }
                if (ThreadContext->Allocator->Init(
                        16ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull, 2u).IsOk())
                {
                    ThreadContext->SuccessfulInitializations.FetchAdd(1u);
                }
            },
            &Context);
        EXPECT_TRUE(ThreadResult.IsOk());
        if (ThreadResult.IsOk())
        {
            Initializers[InitializerCount++] = Move(ThreadResult.Value());
        }
    }

    while (Context.Ready.Load(EMemoryOrder::Acquire) < InitializerCount)
    {
        Yield();
    }
    Context.Start.Store(1u, EMemoryOrder::Release);
    for (u32 Index = 0u; Index < InitializerCount; ++Index)
    {
        Initializers[Index].Join();
    }
    EXPECT_EQ(Context.SuccessfulInitializations.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(Allocator.ShardCount(), 2u);

    FThread Controllers[kThreadCount];
    u32 ControllerCount = 0u;
    for (u32 Index = 0u; Index < kThreadCount; ++Index)
    {
        auto ThreadResult = FThread::Spawn(
            [](void* User)
            {
                auto* ThreadContext = static_cast<FControlContext*>(User);
                for (u32 Cycle = 0u; Cycle < kLifecycleCycles; ++Cycle)
                {
                    ThreadContext->Allocator->Shutdown();
                    (void)ThreadContext->Allocator->Init(
                        16ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull, 2u);
                }
            },
            &Context);
        EXPECT_TRUE(ThreadResult.IsOk());
        if (ThreadResult.IsOk())
        {
            Controllers[ControllerCount++] = Move(ThreadResult.Value());
        }
    }
    for (u32 Index = 0u; Index < ControllerCount; ++Index)
    {
        Controllers[Index].Join();
    }

    Allocator.Shutdown();
    EXPECT_TRUE(Allocator.Init(16ull * 1024ull * 1024ull, 1ull * 1024ull * 1024ull, 2u).IsOk());
    EXPECT_EQ(Allocator.ShardCount(), 2u);
    EXPECT_TRUE(Allocator.ValidateHeap());
    Allocator.Shutdown();
}
