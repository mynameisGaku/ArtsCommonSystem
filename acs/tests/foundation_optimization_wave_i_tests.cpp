// SPDX-License-Identifier: Apache-2.0
#include "test/Expect.h"
#include "test/Test.h"

#include "foundation/EndianSerialization.h"
#include "foundation/EnumLookup.h"
#include "foundation/Move.h"
#include "memory/ArenaAllocator.h"
#include "memory/ArenaAllocatorDiagnostics.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"
#include "threading/ThreadPoolDiagnostics.h"

using namespace acs;

static_assert(sizeof(FThreadPoolDiagnostics) == 96u);
static_assert(alignof(FThreadPoolDiagnostics) == 8u);
static_assert(sizeof(FParallelForDiagnostics) == 40u);
static_assert(alignof(FParallelForDiagnostics) == 8u);
static_assert(sizeof(FArenaAllocatorDiagnostics) == 40u);
static_assert(alignof(FArenaAllocatorDiagnostics) == 8u);
static_assert(!endian_detail::IsEndianSerializableV<bool>);

namespace {

/** constexpr lookup の境界を検証する連続 enum。 */
enum class EWaveITestValue : u8 {
    /** 先頭値。 */
    First = 0u,

    /** 中央値。 */
    Second = 1u,

    /** 末尾値。 */
    Third = 2u,
};

/** EWaveITestValue と同じ index 順の名前列。 */
constexpr const char* kWaveITestNames[]{"First", "Second", "Third"};
/** compile-time 生成する enum lookup。 */
constexpr TContiguousEnumLookup<EWaveITestValue, 3u> kWaveITestLookup(kWaveITestNames);

static_assert(kWaveITestLookup.Contains(EWaveITestValue::First));
static_assert(kWaveITestLookup.Contains(EWaveITestValue::Third));
static_assert(!kWaveITestLookup.Contains(static_cast<EWaveITestValue>(3u)));
static_assert(kWaveITestLookup.Size() == 3u);

/** ParallelFor が処理した index 数を数える context。 */
struct FParallelForCountContext {
    /** 実行済み index 数。 */
    TAtomic<u32> count{0u};
};

/**
 * ParallelFor の各 index を 1 件として記録する。
 *
 * @param Index 処理対象 index。
 * @param WorkerIndex 実行 worker index。
 * @param User 処理数の集約先。
 */
void CountParallelForIndex(u32 /*Index*/, u32 /*WorkerIndex*/, void* User) noexcept
{
    static_cast<FParallelForCountContext*>(User)->count.FetchAdd(1u);
}

/** arena Reset 競合 stress の共有状態。 */
struct FArenaResetStressContext {
    /** 確保対象 arena。 */
    CArenaAllocator* arena = nullptr;

    /** worker の終了要求。 */
    TAtomic<u32> stop{0u};

    /** 成功した確保回数。 */
    TAtomic<u32> successes{0u};

    /** Reset gate に拒否された確保回数。 */
    TAtomic<u32> rejections{0u};

    /** worker entry へ到達した thread 数。 */
    TAtomic<u32> ready_workers{0u};
};

/**
 * Reset と並行して小領域を確保し、gate の終了性を検証する。
 *
 * @param User arena stress の共有状態。
 */
void RunArenaResetStress(void* User) noexcept
{
    /** 共有する stress 状態。 */
    auto& Context = *static_cast<FArenaResetStressContext*>(User);
    Context.ready_workers.FetchAdd(1u);
    while (Context.stop.Load(EMemoryOrder::Acquire) == 0u) {
        /** 現在の世代から取得を試みた領域。 */
        void* const Allocation = Context.arena->Alloc(24u, 8u, FSourceLoc::Current());
        if (Allocation) Context.successes.FetchAdd(1u);
        else Context.rejections.FetchAdd(1u);
    }
}

/** Shutdown 競合中の ParallelFor body を停止させる gate。 */
struct FParallelForShutdownGate {
    /** body の解放条件。 */
    TAtomic<u32> release{0u};

