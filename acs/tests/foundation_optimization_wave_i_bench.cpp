// SPDX-License-Identifier: Apache-2.0
#include "foundation/EndianSerialization.h"
#include "memory/ArenaAllocator.h"
#include "platform/Time.h"
#include "threading/Atomic.h"
#include "threading/ThreadPool.h"

#include <cstdio>

using namespace acs;

namespace {

/** 最適化対象が除去されないよう結果を集約する。 */
volatile u64 g_WaveISink = 0u;

/** ParallelFor の実行回数を集約する状態。 */
struct FParallelBenchContext {
    /** 実行済み index 数。 */
    TAtomic<u32> count{0u};
};

/**
 * ParallelFor の index を一件として記録する。
 *
 * @param Index 処理した index。
 * @param WorkerIndex 実行 worker index。
 * @param User 集約先の FParallelBenchContext。
 */
void CountParallelIndex(u32 Index, u32 /*WorkerIndex*/, void* User) noexcept
{
    static_cast<FParallelBenchContext*>(User)->count.FetchAdd(Index + 1u);
}

/**
 * 計測 tick 差を nanosecond へ変換する。
 *
 * @param Started 計測開始 tick。
 * @param Finished 計測終了 tick。
 * @return 経過 nanosecond。
 */
u64 ToNanoseconds(u64 Started, u64 Finished) noexcept
{
    return static_cast<u64>(Finished - Started) * 1000000000ull / static_cast<u64>(CClock::TicksPerSecond());
}

/** 個別確保と batch 確保の cursor 予約数と時間を比較する。 */
void BenchArenaBatch() noexcept
{
    /** 1 reset 世代で確保する領域数。 */
    constexpr usize kCount = 256u;
    /** 時間を集計する reset 反復数。 */
    constexpr usize kRepeats = 4000u;
    /** 各確保が返した領域列。 */
    void* Outputs[kCount]{};

    /** 個別確保を計測する arena。 */
    CArenaAllocator IndividualArena(64u * 1024u);
    /** 個別確保の開始 tick。 */
    const u64 IndividualStarted = CClock::Ticks();
    /** 個別確保を繰り返す reset 世代 index。 */
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        IndividualArena.Reset(false);
        /** 現在世代で確保する領域 index。 */
        for (usize Index = 0u; Index < kCount; ++Index) Outputs[Index] = IndividualArena.Alloc(24u, 8u, FSourceLoc::Current());
        g_WaveISink += reinterpret_cast<uptr>(Outputs[kCount - 1u]) != 0u ? 1u : 0u;
    }
    /** 個別確保の終了 tick。 */
    const u64 IndividualFinished = CClock::Ticks();
    /** 個別確保で返した利用者領域数。 */
    const u64 IndividualAllocations = IndividualArena.AllocationCount();

    /** batch 確保を計測する arena。 */
    CArenaAllocator BatchArena(64u * 1024u);
    /** batch 確保の開始 tick。 */
    const u64 BatchStarted = CClock::Ticks();
    /** batch 確保を繰り返す reset 世代 index。 */
    for (usize Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        BatchArena.Reset(false);
        (void)BatchArena.AllocBatch(Outputs, kCount, 24u, 8u);
        g_WaveISink += reinterpret_cast<uptr>(Outputs[kCount - 1u]) != 0u ? 1u : 0u;
    }
    /** batch 確保の終了 tick。 */
    const u64 BatchFinished = CClock::Ticks();
    /** batch 専用の診断値。 */
    const FArenaAllocatorDiagnostics BatchDiagnostics = BatchArena.Diagnostics();

    std::printf("T52 arena_individual_ns=%llu regions=%llu arena_batch_ns=%llu batch_calls=%llu batch_suballocations=%llu arena_size=%llu\n", static_cast<unsigned long long>(ToNanoseconds(IndividualStarted, IndividualFinished)), static_cast<unsigned long long>(IndividualAllocations), static_cast<unsigned long long>(ToNanoseconds(BatchStarted, BatchFinished)), static_cast<unsigned long long>(BatchDiagnostics.batch_allocations), static_cast<unsigned long long>(BatchDiagnostics.batch_suballocations), static_cast<unsigned long long>(sizeof(CArenaAllocator)));
}

/** ParallelFor の inline と固定 block 経路を計測する。 */
void BenchParallelForStorage() noexcept
{
    /** inline と block 経路を反復する回数。 */
    constexpr u32 kRepeats = 200u;
    CThreadPool::Shutdown();
    if (CThreadPool::Init(4u).IsErr()) return;
    CThreadPool::ResetDiagnostics();
    /** body が処理した index の集約先。 */
    FParallelBenchContext Context{};

    /** ParallelFor 計測の開始 tick。 */
    const u64 Started = CClock::Ticks();
    /** 2 種類の分割数を実行する反復 index。 */
    for (u32 Repeat = 0u; Repeat < kRepeats; ++Repeat) {
        (void)CThreadPool::ParallelFor(0u, 16u, 1u, &CountParallelIndex, &Context);
        (void)CThreadPool::ParallelFor(0u, 128u, 1u, &CountParallelIndex, &Context);
    }
    /** ParallelFor 計測の終了 tick。 */
    const u64 Finished = CClock::Ticks();
    /** 一時 context 格納経路の診断値。 */
    const FParallelForDiagnostics Diagnostics = CThreadPool::CaptureParallelForDiagnostics();
    g_WaveISink += Context.count.Load(EMemoryOrder::Acquire);
    std::printf("T41_T43_T44 parallel_for_ns=%llu inline_calls=%llu pool_blocks=%llu pool_high_water=%llu heap_blocks=%llu\n", static_cast<unsigned long long>(ToNanoseconds(Started, Finished)), static_cast<unsigned long long>(Diagnostics.inline_calls), static_cast<unsigned long long>(Diagnostics.pool_blocks), static_cast<unsigned long long>(Diagnostics.pool_blocks_high_water), static_cast<unsigned long long>(Diagnostics.heap_blocks));
    CThreadPool::Shutdown();
}

/** endian primitive の exact round-trip を反復計測する。 */
void BenchEndianPrimitive() noexcept
{
    /** endian round-trip を実行する値数。 */
    constexpr usize kRepeats = 2000000u;
    /** 正準 byte 列を保持する最大幅 buffer。 */
    u8 Bytes[8]{};
    /** endian 計測の開始 tick。 */
    const u64 Started = CClock::Ticks();
    /** 書き込みと読み戻しを行う値。 */
    for (usize Index = 0u; Index < kRepeats; ++Index) {
        WriteLittleEndian(Bytes, static_cast<u64>(Index));
        g_WaveISink += ReadLittleEndian<u64>(Bytes);
    }
    /** endian 計測の終了 tick。 */
    const u64 Finished = CClock::Ticks();
    std::printf("T51 endian_roundtrip_ns=%llu values=%llu\n", static_cast<unsigned long long>(ToNanoseconds(Started, Finished)), static_cast<unsigned long long>(kRepeats));
}

} // namespace

/** Wave I の診断 benchmark 群を順に実行する。 */
int main()
{
    BenchArenaBatch();
    BenchParallelForStorage();
    BenchEndianPrimitive();
    std::printf("wave_i_sink=%llu\n", static_cast<unsigned long long>(g_WaveISink));
    return 0;
}