    /** body へ入った回数。 */
    TAtomic<u32> entered{0u};
};

/**
 * Shutdown 開始まで worker を gate で待機させる。
 *
 * @param Index 処理対象 index。
 * @param WorkerIndex 実行 worker index。
 * @param User body 群を停止する共有 gate。
 */
void WaitInParallelForBody(u32 /*Index*/, u32 /*WorkerIndex*/, void* User) noexcept
{
    /** body 群が共有する gate。 */
    auto& Gate = *static_cast<FParallelForShutdownGate*>(User);
    Gate.entered.FetchAdd(1u);
    while (Gate.release.Load(EMemoryOrder::Acquire) == 0u) Yield();
}

/** 外部 thread から ParallelFor を実行する状態。 */
struct FParallelForCallerContext {
    /** body が共有する gate。 */
    FParallelForShutdownGate* gate = nullptr;

    /** 呼び出しが戻ったら 1。 */
    TAtomic<u32> finished{0u};
};

/**
 * Shutdown と競合させる ParallelFor 呼び出し。
 *
 * @param User 外部 ParallelFor 呼び出しの状態。
 */
void RunParallelForCaller(void* User) noexcept
{
    /** 呼び出し結果を書き戻す状態。 */
    auto& Context = *static_cast<FParallelForCallerContext*>(User);
    (void)CThreadPool::ParallelFor(0u, 128u, 1u, &WaitInParallelForBody, Context.gate);
    Context.finished.Store(1u, EMemoryOrder::Release);
}

/** 外部 thread から ThreadPool を終了する状態。 */
struct FShutdownCallerContext {
    /** Shutdown が戻ったら 1。 */
    TAtomic<u32> finished{0u};
};

/**
 * 所有者 thread 相当の外部呼び出しで Shutdown を実行する。
 *
 * @param User 終了結果を書き戻す状態。
 */
void RunThreadPoolShutdown(void* User) noexcept
{
    /** 終了完了を書き戻す状態。 */
    auto& Context = *static_cast<FShutdownCallerContext*>(User);
    CThreadPool::Shutdown();
    Context.finished.Store(1u, EMemoryOrder::Release);
}

/**
 * NUL 終端文字列が byte 単位で一致するかを返す。
 *
 * @param Left 左辺文字列。
 * @param Right 右辺文字列。
 * @return 同じ文字列なら true。
 */
bool TextEquals(const char* Left, const char* Right) noexcept
{
    if (!Left || !Right) return Left == Right;
    while (*Left != '\0' && *Right != '\0') {
        if (*Left != *Right) return false;
        ++Left;
        ++Right;
    }
    return *Left == *Right;
}

} // namespace

ACS_TEST(FoundationOptimizationWaveI, EndianAndEnumTablesAreCanonical)
{
    /** exact byte 比較に使う固定長 buffer。 */
    u8 Bytes[8]{};
    WriteLittleEndian(Bytes, u16{0x1234u});
    EXPECT_EQ(Bytes[0], u8{0x34u});
    EXPECT_EQ(Bytes[1], u8{0x12u});
    EXPECT_EQ(ReadLittleEndian<u16>(Bytes), u16{0x1234u});

    WriteLittleEndian(Bytes, u32{0x12345678u});
    /** u32 の正準 little endian byte 列。 */
    constexpr u8 kExpectedU32[]{0x78u, 0x56u, 0x34u, 0x12u};
    /** u32 の各 byte 位置。 */
    for (usize Index = 0u; Index < sizeof(kExpectedU32); ++Index) EXPECT_EQ(Bytes[Index], kExpectedU32[Index]);
    EXPECT_EQ(ReadLittleEndian<u32>(Bytes), u32{0x12345678u});

    WriteLittleEndian(Bytes, u64{0x0123456789ABCDEFull});
    /** u64 の正準 little endian byte 列。 */
    constexpr u8 kExpectedU64[]{0xEFu, 0xCDu, 0xABu, 0x89u, 0x67u, 0x45u, 0x23u, 0x01u};
    /** u64 の各 byte 位置。 */
    for (usize Index = 0u; Index < sizeof(kExpectedU64); ++Index) EXPECT_EQ(Bytes[Index], kExpectedU64[Index]);
    EXPECT_EQ(ReadLittleEndian<u64>(Bytes), u64{0x0123456789ABCDEFull});

    WriteLittleEndian(Bytes, 1.0f);
    /** f32 1.0 の正準 little endian byte 列。 */
    constexpr u8 kExpectedF32[]{0x00u, 0x00u, 0x80u, 0x3Fu};
    /** f32 の各 byte 位置。 */
    for (usize Index = 0u; Index < sizeof(kExpectedF32); ++Index) EXPECT_EQ(Bytes[Index], kExpectedF32[Index]);
    EXPECT_EQ(ReadLittleEndian<f32>(Bytes), 1.0f);

    WriteLittleEndian(Bytes, EWaveITestValue::Third);
    EXPECT_EQ(Bytes[0], u8{2u});
    EXPECT_EQ(ReadLittleEndian<EWaveITestValue>(Bytes), EWaveITestValue::Third);
    EXPECT_TRUE(TextEquals(kWaveITestLookup.Name(EWaveITestValue::Second), "Second"));
    EXPECT_TRUE(TextEquals(kWaveITestLookup.Name(static_cast<EWaveITestValue>(255u)), "Unknown"));
}

ACS_TEST(FoundationOptimizationWaveI, ArenaBatchesAndResetsWithBoundedPageVisits)
{
    /** batch 統計を検証する arena。 */
    CArenaAllocator BatchArena(1024u);
    /** batch API が返す 8 領域。 */
    void* Allocations[8]{};
    EXPECT_TRUE(BatchArena.AllocBatch(Allocations, 8u, 12u, 16u));
    /** alignment と隣接間隔を検証する領域 index。 */
    for (usize Index = 0u; Index < 8u; ++Index) {
        EXPECT_TRUE(Allocations[Index] != nullptr);
        EXPECT_EQ(reinterpret_cast<uptr>(Allocations[Index]) % 16u, uptr{0u});
        if (Index != 0u) {
            EXPECT_EQ(reinterpret_cast<uptr>(Allocations[Index]) - reinterpret_cast<uptr>(Allocations[Index - 1u]), uptr{16u});
        }
    }
    EXPECT_EQ(BatchArena.BytesAllocated(), u64{96u});
    EXPECT_EQ(BatchArena.AllocationCount(), u64{8u});
    /** 成功した batch の診断値。 */
    const FArenaAllocatorDiagnostics BatchDiagnostics = BatchArena.Diagnostics();
    EXPECT_EQ(BatchDiagnostics.batch_allocations, u64{1u});
    EXPECT_EQ(BatchDiagnostics.batch_suballocations, u64{8u});

    /** 失敗時に nullptr へ戻される出力列。 */
    void* FailedOutputs[2]{reinterpret_cast<void*>(uptr{1u}), reinterpret_cast<void*>(uptr{1u})};
    EXPECT_FALSE(BatchArena.AllocBatch(FailedOutputs, 2u, 8u, 128u * 1024u));
    EXPECT_TRUE(FailedOutputs[0] == nullptr);
    EXPECT_TRUE(FailedOutputs[1] == nullptr);
    EXPECT_EQ(BatchArena.BytesAllocated(), u64{96u});
    EXPECT_EQ(BatchArena.AllocationCount(), u64{8u});

    /** 世代 reset と page 再利用を検証する arena。 */
    CArenaAllocator ResetArena(64u);
    /** 複数 page を作り、旧世代判定にも使う領域列。 */
    void* OldAllocations[5]{};
    /** 旧世代に確保する page index。 */
    for (usize Index = 0u; Index < 5u; ++Index) {
        OldAllocations[Index] = ResetArena.Alloc(80u, 8u, FSourceLoc::Current());
        EXPECT_TRUE(OldAllocations[Index] != nullptr);
    }
    EXPECT_EQ(ResetArena.Diagnostics().retained_pages, u64{5u});
    ResetArena.Reset(false);
    /** 保持 reset 直後の診断値。 */
    const FArenaAllocatorDiagnostics ResetDiagnostics = ResetArena.Diagnostics();
    EXPECT_EQ(ResetDiagnostics.retained_pages, u64{5u});
    EXPECT_EQ(ResetDiagnostics.last_reset_page_visits, u64{1u});
    /** 無効化を検証する旧世代領域。 */
    for (void* Allocation : OldAllocations) EXPECT_FALSE(ResetArena.ContainsCurrentAllocationRange(Allocation, 1u));

    /** 保持 page を再利用する確保 index。 */
    for (usize Index = 0u; Index < 5u; ++Index) EXPECT_TRUE(ResetArena.Alloc(80u, 8u, FSourceLoc::Current()) != nullptr);
    /** 遅延 page 初期化後の診断値。 */
    const FArenaAllocatorDiagnostics ReuseDiagnostics = ResetArena.Diagnostics();
    EXPECT_EQ(ReuseDiagnostics.retained_pages, u64{5u});
    EXPECT_EQ(ReuseDiagnostics.lazy_page_resets, u64{4u});

    ResetArena.Reset(true);
    /** 全 page 解放後の診断値。 */
    const FArenaAllocatorDiagnostics ReleaseDiagnostics = ResetArena.Diagnostics();
    EXPECT_EQ(ReleaseDiagnostics.retained_pages, u64{0u});
    EXPECT_EQ(ReleaseDiagnostics.last_reset_page_visits, u64{5u});
}

ACS_TEST(FoundationOptimizationWaveI, ArenaResetGateSurvivesConcurrentAllocations)
{
    /** Reset と並行確保を行う arena。 */
    CArenaAllocator Arena(4096u);
    /** worker 群が共有する stress 状態。 */
    FArenaResetStressContext Context{};
    Context.arena = &Arena;
    /** Reset と競合する allocator worker 群。 */
    FThread Workers[4];
    /** 生成する allocator worker の index。 */
    for (usize Index = 0u; Index < 4u; ++Index) {
        /** 現在 worker の生成結果。 */
        TResult<FThread> Result = FThread::Spawn(&RunArenaResetStress, &Context);
        EXPECT_TRUE(Result.IsOk());
        if (Result.IsOk()) Workers[Index] = Move(Result.Value());
    }

    /** 全 worker の起動を待つ反復 index。 */
    for (u32 WaitIndex = 0u; WaitIndex < 10000u && Context.ready_workers.Load(EMemoryOrder::Acquire) != 4u; ++WaitIndex) Yield();
    EXPECT_EQ(Context.ready_workers.Load(EMemoryOrder::Acquire), u32{4u});
    /** 最初の確保成功を待つ反復 index。 */
    for (u32 WaitIndex = 0u; WaitIndex < 10000u && Context.successes.Load(EMemoryOrder::Acquire) == 0u; ++WaitIndex) Yield();
    /** 競合させる Reset の反復 index。 */
    for (u32 ResetIndex = 0u; ResetIndex < 500u; ++ResetIndex) Arena.Reset(false);
    Context.stop.Store(1u, EMemoryOrder::Release);
    /** 終了を待つ allocator worker。 */
    for (FThread& Worker : Workers) Worker.Join();
    EXPECT_TRUE(Context.successes.Load(EMemoryOrder::Acquire) != 0u);
    Arena.Reset(false);
    EXPECT_TRUE(Arena.Diagnostics().last_reset_page_visits <= 1u);
    Arena.Reset(true);
    EXPECT_EQ(Arena.Diagnostics().retained_pages, u64{0u});
}

ACS_TEST(FoundationOptimizationWaveI, ParallelForUsesInlineAndReusableBlocks)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4u).IsOk());
    CThreadPool::ResetDiagnostics();

    /** ParallelFor が処理した index 数。 */
    FParallelForCountContext Context{};
    EXPECT_TRUE(CThreadPool::ParallelFor(0u, 16u, 1u, &CountParallelForIndex, &Context).IsOk());
    EXPECT_TRUE(CThreadPool::ParallelFor(0u, 128u, 1u, &CountParallelForIndex, &Context).IsOk());
    EXPECT_EQ(Context.count.Load(EMemoryOrder::Acquire), u32{144u});

    /** inline と block 経路の診断値。 */
    const FParallelForDiagnostics Diagnostics = CThreadPool::CaptureParallelForDiagnostics();
    EXPECT_EQ(Diagnostics.inline_calls, u64{1u});
    EXPECT_EQ(Diagnostics.pool_blocks, u64{2u});
    EXPECT_EQ(Diagnostics.pool_blocks_in_use, u64{0u});
    EXPECT_EQ(Diagnostics.pool_blocks_high_water, u64{2u});
    EXPECT_EQ(Diagnostics.heap_blocks, u64{0u});
    CThreadPool::Shutdown();
}

ACS_TEST(FoundationOptimizationWaveI, ParallelForPoolCoversRepresentativeConcurrency)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4u).IsOk());
    CThreadPool::ResetDiagnostics();

    /** 同時実行する外部 ParallelFor 数。 */
    constexpr usize kCallerCount = 8u;
    /** 全 body の解放を制御する gate。 */
    FParallelForShutdownGate Gate{};
    /** 同時に ParallelFor を呼ぶ外部 thread の状態列。 */
    FParallelForCallerContext Contexts[kCallerCount]{};
    /** 同時呼び出しを保持する thread 列。 */
    FThread Callers[kCallerCount];
    /** 各 thread の生成成功状態。 */
    bool Spawned[kCallerCount]{};
    /** 生成する外部 caller の index。 */
    for (usize Index = 0u; Index < kCallerCount; ++Index) {
        Contexts[Index].gate = &Gate;
        /** 現在 caller の生成結果。 */
        TResult<FThread> Result = FThread::Spawn(&RunParallelForCaller, &Contexts[Index]);
        EXPECT_TRUE(Result.IsOk());
        if (Result.IsOk()) {
            Callers[Index] = Move(Result.Value());
            Spawned[Index] = true;
        }
    }

    /** 8 呼び出し x 2 block が同時貸し出しになるまでの待機上限。 */
    constexpr u64 kExpectedHighWater = 16u;
    /** 最大同時貸し出しを待つ反復 index。 */
    for (u32 WaitIndex = 0u; WaitIndex < 100000u; ++WaitIndex) {
        if (CThreadPool::CaptureParallelForDiagnostics().pool_blocks_in_use == kExpectedHighWater) break;
        Yield();
    }
    /** body 解放前の同時使用診断値。 */
    const FParallelForDiagnostics ActiveDiagnostics = CThreadPool::CaptureParallelForDiagnostics();
    EXPECT_EQ(ActiveDiagnostics.pool_blocks_in_use, kExpectedHighWater);
    EXPECT_EQ(ActiveDiagnostics.pool_blocks_high_water, kExpectedHighWater);
    EXPECT_EQ(ActiveDiagnostics.heap_blocks, u64{0u});

    Gate.release.Store(1u, EMemoryOrder::Release);
    /** 完了を待つ外部 caller の index。 */
    for (usize Index = 0u; Index < kCallerCount; ++Index) {
        if (Spawned[Index]) Callers[Index].Join();
        if (Spawned[Index]) EXPECT_EQ(Contexts[Index].finished.Load(EMemoryOrder::Acquire), u32{1u});
    }
    /** 全 caller 完了後の格納診断値。 */
    const FParallelForDiagnostics CompletedDiagnostics = CThreadPool::CaptureParallelForDiagnostics();
    EXPECT_EQ(CompletedDiagnostics.pool_blocks, kExpectedHighWater);
    EXPECT_EQ(CompletedDiagnostics.pool_blocks_in_use, u64{0u});
    EXPECT_EQ(CompletedDiagnostics.pool_blocks_high_water, kExpectedHighWater);
    EXPECT_EQ(CompletedDiagnostics.heap_blocks, u64{0u});
    CThreadPool::Shutdown();
}

ACS_TEST(FoundationOptimizationWaveI, ParallelForPinsStorageDuringShutdown)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4u).IsOk());

    /** body の停止と入場数を管理する gate。 */
    FParallelForShutdownGate Gate{};
    /** 外部 ParallelFor 呼び出しの状態。 */
    FParallelForCallerContext ParallelContext{};
    ParallelContext.gate = &Gate;
    /** 外部 ParallelFor thread の生成結果。 */
    TResult<FThread> ParallelResult = FThread::Spawn(&RunParallelForCaller, &ParallelContext);
    EXPECT_TRUE(ParallelResult.IsOk());

    /** 最初の body 入場を待つ反復 index。 */
    for (u32 WaitIndex = 0u; WaitIndex < 10000u && Gate.entered.Load(EMemoryOrder::Acquire) == 0u; ++WaitIndex) Yield();
    EXPECT_TRUE(Gate.entered.Load(EMemoryOrder::Acquire) != 0u);

    /** 外部 Shutdown 呼び出しの状態。 */
    FShutdownCallerContext ShutdownContext{};
    /** 外部 Shutdown thread の生成結果。 */
    TResult<FThread> ShutdownResult = FThread::Spawn(&RunThreadPoolShutdown, &ShutdownContext);
    EXPECT_TRUE(ShutdownResult.IsOk());
    Gate.release.Store(1u, EMemoryOrder::Release);

    if (ParallelResult.IsOk()) ParallelResult.Value().Join();
    if (ShutdownResult.IsOk()) ShutdownResult.Value().Join();
    EXPECT_EQ(ParallelContext.finished.Load(EMemoryOrder::Acquire), u32{1u});
    EXPECT_EQ(ShutdownContext.finished.Load(EMemoryOrder::Acquire), u32{1u});
    EXPECT_EQ(CThreadPool::WorkerCount(), u32{0u});
}
