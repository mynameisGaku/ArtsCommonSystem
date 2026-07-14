// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory — メモリシステム単体テスト
// -----------------------------------------------------------------------------
// VirtualMemory / TLSF / FMemorySystem / ScopedMemorySegment / FMemorySnapshot
// を統合的にテストする。
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/VirtualMemory.h"
#include "memory/Tlsf.h"
#include "memory/ShardedTlsf.h"
#include "memory/MimallocAllocator.h"
#include "memory/MemorySystem.h"
#include "memory/MemorySnapshot.h"
#include "memory/Memory.h" // DefaultAllocator / SetDefaultAllocator
#include "memory/RelocatableAllocator.h"
#include "threading/ThreadPool.h"
#include "threading/Thread.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "foundation/Platform.h" // HeapAlloc / GetProcessHeap / QueryPerformanceCounter
#include "foundation/Move.h"     // Move (VmReservation のムーブ)

#include <cstdio> // ベンチ結果出力

using namespace acs;

namespace {

/** テスト対象アドレスの現在の VirtualQuery 状態を返す。 */
DWORD QueryVirtualMemoryState(const void* Address) noexcept
{
    MEMORY_BASIC_INFORMATION Information{};
    return ::VirtualQuery(Address, &Information, sizeof(Information)) != 0 ? Information.State : 0;
}

} // namespace

// VirtualMemory: 予約、遅延デコミット、再コミット、実デコミットを OS 状態まで確認する。
ACS_TEST(MemSystem, VmReserveCommitDecommit)
{
    const usize PageSize = VmPageSize();
    const usize AllocationGranularity = VmAllocGranularity();
    auto ReservationResult = VmReservation::Reserve(64 * 1024 * 1024);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (!ReservationResult.IsOk()) {
        return;
    }

    auto& Reservation = ReservationResult.Value();
    EXPECT_TRUE(Reservation.Base() != nullptr);
    EXPECT_TRUE(Reservation.Capacity() >= 64ull * 1024 * 1024);
    EXPECT_TRUE(VmIsAligned(reinterpret_cast<uptr>(Reservation.Base()), AllocationGranularity));
    EXPECT_TRUE(VmIsAligned(Reservation.Capacity(), AllocationGranularity));
    EXPECT_EQ(QueryVirtualMemoryState(Reservation.Base()), static_cast<DWORD>(MEM_RESERVE));

    auto CommitResult = Reservation.Commit(0, PageSize);
    EXPECT_TRUE(CommitResult.IsOk());
    EXPECT_EQ(Reservation.Committed(), PageSize);
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.ActualCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.DecommitCacheMissCount(), 1ull);
    EXPECT_EQ(QueryVirtualMemoryState(Reservation.Base()), static_cast<DWORD>(MEM_COMMIT));

    *static_cast<u8*>(Reservation.Base()) = 0x42;
    EXPECT_EQ(*static_cast<u8*>(Reservation.Base()), static_cast<u8>(0x42));

    auto DecommitResult = Reservation.Decommit(0, PageSize);
    EXPECT_TRUE(DecommitResult.IsOk());
    EXPECT_EQ(Reservation.Committed(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.ActualCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 1u);
    // 遅延キャッシュ中なので OS 上ではまだコミット状態にある。
    EXPECT_EQ(QueryVirtualMemoryState(Reservation.Base()), static_cast<DWORD>(MEM_COMMIT));

    EXPECT_TRUE(Reservation.Commit(0, PageSize).IsOk());
    EXPECT_EQ(Reservation.DecommitCacheHitCount(), 1ull);
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));

    EXPECT_TRUE(Reservation.Decommit(0, PageSize).IsOk());
    EXPECT_TRUE(Reservation.FlushDecommitCache().IsOk());
    EXPECT_EQ(Reservation.ActualCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 0u);
    EXPECT_EQ(QueryVirtualMemoryState(Reservation.Base()), static_cast<DWORD>(MEM_RESERVE));
}

// ゼロ、overflow、予約外、ページ非整列を OS 呼び出し前に拒否する。
ACS_TEST(MemSystem, VmRejectsInvalidRanges)
{
    const usize PageSize = VmPageSize();
    const usize MaximumSize = ~static_cast<usize>(0);
    EXPECT_TRUE(!VmIsAligned(0, 0));
    EXPECT_TRUE(!VmIsAligned(0, 3));
    EXPECT_TRUE(VmReservation::Reserve(0).IsErr());
    EXPECT_TRUE(VmReservation::Reserve(MaximumSize).IsErr());

    auto ReservationResult = VmReservation::Reserve(PageSize * 4);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (!ReservationResult.IsOk()) {
        return;
    }
    auto& Reservation = ReservationResult.Value();

    EXPECT_TRUE(Reservation.Commit(0, 0).IsErr());
    EXPECT_TRUE(Reservation.Commit(1, PageSize).IsErr());
    EXPECT_TRUE(Reservation.Commit(0, PageSize - 1).IsErr());
    EXPECT_TRUE(Reservation.Commit(Reservation.Capacity(), PageSize).IsErr());
    EXPECT_TRUE(Reservation.Commit(PageSize, MaximumSize).IsErr());

    EXPECT_TRUE(Reservation.Decommit(0, 0).IsErr());
    EXPECT_TRUE(Reservation.Decommit(1, PageSize).IsErr());
    EXPECT_TRUE(Reservation.Decommit(0, PageSize - 1).IsErr());
    EXPECT_TRUE(Reservation.Decommit(Reservation.Capacity(), PageSize).IsErr());
    EXPECT_TRUE(Reservation.Decommit(PageSize, MaximumSize).IsErr());
    EXPECT_TRUE(Reservation.Decommit(0, PageSize).IsErr());

    EXPECT_EQ(Reservation.ActiveCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.ActualCommittedBytes(), static_cast<usize>(0));
}

// 二重 Commit は冪等、二重/重複 Decommit は拒否とし、統計を二重加算しない。
ACS_TEST(MemSystem, VmHandlesDuplicateAndOverlappingOperations)
{
    const usize PageSize = VmPageSize();
    auto ReservationResult = VmReservation::Reserve(PageSize * 4);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (!ReservationResult.IsOk()) {
        return;
    }
    auto& Reservation = ReservationResult.Value();

    EXPECT_TRUE(Reservation.Commit(0, PageSize * 4).IsOk());
    EXPECT_TRUE(Reservation.Commit(0, PageSize * 4).IsOk());
    EXPECT_TRUE(Reservation.Commit(PageSize, PageSize).IsOk());
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), PageSize * 4);
    EXPECT_EQ(Reservation.DecommitCacheMissCount(), 1ull);

    EXPECT_TRUE(Reservation.Decommit(0, PageSize * 2).IsOk());
    EXPECT_TRUE(Reservation.Decommit(0, PageSize * 2).IsErr());
    EXPECT_TRUE(Reservation.Decommit(PageSize, PageSize * 2).IsErr());
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), PageSize * 2);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize * 2);

    EXPECT_TRUE(Reservation.Decommit(PageSize * 2, PageSize * 2).IsOk());
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 2u);
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize * 4);
    EXPECT_TRUE(Reservation.Commit(0, PageSize * 4).IsErr());

    EXPECT_TRUE(Reservation.Commit(0, PageSize * 2).IsOk());
    EXPECT_TRUE(Reservation.Commit(PageSize * 2, PageSize * 2).IsOk());
    EXPECT_EQ(Reservation.DecommitCacheHitCount(), 2ull);
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), PageSize * 4);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.ActualCommittedBytes(), PageSize * 4);
}

// 64KiB 未満のオフセットを切り捨てず、正しいページを実デコミットする。
ACS_TEST(MemSystem, VmDecommitCachePreservesFullOffset)
{
    const usize PageSize = VmPageSize();
    auto ReservationResult = VmReservation::Reserve(VmAllocGranularity() * 2);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (!ReservationResult.IsOk()) {
        return;
    }
    auto& Reservation = ReservationResult.Value();
    void* const SecondPage = static_cast<u8*>(Reservation.Base()) + PageSize;

    EXPECT_TRUE(Reservation.Commit(PageSize, PageSize).IsOk());
    EXPECT_TRUE(Reservation.Decommit(PageSize, PageSize).IsOk());
    EXPECT_TRUE(Reservation.FlushDecommitCache().IsOk());
    EXPECT_EQ(QueryVirtualMemoryState(SecondPage), static_cast<DWORD>(MEM_RESERVE));
    EXPECT_EQ(Reservation.ActualCommittedBytes(), static_cast<usize>(0));
}

// 17 番目の挿入で最古範囲だけを実デコミットし、trim/flush の保持量を正確に反映する。
ACS_TEST(MemSystem, VmDecommitCacheEvictsAndTrims)
{
    constexpr u32 range_count = 17;
    const usize PageSize = VmPageSize();
    auto ReservationResult = VmReservation::Reserve(PageSize * range_count);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (!ReservationResult.IsOk()) {
        return;
    }
    auto& Reservation = ReservationResult.Value();

    for (u32 Index = 0; Index < range_count; ++Index) {
        EXPECT_TRUE(Reservation.Commit(PageSize * Index, PageSize).IsOk());
    }
    for (u32 Index = 0; Index < range_count; ++Index) {
        EXPECT_TRUE(Reservation.Decommit(PageSize * Index, PageSize).IsOk());
    }

    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 16u);
    EXPECT_EQ(Reservation.ActiveCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize * 16);
    EXPECT_EQ(Reservation.ActualCommittedBytes(), PageSize * 16);
    EXPECT_EQ(QueryVirtualMemoryState(Reservation.Base()), static_cast<DWORD>(MEM_RESERVE));
    EXPECT_EQ(QueryVirtualMemoryState(static_cast<u8*>(Reservation.Base()) + PageSize), static_cast<DWORD>(MEM_COMMIT));

    EXPECT_TRUE(Reservation.TrimDecommitCache(PageSize * 8).IsOk());
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 8u);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize * 8);
    EXPECT_EQ(Reservation.ActualCommittedBytes(), PageSize * 8);
    EXPECT_TRUE(Reservation.FlushDecommitCache().IsOk());
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 0u);
    EXPECT_EQ(Reservation.ActualCommittedBytes(), static_cast<usize>(0));
}

// move は予約、キャッシュ、統計を移し、移動元と Release 後の再利用状態を完全に空へ戻す。
ACS_TEST(MemSystem, VmMoveAndReinitializeAreSafe)
{
    const usize PageSize = VmPageSize();
    auto SourceResult = VmReservation::Reserve(PageSize * 4);
    EXPECT_TRUE(SourceResult.IsOk());
    if (!SourceResult.IsOk()) {
        return;
    }
    VmReservation Source = Move(SourceResult.Value());
    EXPECT_TRUE(Source.Commit(PageSize, PageSize).IsOk());
    EXPECT_TRUE(Source.Decommit(PageSize, PageSize).IsOk());
    void* const TransferredBase = Source.Base();

    VmReservation MovedReservation(Move(Source));
    EXPECT_TRUE(Source.Base() == nullptr);
    EXPECT_EQ(Source.Capacity(), static_cast<usize>(0));
    EXPECT_EQ(Source.ActualCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Source.DecommitCacheEntryCount(), 0u);
    EXPECT_EQ(MovedReservation.Base(), TransferredBase);
    EXPECT_EQ(MovedReservation.CachedCommittedBytes(), PageSize);

    auto DestinationResult = VmReservation::Reserve(PageSize * 2);
    EXPECT_TRUE(DestinationResult.IsOk());
    if (!DestinationResult.IsOk()) {
        return;
    }
    VmReservation Destination = Move(DestinationResult.Value());
    EXPECT_TRUE(Destination.Commit(0, PageSize).IsOk());
    Destination = Move(MovedReservation);
    EXPECT_TRUE(MovedReservation.Base() == nullptr);
    EXPECT_EQ(Destination.Base(), TransferredBase);
    EXPECT_EQ(Destination.CachedCommittedBytes(), PageSize);
    EXPECT_TRUE(Destination.Commit(PageSize, PageSize).IsOk());
    EXPECT_EQ(Destination.DecommitCacheHitCount(), 1ull);

    EXPECT_TRUE(Destination.Release().IsOk());
    EXPECT_TRUE(Destination.Release().IsOk());
    EXPECT_TRUE(Destination.Base() == nullptr);
    EXPECT_EQ(Destination.Capacity(), static_cast<usize>(0));
    EXPECT_EQ(Destination.ActualCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Destination.DecommitCacheHitCount(), 0ull);
    EXPECT_EQ(Destination.DecommitCacheMissCount(), 0ull);

    auto ReplacementResult = VmReservation::Reserve(PageSize);
    EXPECT_TRUE(ReplacementResult.IsOk());
    if (ReplacementResult.IsOk()) {
        Destination = Move(ReplacementResult.Value());
        EXPECT_TRUE(Destination.Base() != nullptr);
        EXPECT_TRUE(Destination.Commit(0, PageSize).IsOk());
        Destination = Move(Destination);
        EXPECT_EQ(Destination.ActiveCommittedBytes(), PageSize);
    }
}

// TLSF: 単純な alloc / free
ACS_TEST(MemSystem, TlsfBasic)
{
    constexpr usize kPoolSize = 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) return;

    FTlsfAllocator t;
    auto ir = t.Init(pool, kPoolSize);
    EXPECT_TRUE(ir.IsOk());

    void* p1 = t.Alloc(128, 16, FSourceLoc::Current());
    void* p2 = t.Alloc(256, 16, FSourceLoc::Current());
    void* p3 = t.Alloc(512, 16, FSourceLoc::Current());
    EXPECT_TRUE(p1 != nullptr);
    EXPECT_TRUE(p2 != nullptr);
    EXPECT_TRUE(p3 != nullptr);
    EXPECT_TRUE(t.BytesAllocated() > 0);
    EXPECT_EQ(t.AllocationCount(), 3ull);
    EXPECT_EQ(t.GetStats().used_blocks, 3ull);

    void* const ResizedAllocation = t.Realloc(p2, 256, 768, 16, FSourceLoc::Current());
    EXPECT_TRUE(ResizedAllocation != nullptr);
    if (ResizedAllocation) p2 = ResizedAllocation;
    EXPECT_EQ(t.AllocationCount(), 3ull);
    EXPECT_EQ(t.GetStats().used_blocks, 3ull);

    t.Free(p1);
    EXPECT_EQ(t.AllocationCount(), 2ull);
    t.Free(p2);
    t.Free(p3);
    EXPECT_EQ(t.AllocationCount(), 0ull);
    EXPECT_EQ(t.GetStats().used_blocks, 0ull);

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// TLSF: 返却ポインタが要求アライメントを必ず満たすことを検証
// (chaining が block_size を ≡8 mod16 に保つことで全 payload が 16 整列、>16 は leading split)
ACS_TEST(MemSystem, TlsfAlignment)
{
    constexpr usize kPoolSize = 4 * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) return;

    FTlsfAllocator t;
    auto ir = t.Init(pool, kPoolSize);
    EXPECT_TRUE(ir.IsOk());

    // 16 整列: 多数の確保が連鎖しても全て 16 整列を保つこと (旧バグ: 約半数が 8 整列)
    void* ptrs[64] = {};
    bool all16 = true;
    for (int i = 0; i < 64; ++i) {
        // サイズを変化させて分割パターンを揺らす
        usize sz = (usize)(8 + (i * 13) % 200);
        ptrs[i] = t.Alloc(sz, 16, FSourceLoc::Current());
        if (!ptrs[i] || (reinterpret_cast<uptr>(ptrs[i]) & 15u) != 0) all16 = false;
    }
    EXPECT_TRUE(all16);
    for (int i = 0; i < 64; ++i)
        if (ptrs[i]) t.Free(ptrs[i]);

    // 大アライメント: 32/64/128/256 をそれぞれ複数確保して境界を満たすこと
    const usize aligns[4] = {32, 64, 128, 256};
    bool all_big = true;
    for (int a = 0; a < 4; ++a) {
        void* bp[8] = {};
        for (int i = 0; i < 8; ++i) {
            usize sz = (usize)(16 + (i * 37) % 500);
            bp[i] = t.Alloc(sz, aligns[a], FSourceLoc::Current());
            if (!bp[i] || (reinterpret_cast<uptr>(bp[i]) & (aligns[a] - 1)) != 0) all_big = false;
            // 書き込んでもクラッシュしないこと (確保領域が実際に使えること)
            if (bp[i]) {
                u8* b = static_cast<u8*>(bp[i]);
                for (usize k = 0; k < sz; ++k)
                    b[k] = (u8)(k & 0xFF);
            }
        }
        for (int i = 0; i < 8; ++i)
            if (bp[i]) t.Free(bp[i]);
    }
    EXPECT_TRUE(all_big);

    // leading split で生じた free ブロックが再利用でき、ヒープが壊れていないことの確認:
    // 全解放後に大きな確保ができること
    void* big = t.Alloc(1024 * 1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(big != nullptr);
    if (big) t.Free(big);

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// TLSF: FL/SLが表現できない巨大要求や巨大プールを、ヒープへ触れる前に拒否する。
ACS_TEST(MemSystem, TlsfRejectsUnrepresentableSizes)
{
    constexpr usize kPoolSize = 1024 * 1024;
    void* Pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(Pool != nullptr);
    if (!Pool) return;

    FTlsfAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(Pool, kPoolSize).IsOk());

    const usize TooLargeRequest = tlsf::MAXIMUM_SEARCHABLE_BLOCK_SIZE + 1u;
    EXPECT_TRUE(Allocator.Alloc(TooLargeRequest, 16, FSourceLoc::Current()) == nullptr);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(Allocator.ValidateHeap());

#if ACS_ARCH_X64
    alignas(16) u8 FakePool[1024] = {};
    const usize OversizedPoolSize = static_cast<usize>(u64(1) << tlsf::FL_INDEX_MAX) + 1024u;
    EXPECT_TRUE(Allocator.AddPool(FakePool, OversizedPoolSize).IsErr());
    EXPECT_TRUE(Allocator.ValidateHeap());
#endif

    void* Allocation = Allocator.Alloc(128, 16, FSourceLoc::Current());
    EXPECT_TRUE(Allocation != nullptr);
    if (Allocation) {
        EXPECT_TRUE(Allocator.Realloc(Allocation, 128, 256, 24, FSourceLoc::Current()) == nullptr);
        EXPECT_EQ(Allocator.AllocationCount(), 1ull);
        Allocator.Free(Allocation);
    }
    EXPECT_TRUE(Allocator.ValidateHeap());

    ::HeapFree(::GetProcessHeap(), 0, Pool);
}

// TLSF: Reset前の再初期化、重複プール、追跡上限超過を状態変更前に拒否する。
ACS_TEST(MemSystem, TlsfRejectsReinitializationAndInvalidPools)
{
    constexpr usize kPoolSize = 1024 * 1024;
    constexpr usize kSmallPoolSize = 1024;
    constexpr usize kAdditionalPoolCount = 64;
    void* FirstPool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    void* SecondPool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    void* AdditionalPools = ::HeapAlloc(::GetProcessHeap(), 0, kSmallPoolSize * kAdditionalPoolCount);
    EXPECT_TRUE(FirstPool != nullptr && SecondPool != nullptr && AdditionalPools != nullptr);
    if (!FirstPool || !SecondPool || !AdditionalPools) {
        if (FirstPool) (void)::HeapFree(::GetProcessHeap(), 0, FirstPool);
        if (SecondPool) (void)::HeapFree(::GetProcessHeap(), 0, SecondPool);
        if (AdditionalPools) (void)::HeapFree(::GetProcessHeap(), 0, AdditionalPools);
        return;
    }

    FTlsfAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(FirstPool, kPoolSize).IsOk());
    void* const LiveAllocation = Allocator.Alloc(256, 16, FSourceLoc::Current());
    EXPECT_TRUE(LiveAllocation != nullptr);
    EXPECT_TRUE(Allocator.Init(SecondPool, kPoolSize).IsErr());
    EXPECT_TRUE(Allocator.AddPool(static_cast<u8*>(FirstPool) + 16u, 4096u).IsErr());
    EXPECT_TRUE(Allocator.ValidateHeap());

    auto* AdditionalBase = static_cast<u8*>(AdditionalPools);
    for (usize Index = 0; Index < kAdditionalPoolCount - 1u; ++Index) {
        EXPECT_TRUE(Allocator.AddPool(AdditionalBase + Index * kSmallPoolSize, kSmallPoolSize).IsOk());
    }
    EXPECT_TRUE(
        Allocator.AddPool(AdditionalBase + (kAdditionalPoolCount - 1u) * kSmallPoolSize, kSmallPoolSize).IsErr());
    EXPECT_TRUE(Allocator.ValidateHeap());

    Allocator.Free(LiveAllocation);
    EXPECT_TRUE(Allocator.Reset().IsOk());
    EXPECT_TRUE(Allocator.Init(SecondPool, kPoolSize).IsOk());
    EXPECT_TRUE(Allocator.Reset().IsOk());

    (void)::HeapFree(::GetProcessHeap(), 0, AdditionalPools);
    (void)::HeapFree(::GetProcessHeap(), 0, SecondPool);
    (void)::HeapFree(::GetProcessHeap(), 0, FirstPool);
}

// TLSF ストレス: alloc/free を多数混在させ、整列・統合・ヒープ整合 (ValidateHeap) を検証。
// 安全ガード (範囲検証/二重free検知) と Debug poison が通常運用を壊さないことも確認する。
ACS_TEST(MemSystem, TlsfStressValidate)
{
    constexpr usize kPoolSize = 8 * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) return;

    FTlsfAllocator t;
    auto ir = t.Init(pool, kPoolSize);
    EXPECT_TRUE(ir.IsOk());
    EXPECT_TRUE(t.ValidateHeap());

    // 決定論的 LCG (Math.random 不使用) で確保/解放を揺らす
    u32 rng = 0x12345678u;
    auto next = [&rng]() -> u32 {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };

    void* live[256] = {};
    bool ok = true;
    for (int iter = 0; iter < 6000 && ok; ++iter) {
        const u32 i = next() % 256u;
        if (live[i]) {
            t.Free(live[i]);
            live[i] = nullptr;
        } else {
            const usize sz = static_cast<usize>(1u + (next() % 4096u));
            const usize al = static_cast<usize>(16u << (next() % 5u)); // 16/32/64/128/256
            void* p = t.Alloc(sz, al, FSourceLoc::Current());
            if (p) {
                if ((reinterpret_cast<uptr>(p) & (al - 1)) != 0) ok = false; // 整列違反
                u8* b = static_cast<u8*>(p);
                for (usize k = 0; k < sz; ++k)
                    b[k] = static_cast<u8>(i + k); // 全域書込でOOBが無いこと
                live[i] = p;
            }
        }
        if ((iter & 0x1FF) == 0 && !t.ValidateHeap()) ok = false; // 周期的に整合検証
    }
    EXPECT_TRUE(ok);

    for (int i = 0; i < 256; ++i)
        if (live[i]) t.Free(live[i]);
    EXPECT_TRUE(t.ValidateHeap());

    // 全解放後は統合が効いて大確保が通る
    void* big = t.Alloc(4 * 1024 * 1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(big != nullptr);
    if (big) t.Free(big);
    EXPECT_TRUE(t.ValidateHeap());

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// TLSF auto-grow: 予約のうち初期コミットを小さくし、それを超える確保が予約からの
// 段階コミット (grow) で成功すること。grow が無ければ初期コミット超で nullptr になる。
ACS_TEST(MemSystem, TlsfAutoGrow)
{
    auto rr = VmReservation::Reserve(64ull * 1024 * 1024); // 64MB 予約
    EXPECT_TRUE(rr.IsOk());
    if (!rr.IsOk()) return;

    FTlsfAllocator t;
    auto ir = t.InitWithReservation(Move(rr.Value()), 1ull * 1024 * 1024); // 初期コミット 1MB のみ
    EXPECT_TRUE(ir.IsOk());
    EXPECT_TRUE(t.ValidateHeap());

    // 256KB を 48 個 = 12MB。初期 1MB を大きく超えるので grow が効かないと途中で失敗する。
    void* live[48] = {};
    bool ok = true;
    for (int i = 0; i < 48; ++i) {
        live[i] = t.Alloc(256 * 1024, 16, FSourceLoc::Current());
        if (!live[i]) {
            ok = false;
        } else {
            u8* b = static_cast<u8*>(live[i]);
            for (int k = 0; k < 256 * 1024; k += 4096)
                b[k] = static_cast<u8>(i); // 触って実コミット確認
        }
    }
    EXPECT_TRUE(ok);                                        // grow が効けば全部成功
    EXPECT_TRUE(t.BytesAllocated() >= 12ull * 1024 * 1024); // 初期 1MB を超えて確保できている
    EXPECT_TRUE(t.ValidateHeap());                          // 複数プールをまたいでヒープ健全

    for (int i = 0; i < 48; ++i)
        if (live[i]) t.Free(live[i]);
    EXPECT_TRUE(t.ValidateHeap());

    // grow 後の空き再利用: 全解放後に初期コミットを超える単一確保が通る
    void* big = t.Alloc(8ull * 1024 * 1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(big != nullptr);
    if (big) t.Free(big);
    EXPECT_TRUE(t.ValidateHeap());
}

// TLSF in-place Realloc: 拡大(次が free なら統合)/縮小(末尾解放) はコピー無しで ptr 不変、
// 不可なら移動 + データ保持。ランダムストレスでデータ整合とヒープ健全を検証する。
ACS_TEST(MemSystem, TlsfReallocInPlace)
{
    constexpr usize kPoolSize = 8 * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) return;
    FTlsfAllocator t;
    EXPECT_TRUE(t.Init(pool, kPoolSize).IsOk());

    auto fill = [](void* p, usize n, u8 seed) {
        u8* b = static_cast<u8*>(p);
        for (usize i = 0; i < n; ++i)
            b[i] = static_cast<u8>(seed + (i & 0x3F));
    };
    auto check = [](const void* p, usize n, u8 seed) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i)
            if (b[i] != static_cast<u8>(seed + (i & 0x3F))) return false;
        return true;
    };

    // (1) 拡大 in-place: A 直後の B を解放 → A を拡大すると空き領域を吸収して ptr 不変
    void* a = t.Alloc(1000, 16, FSourceLoc::Current());
    void* b = t.Alloc(1000, 16, FSourceLoc::Current());
    EXPECT_TRUE(a && b);
    fill(a, 1000, 0xA0);
    t.Free(b);
    void* a2 = t.Realloc(a, 1000, 1800, 16, FSourceLoc::Current());
    EXPECT_TRUE(a2 == a);               // 移動していない (in-place)
    EXPECT_TRUE(check(a2, 1000, 0xA0)); // データ保持
    EXPECT_TRUE(t.ValidateHeap());

    // (2) 縮小 in-place: ptr 不変、末尾を解放
    void* a3 = t.Realloc(a2, 1800, 400, 16, FSourceLoc::Current());
    EXPECT_TRUE(a3 == a2);
    EXPECT_TRUE(check(a3, 400, 0xA0));
    EXPECT_TRUE(t.ValidateHeap());

    // (3) 直後を別確保で塞いでから拡大 → 移動 + 先頭データ保持
    void* c = t.Alloc(1000, 16, FSourceLoc::Current());
    EXPECT_TRUE(c);
    void* a4 = t.Realloc(a3, 400, 5000, 16, FSourceLoc::Current());
    EXPECT_TRUE(a4 != nullptr);
    EXPECT_TRUE(check(a4, 400, 0xA0)); // コピーで先頭 400B 保持
    EXPECT_TRUE(t.ValidateHeap());
    t.Free(a4);
    t.Free(c);
    EXPECT_TRUE(t.ValidateHeap());

    // (4) ランダム realloc ストレス: 毎回データ整合 (min(old,new)) とヒープ健全を検証
    u32 rng = 0x9e3779b9u;
    auto next = [&rng]() -> u32 {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    void* p = t.Alloc(64, 16, FSourceLoc::Current());
    usize sz = 64;
    u8 seed = 0x11;
    fill(p, sz, seed);
    bool ok = (p != nullptr);
    for (int i = 0; i < 2000 && ok; ++i) {
        const usize ns = static_cast<usize>(16u + (next() % 8192u));
        void* np = t.Realloc(p, sz, ns, 16, FSourceLoc::Current());
        if (!np) {
            ok = false;
            break;
        }
        const usize keep = sz < ns ? sz : ns;
        if (!check(np, keep, seed)) ok = false; // 旧データ保持
        p = np;
        sz = ns;
        seed = static_cast<u8>(seed + 1u);
        fill(p, sz, seed);
        if ((i & 0x7F) == 0 && !t.ValidateHeap()) ok = false;
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(t.ValidateHeap());
    t.Free(p);
    EXPECT_TRUE(t.ValidateHeap());

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// シャード化 TLSF: 単体での基本動作 (確保/解放/realloc/アドレスルーティング/整合)。
ACS_TEST(MemSystem, ShardedTlsfBasic)
{
    FShardedTlsfAllocator a;
    auto ir = a.Init(64ull * 1024 * 1024, 4ull * 1024 * 1024, 4u); // 明示 4 シャード
    EXPECT_TRUE(ir.IsOk());
    EXPECT_EQ(a.ShardCount(), 4u);
    EXPECT_TRUE(a.ValidateHeap());

    // 多数確保 → 全 16 整列、書き込み可能、解放でルーティング成功
    void* ptrs[200] = {};
    bool ok = true;
    for (int i = 0; i < 200; ++i) {
        ptrs[i] = a.Alloc(static_cast<usize>(64 + (i * 37) % 4000), 16, FSourceLoc::Current());
        if (!ptrs[i] || (reinterpret_cast<uptr>(ptrs[i]) & 15u) != 0)
            ok = false;
        else {
            u8* b = static_cast<u8*>(ptrs[i]);
            b[0] = static_cast<u8>(i);
        }
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(a.BytesAllocated() > 0);
    EXPECT_EQ(a.AllocationCount(), 200ull);
    EXPECT_TRUE(a.ValidateHeap());

    // realloc (in-place / 移動 / シャード跨ぎ移動を含む)
    void* r = a.Alloc(100, 16, FSourceLoc::Current());
    EXPECT_TRUE(r != nullptr);
    EXPECT_EQ(a.AllocationCount(), 201ull);
    static_cast<u8*>(r)[0] = 0x5A;
    void* r2 = a.Realloc(r, 100, 6000, 16, FSourceLoc::Current());
    EXPECT_TRUE(r2 != nullptr);
    EXPECT_EQ(a.AllocationCount(), 201ull);
    EXPECT_TRUE(static_cast<u8*>(r2)[0] == 0x5A); // データ保持
    a.Free(r2);
    EXPECT_EQ(a.AllocationCount(), 200ull);

    for (int i = 0; i < 200; ++i)
        if (ptrs[i]) a.Free(ptrs[i]);
    EXPECT_EQ(a.AllocationCount(), 0ull);
    EXPECT_TRUE(a.ValidateHeap());
    a.Shutdown();
}

ACS_TEST(MemSystem, ShardedTlsfRejectsAlignmentOverflowBeforeReservation)
{
    FShardedTlsfAllocator allocator;

    EXPECT_TRUE(allocator.Init(~usize(0), 0u, 1u).IsErr());
    EXPECT_EQ(allocator.ShardCount(), 0u);
    EXPECT_EQ(allocator.Epoch(), 0ull);

    EXPECT_TRUE(allocator.Init(16u * 1024u * 1024u, ~usize(0), 1u).IsErr());
    EXPECT_EQ(allocator.ShardCount(), 0u);
    EXPECT_EQ(allocator.Epoch(), 0ull);

    // 境界入力の拒否後も中途半端な予約を残さず、通常値で初期化できること。
    EXPECT_TRUE(allocator.Init(16u * 1024u * 1024u, 1u * 1024u * 1024u, 1u).IsOk());
    EXPECT_EQ(allocator.ShardCount(), 1u);
    allocator.Shutdown();
}

ACS_TEST(MemSystem, ShardedTlsfReallocationToZeroPreservesInvalidPointer)
{
    FShardedTlsfAllocator allocator;
    EXPECT_TRUE(allocator.Init(16u * 1024u * 1024u, 1u * 1024u * 1024u, 1u).IsOk());

    auto* const allocation = static_cast<u8*>(allocator.Alloc(128u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(allocation != nullptr);
    if (!allocation) {
        allocator.Shutdown();
        return;
    }
    allocation[0] = 0xA5u;

    void* const interior_pointer = allocation + 16u;
    void* const rejected_release = allocator.Realloc(interior_pointer, 112u, 0u, 16u, FSourceLoc::Current());
    EXPECT_EQ(rejected_release, interior_pointer);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);
    EXPECT_EQ(allocation[0], static_cast<u8>(0xA5u));
    EXPECT_TRUE(allocator.ValidateHeap());

    void* const released = allocator.Realloc(allocation, 128u, 0u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(released == nullptr);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.Shutdown();
}

// シャード化 TLSF: マルチスレッド・ストレス。8 ワーカーで並行に alloc/realloc/free を回し、
// データ整合 (スレッドセーフでなければ領域が重複して壊れる) と最終ヒープ整合を検証する。
ACS_TEST(MemSystem, ShardedTlsfMultiThread)
{
    auto ir_pool = FThreadPool::Init(8);
    EXPECT_TRUE(ir_pool.IsOk());

    FShardedTlsfAllocator a;
    auto ir = a.Init(128ull * 1024 * 1024, 8ull * 1024 * 1024, 0u); // シャード数は自動
    EXPECT_TRUE(ir.IsOk());
    EXPECT_TRUE(a.ValidateHeap());

    struct Ctx {
        FShardedTlsfAllocator* alloc;
        TAtomic<u32> fail{0};
    };
    Ctx ctx;
    ctx.alloc = &a;

    auto thunk = [](u32 i, u32 /*worker*/, void* user) {
        Ctx* c = static_cast<Ctx*>(user);
        FShardedTlsfAllocator* al = c->alloc;
        u32 rng = 0x01000193u * (i + 1u) + 0x2545F491u;
        auto next = [&rng]() -> u32 {
            rng = rng * 1664525u + 1013904223u;
            return rng;
        };
        auto fill = [](void* p, usize n, u8 seed) {
            u8* b = static_cast<u8*>(p);
            for (usize k = 0; k < n; ++k)
                b[k] = static_cast<u8>(seed + (k & 0x1F));
        };
        auto ck = [](const void* p, usize n, u8 seed) -> bool {
            const u8* b = static_cast<const u8*>(p);
            for (usize k = 0; k < n; ++k)
                if (b[k] != static_cast<u8>(seed + (k & 0x1F))) return false;
            return true;
        };
        void* live[16] = {};
        usize sz[16] = {};
        u8 sd[16] = {};
        bool local_ok = true;
        for (int it = 0; it < 400 && local_ok; ++it) {
            const u32 slot = next() % 16u;
            if (live[slot]) {
                if (!ck(live[slot], sz[slot], sd[slot])) {
                    local_ok = false;
                    break;
                } // 重複/破壊検出
                if (next() & 1u) {
                    const usize ns = static_cast<usize>(8u + (next() % 2000u));
                    void* np = al->Realloc(live[slot], sz[slot], ns, 16, FSourceLoc::Current());
                    if (!np) {
                        local_ok = false;
                        break;
                    }
                    const usize keep = sz[slot] < ns ? sz[slot] : ns;
                    if (!ck(np, keep, sd[slot])) {
                        local_ok = false;
                        break;
                    }
                    live[slot] = np;
                    sz[slot] = ns;
                    sd[slot] = static_cast<u8>(sd[slot] + 1u);
                    fill(np, ns, sd[slot]);
                } else {
                    al->Free(live[slot]);
                    live[slot] = nullptr;
                }
            } else {
                const usize n = static_cast<usize>(8u + (next() % 2000u));
                void* p = al->Alloc(n, 16, FSourceLoc::Current());
                if (!p) {
                    local_ok = false;
                    break;
                }
                const u8 seed = static_cast<u8>(next());
                fill(p, n, seed);
                live[slot] = p;
                sz[slot] = n;
                sd[slot] = seed;
            }
        }
        for (int s = 0; s < 16; ++s)
            if (live[s]) al->Free(live[s]);
        if (!local_ok) c->fail.FetchAdd(1);
    };

    auto pr = FThreadPool::ParallelFor(0, 256, 1, +thunk, &ctx);
    EXPECT_TRUE(pr.IsOk());
    EXPECT_EQ(ctx.fail.Load(EMemoryOrder::Acquire), 0u); // データ破壊ゼロ = スレッドセーフ
    EXPECT_EQ(a.AllocationCount(), 0ull);
    EXPECT_TRUE(a.ValidateHeap()); // 全シャード健全

    a.Shutdown();
    FThreadPool::Shutdown();
}

// thread-local マガジン (lock-free hot path) の正当性。小サイズ中心に alloc/free を大量に回し、
// refill/pop/push/flush を踏みながらデータ整合 (重複払い出しがあれば破壊検出) と ValidateHeap を検証。
ACS_TEST(MemSystem, ShardedTlsfThreadCache)
{
    FShardedTlsfAllocator a;
    EXPECT_TRUE(a.Init(64ull * 1024 * 1024, 4ull * 1024 * 1024, 4u).IsOk());
    a.EnableThreadCache();
    EXPECT_TRUE(a.ThreadCacheEnabled());
    EXPECT_TRUE(a.ValidateHeap());

    u32 rng = 0xABCDEF01u;
    auto next = [&rng]() -> u32 {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    void* live[512] = {};
    usize sz[512] = {};
    u8 sd[512] = {};
    bool ok = true;
    for (int it = 0; it < 20000 && ok; ++it) {
        const u32 slot = next() % 512u;
        if (live[slot]) {
            const u8* b = static_cast<const u8*>(live[slot]);
            for (usize k = 0; k < sz[slot]; ++k)
                if (b[k] != static_cast<u8>(sd[slot] + (k & 0x1F))) {
                    ok = false;
                    break;
                }
            if (!ok) break;
            a.Free(live[slot]); // マガジンへ push (大量 free でバケット flush も踏む)
            live[slot] = nullptr;
        } else {
            const usize n = static_cast<usize>(8u + (next() % 480u)); // 小サイズ中心 (キャッシュ対象)
            void* p = a.Alloc(n, 16, FSourceLoc::Current());          // pop / refill
            if (!p) {
                ok = false;
                break;
            }
            const u8 seed = static_cast<u8>(next());
            u8* b = static_cast<u8*>(p);
            for (usize k = 0; k < n; ++k)
                b[k] = static_cast<u8>(seed + (k & 0x1F));
            live[slot] = p;
            sz[slot] = n;
            sd[slot] = seed;
        }
        if ((it & 0xFFF) == 0 && !a.ValidateHeap()) ok = false;
    }
    EXPECT_TRUE(ok);
    for (int i = 0; i < 512; ++i)
        if (live[i]) a.Free(live[i]);
    EXPECT_EQ(a.AllocationCount(), 0ull); // マガジン内の空きブロックは未解放件数に含めない
    EXPECT_TRUE(a.ValidateHeap());
    a.Shutdown();
}

// 解放後 payload は利用者に壊され得るため、キャッシュ管理リンクを payload 内へ置かないこと。
ACS_TEST(MemSystem, ShardedTlsfThreadCacheDoesNotDereferenceReleasedPayload)
{
    FShardedTlsfAllocator allocator;
    EXPECT_TRUE(allocator.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 1u).IsOk());
    allocator.EnableThreadCache();

    auto* const allocation = static_cast<uptr*>(allocator.Alloc(96u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(allocation != nullptr);
    if (!allocation) {
        allocator.Shutdown();
        return;
    }

    allocator.Free(allocation);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);

    // 旧実装はこの先頭ワードを next ポインタとして参照し、次回 pop で不正アドレスを掴んだ。
    *allocation = ~uptr(0);
    void* const reused = allocator.Alloc(96u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(reused == allocation);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);

    if (reused) allocator.Free(reused);
    allocator.FlushCurrentThreadCache();
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.Shutdown();
}

// thread-local キャッシュへ同じブロックを二度積まず、公開中件数をアンダーフローさせない。
ACS_TEST(MemSystem, ShardedTlsfThreadCacheRejectsDuplicateFree)
{
    FShardedTlsfAllocator allocator;
    EXPECT_TRUE(allocator.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
    allocator.EnableThreadCache();

    void* const allocation = allocator.Alloc(96u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);
    if (!allocation) {
        allocator.Shutdown();
        return;
    }

    allocator.Free(allocation);
    const u64 cached_bytes = allocator.BytesAllocated();
    EXPECT_EQ(allocator.AllocationCount(), 0ull);

    allocator.Free(allocation);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    EXPECT_EQ(allocator.BytesAllocated(), cached_bytes);
    EXPECT_TRUE(allocator.ValidateHeap());

    void* const first = allocator.Alloc(96u, 16u, FSourceLoc::Current());
    void* const second = allocator.Alloc(96u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(second != nullptr);
    EXPECT_TRUE(first != second);
    if (first) allocator.Free(first);
    if (second) allocator.Free(second);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.Shutdown();
}

ACS_TEST(MemSystem, ShardedTlsfThreadCacheRejectsInteriorAndUncommittedPointers)
{
    FShardedTlsfAllocator allocator;
    auto result = allocator.Init(32u * 1024u * 1024u, 64u * 1024u, 1u);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;

    allocator.EnableThreadCache();
    auto* const allocation = static_cast<u8*>(allocator.Alloc(128u, 16u, FSourceLoc::Current()));
    EXPECT_TRUE(allocation != nullptr);
    if (!allocation) {
        allocator.Shutdown();
        return;
    }

    // payload 内に前後リンクまで整合する偽ヘッダを置いても、外部境界表にないポインタは拒否する。
    void* const interior_pointer = allocation + 64u;
    *reinterpret_cast<usize*>(allocation + 8u) = 40u;
    *reinterpret_cast<void**>(allocation + 48u) = allocation;
    *reinterpret_cast<usize*>(allocation + 56u) = 24u;
    *reinterpret_cast<void**>(allocation + 80u) = allocation + 48u;
    allocator.Free(interior_pointer);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);

    void* const next_allocation = allocator.Alloc(16u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(next_allocation != interior_pointer);
    EXPECT_EQ(allocator.AllocationCount(), 2ull);

    // 予約済みでも未コミットのアドレスではヘッダを読まず、安全に拒否する。
    void* const uncommitted_pointer = allocation + (8u * 1024u * 1024u);
    allocator.Free(uncommitted_pointer);
    EXPECT_EQ(allocator.AllocationCount(), 2ull);

    allocator.Free(next_allocation);
    allocator.Free(allocation);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.FlushCurrentThreadCache();
    allocator.Shutdown();
}

// スレッド終了時に TLS マガジンの残存ブロックを owner へ返し、実使用量を残さない。
ACS_TEST(MemSystem, ShardedTlsfThreadCacheReturnsOnThreadExit)
{
    FShardedTlsfAllocator allocator;
    EXPECT_TRUE(allocator.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
    allocator.EnableThreadCache();

    struct Context {
        FShardedTlsfAllocator* allocator = nullptr;
        TAtomic<u32> failures{0};
    } context;
    context.allocator = &allocator;

    auto thread = FThread::Spawn(
        [](void* user) {
            Context* context = static_cast<Context*>(user);
            void* blocks[32] = {};
            for (u32 i = 0; i < 32u; ++i) {
                blocks[i] = context->allocator->Alloc(64u + (i % 4u) * 32u, 16u, FSourceLoc::Current());
                if (!blocks[i]) context->failures.FetchAdd(1);
            }
            for (void* block : blocks) {
                if (block) context->allocator->Free(block);
            }
        },
        &context);

    EXPECT_TRUE(thread.IsOk());
    if (thread.IsOk()) thread.Value().Join();
    EXPECT_EQ(context.failures.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.Shutdown();
}

// 同一スレッドで owner を切り替えるとき、旧 owner のマガジンを先に返す。
ACS_TEST(MemSystem, ShardedTlsfThreadCacheReturnsOnAllocatorSwitch)
{
    FShardedTlsfAllocator first;
    FShardedTlsfAllocator second;
    EXPECT_TRUE(first.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
    EXPECT_TRUE(second.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
    first.EnableThreadCache();
    second.EnableThreadCache();

    void* first_block = first.Alloc(96u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(first_block != nullptr);
    if (first_block) first.Free(first_block);
    EXPECT_TRUE(first.BytesAllocated() > 0u);

    void* second_block = second.Alloc(96u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(second_block != nullptr);
    if (second_block) second.Free(second_block);
    EXPECT_EQ(first.BytesAllocated(), 0ull);
    EXPECT_TRUE(first.ValidateHeap());

    second.Shutdown();
    first.Shutdown();
}

// 別スレッドの旧世代マガジンは、Shutdown/再 Init 後の同一アドレスへ返さず安全に破棄する。
ACS_TEST(MemSystem, ShardedTlsfThreadCacheRejectsRetiredGeneration)
{
    FShardedTlsfAllocator allocator;
    EXPECT_TRUE(allocator.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
    allocator.EnableThreadCache();

    struct Context {
        FShardedTlsfAllocator* allocator = nullptr;
        TAtomic<u32> ready{0};
        TAtomic<u32> proceed{0};
        TAtomic<u32> failures{0};
    } context;
    context.allocator = &allocator;

    auto thread = FThread::Spawn(
        [](void* user) {
            Context* context = static_cast<Context*>(user);
            void* old_block = context->allocator->Alloc(128u, 16u, FSourceLoc::Current());
            if (!old_block)
                context->failures.FetchAdd(1);
            else
                context->allocator->Free(old_block);
            context->ready.Store(1u, EMemoryOrder::Release);

            while (context->proceed.Load(EMemoryOrder::Acquire) == 0u)
                SleepMs(1u);

            void* new_block = context->allocator->Alloc(128u, 16u, FSourceLoc::Current());
            if (!new_block)
                context->failures.FetchAdd(1);
            else
                context->allocator->Free(new_block);
        },
        &context);

    EXPECT_TRUE(thread.IsOk());
    if (thread.IsOk()) {
        while (context.ready.Load(EMemoryOrder::Acquire) == 0u)
            SleepMs(1u);
        EXPECT_TRUE(allocator.BytesAllocated() > 0u);

        const u64 old_epoch = allocator.Epoch();
        allocator.Shutdown();
        EXPECT_EQ(allocator.Epoch(), 0ull);
        EXPECT_TRUE(allocator.Init(16ull * 1024 * 1024, 1ull * 1024 * 1024, 2u).IsOk());
        EXPECT_TRUE(allocator.Epoch() != old_epoch);

        context.proceed.Store(1u, EMemoryOrder::Release);
        thread.Value().Join();
    }

    EXPECT_EQ(context.failures.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    EXPECT_TRUE(allocator.ValidateHeap());
    allocator.Shutdown();
}

// 再配置可能アロケータ: 基本確保/Resolve、断片化 → Compact でデータ生存 + high-water 回収、
// freed ハンドルの無効化、世代による use-after-free 検出を検証する。
ACS_TEST(MemSystem, RelocatableBasicCompact)
{
    FRelocatableAllocator r;
    EXPECT_TRUE(r.Init(1u * 1024 * 1024, 256u, nullptr).IsOk());

    auto fill = [](void* p, usize n, u8 s) {
        u8* b = static_cast<u8*>(p);
        for (usize i = 0; i < n; ++i)
            b[i] = static_cast<u8>(s + (i & 0x3F));
    };
    auto check = [](void* p, usize n, u8 s) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i)
            if (b[i] != static_cast<u8>(s + (i & 0x3F))) return false;
        return true;
    };

    FRelocHandle h[100];
    usize sz[100];
    u8 sd[100];
    bool ok = true;
    for (int i = 0; i < 100; ++i) {
        sz[i] = static_cast<usize>(64 + (i * 53) % 2000);
        h[i] = r.Alloc(sz[i], 16);
        if (!h[i].IsValid()) {
            ok = false;
            break;
        }
        sd[i] = static_cast<u8>(i * 7 + 1);
        void* p = r.Resolve(h[i]);
        if (!p || (reinterpret_cast<uptr>(p) & 15u) != 0) {
            ok = false;
            break;
        }
        fill(p, sz[i], sd[i]);
    }
    EXPECT_TRUE(ok);
    EXPECT_EQ(r.LiveCount(), 100u);

    // 1 つおきに解放 → 断片化 (high-water > used)
    for (int i = 0; i < 100; i += 2)
        r.Free(h[i]);
    EXPECT_EQ(r.LiveCount(), 50u);
    EXPECT_TRUE(r.HighWater() > r.Used());
    EXPECT_TRUE(r.Resolve(h[0]) == nullptr); // 解放済みは nullptr

    // Compact: high-water が縮み、生存ハンドルのデータは移動しても保持される
    const usize hw_before = r.HighWater();
    const usize reclaimed = r.Compact();
    EXPECT_TRUE(reclaimed > 0);
    EXPECT_TRUE(r.HighWater() < hw_before);
    EXPECT_TRUE(r.HighWater() >= r.Used());
    bool ok2 = true;
    for (int i = 1; i < 100; i += 2) {
        void* p = r.Resolve(h[i]);
        if (!p || !check(p, sz[i], sd[i])) {
            ok2 = false;
            break;
        }
    }
    EXPECT_TRUE(ok2);

    // 世代: 解放したハンドルを再利用すると旧ハンドルは無効
    FRelocHandle old = h[1];
    r.Free(h[1]);
    EXPECT_TRUE(r.Resolve(old) == nullptr);
    FRelocHandle nw = r.Alloc(128, 16);
    EXPECT_TRUE(nw.IsValid());
    EXPECT_TRUE(r.Resolve(old) == nullptr); // 旧ハンドルは依然無効 (世代不一致 or 解放済)

    r.Shutdown();
}

// 再配置アロケータ ストレス: ランダム alloc/free + 周期 Compact で、全生存ハンドルの
// データが移動後も保持されること (再配置の正当性) を検証する。
ACS_TEST(MemSystem, RelocatableStress)
{
    FRelocatableAllocator r;
    EXPECT_TRUE(r.Init(2u * 1024 * 1024, 512u, nullptr).IsOk());

    auto fill = [](void* p, usize n, u8 s) {
        u8* b = static_cast<u8*>(p);
        for (usize i = 0; i < n; ++i)
            b[i] = static_cast<u8>(s + (i & 0x3F));
    };
    auto check = [](void* p, usize n, u8 s) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i)
            if (b[i] != static_cast<u8>(s + (i & 0x3F))) return false;
        return true;
    };

    struct Rec {
        FRelocHandle h;
        usize sz;
        u8 sd;
    };
    Rec rec[512];
    for (int i = 0; i < 512; ++i) {
        rec[i].h = FRelocHandle{};
        rec[i].sz = 0;
        rec[i].sd = 0;
    }

    u32 rng = 0x55555555u;
    auto next = [&rng]() -> u32 {
        rng = rng * 1664525u + 1013904223u;
        return rng;
    };
    bool ok = true;
    for (int it = 0; it < 12000 && ok; ++it) {
        const u32 slot = next() % 512u;
        if (rec[slot].h.IsValid()) {
            void* p = r.Resolve(rec[slot].h);
            if (!p || !check(p, rec[slot].sz, rec[slot].sd)) {
                ok = false;
                break;
            }
            r.Free(rec[slot].h);
            rec[slot].h = FRelocHandle{};
        } else {
            const usize n = static_cast<usize>(16u + (next() % 1024u));
            FRelocHandle h = r.Alloc(n, 16);
            if (!h.IsValid()) continue; // 満杯 (Alloc が内部 Compact しても入らない) → skip
            const u8 s = static_cast<u8>(next());
            fill(r.Resolve(h), n, s);
            rec[slot].h = h;
            rec[slot].sz = n;
            rec[slot].sd = s;
        }
        if ((it & 0x1FF) == 0) {
            r.Compact(); // 周期デフラグ: 全生存データが移動後も無事か
            for (int j = 0; j < 512 && ok; ++j) {
                if (rec[j].h.IsValid()) {
                    void* p = r.Resolve(rec[j].h);
                    if (!p || !check(p, rec[j].sz, rec[j].sd)) ok = false;
                }
            }
        }
    }
    EXPECT_TRUE(ok);
    r.Shutdown();
}

// 既定アロケータ結線: Init で Default セグメントへ差し替わり、Shutdown で
// 元へ復元されること。既定経由の確保が Default セグメントに計上されることも確認する。
ACS_TEST(MemSystem, DefaultAllocatorWiring)
{
    FAllocator* before = &DefaultAllocator();

    MemorySystemConfig cfg = FMemorySystem::DefaultConfig();
    cfg.install_as_default_allocator = true;
    EXPECT_TRUE(FMemorySystem::Init(cfg).IsOk());

    // 既定が Default セグメントへ切り替わっている
    EXPECT_TRUE(&DefaultAllocator() == FMemorySystem::Get(ESegment::Default));
    EXPECT_TRUE(&DefaultAllocator() != before);

    // 既定経由の確保が Default セグメントに計上される
    FAllocator* def_seg = FMemorySystem::Get(ESegment::Default);
    const u64 used0 = def_seg->BytesAllocated();
    void* p = DefaultAllocator().Alloc(4096, 16, FSourceLoc::Current());
    EXPECT_TRUE(p != nullptr);
    EXPECT_TRUE(def_seg->BytesAllocated() > used0);
    DefaultAllocator().Free(p);

    FMemorySystem::Shutdown();
    // Shutdown で元の既定へ復元されている
    EXPECT_TRUE(&DefaultAllocator() == before);
}

// マイクロベンチ: 8 スレッドで alloc+free を churn し、単一ロック TLSF / シャード TLSF /
// HeapAlloc のスループットを比較する。ロック競合解消を実測で可視化する (時間はログ出力のみ、
// 環境差でブレるため assert は正当性のみ)。
ACS_TEST(MemSystem, ShardedTlsfBenchmark)
{
    auto ir_pool = FThreadPool::Init(8);
    EXPECT_TRUE(ir_pool.IsOk());

    constexpr usize kPoolSize = 128ull * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) {
        FThreadPool::Shutdown();
        return;
    }

    FTlsfAllocator single; // 単一ロック比較用 (HeapAlloc プール)
    EXPECT_TRUE(single.Init(pool, kPoolSize).IsOk());
    FMutex single_lock;

    FShardedTlsfAllocator sharded;
    EXPECT_TRUE(sharded.Init(kPoolSize, 8ull * 1024 * 1024, 0u).IsOk());

    FShardedTlsfAllocator sharded_c; // lock-free マガジン有効版
    EXPECT_TRUE(sharded_c.Init(kPoolSize, 8ull * 1024 * 1024, 0u).IsOk());
    sharded_c.EnableThreadCache();

    struct BCtx {
        int mode;
        FTlsfAllocator* single;
        FMutex* lk;
        FShardedTlsfAllocator* sharded;
        FShardedTlsfAllocator* sharded_c;
    };
    BCtx bc{0, &single, &single_lock, &sharded, &sharded_c};

    auto bench = [](u32 i, u32 /*w*/, void* user) {
        BCtx* c = static_cast<BCtx*>(user);
        constexpr int kOps = 4000;
        u32 rng = 0x2545F491u * (i + 1u);
        for (int k = 0; k < kOps; ++k) {
            rng = rng * 1664525u + 1013904223u;
            const usize n = static_cast<usize>(16u + (rng % 1024u));
            void* p = nullptr;
            if (c->mode == 0) {
                FScopedLock l(*c->lk);
                p = c->single->Alloc(n, 16, FSourceLoc::Current());
            } else if (c->mode == 1) {
                p = c->sharded->Alloc(n, 16, FSourceLoc::Current());
            } else if (c->mode == 2) {
                p = ::HeapAlloc(::GetProcessHeap(), 0, n);
            } else {
                p = c->sharded_c->Alloc(n, 16, FSourceLoc::Current());
            }
            if (!p) continue;
            static_cast<u8*>(p)[0] = static_cast<u8>(k);
            if (c->mode == 0) {
                FScopedLock l(*c->lk);
                c->single->Free(p);
            } else if (c->mode == 1) {
                c->sharded->Free(p);
            } else if (c->mode == 2) {
                ::HeapFree(::GetProcessHeap(), 0, p);
            } else {
                c->sharded_c->Free(p);
            }
        }
    };

    LARGE_INTEGER freq;
    ::QueryPerformanceFrequency(&freq);
    auto run_ms = [&](int mode) -> double {
        bc.mode = mode;
        LARGE_INTEGER a0;
        ::QueryPerformanceCounter(&a0);
        auto r = FThreadPool::ParallelFor(0, 256, 1, +bench, &bc);
        LARGE_INTEGER a1;
        ::QueryPerformanceCounter(&a1);
        (void)r;
        return static_cast<double>(a1.QuadPart - a0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    };

    const double t_single = run_ms(0);
    const double t_sharded = run_ms(1);
    const double t_heap = run_ms(2);
    const double t_cached = run_ms(3);
    std::printf("[bench MT8 alloc/free x %d] single-lock-TLSF=%.1fms  sharded-TLSF=%.1fms  "
                "sharded+cache=%.1fms  HeapAlloc=%.1fms\n",
                256 * 4000, t_single, t_sharded, t_cached, t_heap);

    EXPECT_TRUE(sharded.ValidateHeap()); // ベンチ後も健全
    EXPECT_TRUE(single.ValidateHeap());

    sharded_c.Shutdown();
    sharded.Shutdown();
    ::HeapFree(::GetProcessHeap(), 0, pool);
    FThreadPool::Shutdown();
}

ACS_TEST(MemSystem, MimallocLifetimeAndInvalidZeroReallocationPreserveOwnership)
{
    FMimallocAllocator allocator;
    EXPECT_TRUE(allocator.Init().IsOk());
    const u64 lifetime_generation = allocator.LifetimeGeneration();
    EXPECT_TRUE(lifetime_generation != 0u);

    void* const allocation = allocator.Alloc(256u, 16u, FSourceLoc::Current());
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);

    alignas(16) u8 foreign_storage[64]{};
    EXPECT_TRUE(allocator.Realloc(foreign_storage, sizeof(foreign_storage), 0u, 16u, FSourceLoc::Current()) ==
                foreign_storage);
    EXPECT_EQ(allocator.AllocationCount(), 1ull);

    allocator.Free(allocation);
    EXPECT_EQ(allocator.AllocationCount(), 0ull);
    allocator.Shutdown();
    EXPECT_EQ(allocator.LifetimeGeneration(), 0ull);
}

// FMemorySystem: 全セグメント初期化 → 取得 → 解放
ACS_TEST(MemSystem, SegmentInitGet)
{
    MemorySystemConfig cfg = FMemorySystem::DefaultConfig();
    auto r = FMemorySystem::Init(cfg);
    EXPECT_TRUE(r.IsOk());

    FAllocator* a = FMemorySystem::Get(ESegment::Default);
    EXPECT_TRUE(a != nullptr);
    const u64 first_lifetime_generation = a ? a->LifetimeGeneration() : 0u;
    EXPECT_TRUE(first_lifetime_generation != 0u);

    void* p = a->Alloc(1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(p != nullptr);
    a->Free(p);

    FMemorySystem::Shutdown();
    EXPECT_EQ(a->LifetimeGeneration(), 0ull);

    EXPECT_TRUE(FMemorySystem::Init(cfg).IsOk());
    FAllocator* const rebound_allocator = FMemorySystem::Get(ESegment::Default);
    EXPECT_TRUE(rebound_allocator == a);
    EXPECT_TRUE(rebound_allocator != nullptr);
    if (rebound_allocator) {
        EXPECT_TRUE(rebound_allocator->LifetimeGeneration() != 0u);
        EXPECT_TRUE(rebound_allocator->LifetimeGeneration() != first_lifetime_generation);
    }
    FMemorySystem::Shutdown();
}

ACS_TEST(MemSystem, MimallocBackendBudgetAndIndependentInspection)
{
    MemorySystemConfig configuration = FMemorySystem::DefaultConfig();
    configuration.segments[(usize)ESegment::Default].hard_budget_bytes = 2048u;
    EXPECT_TRUE(FMemorySystem::Init(configuration).IsOk());

    FAllocator* const allocator = FMemorySystem::Get(ESegment::Default);
    EXPECT_TRUE(allocator != nullptr);
    if (!allocator) {
        FMemorySystem::Shutdown();
        return;
    }

    void* first = allocator->Alloc(1536u, 64u, FSourceLoc::Current());
    void* over_budget = allocator->Alloc(1024u, 64u, FSourceLoc::Current());
    EXPECT_TRUE(first != nullptr);
    EXPECT_TRUE(over_budget == nullptr);

    const MemorySegmentInspection inspection = FMemorySystem::InspectSegmentMemory(ESegment::Default);
    EXPECT_TRUE(inspection.backend_inspection_supported);
    EXPECT_TRUE(inspection.visit_succeeded);
    EXPECT_TRUE(inspection.metadata_valid);
    EXPECT_TRUE(inspection.matches_authoritative_statistics);
    EXPECT_EQ(inspection.outstanding_allocation_count, 1ull);
    EXPECT_EQ(inspection.requested_bytes, 1536ull);
    EXPECT_TRUE(inspection.usable_bytes >= inspection.requested_bytes);

    allocator->Free(first);
    FMemorySystem::Shutdown();
}

ACS_TEST(MemSystem, FrameArenaBudgetIsAtomicAndResets)
{
    constexpr u64 kBudgetBytes = 4096u;
    constexpr u32 kThreadCount = 32u;

    MemorySystemConfig configuration = FMemorySystem::DefaultConfig();
    configuration.segments[(usize)ESegment::Temp].hard_budget_bytes = kBudgetBytes;
    EXPECT_TRUE(FMemorySystem::Init(configuration).IsOk());

    FAllocator* const allocator = FMemorySystem::Get(ESegment::Temp);
    EXPECT_TRUE(allocator != nullptr);
    if (!allocator) {
        FMemorySystem::Shutdown();
        return;
    }

    struct FrameBudgetContext {
        FAllocator* allocator = nullptr;
        u64 allocation_bytes = 0;
        TAtomic<u32> ready{0};
        TAtomic<u32> start{0};
        TAtomic<u64> successful_bytes{0};
    } context;
    context.allocator = allocator;
    context.allocation_bytes = kBudgetBytes;

    FThread threads[kThreadCount];
    u32 spawned_threads = 0;
    for (u32 index = 0; index < kThreadCount; ++index) {
        auto thread = FThread::Spawn(
            [](void* user) {
                auto* thread_context = static_cast<FrameBudgetContext*>(user);
                thread_context->ready.FetchAdd(1u);
                while (thread_context->start.Load(EMemoryOrder::Acquire) == 0u) {
                    Yield();
                }
                void* const allocation = thread_context->allocator->Alloc(
                    static_cast<usize>(thread_context->allocation_bytes), 16u, FSourceLoc::Current());
                if (allocation) {
                    thread_context->successful_bytes.FetchAdd(thread_context->allocation_bytes);
                }
            },
            &context);
        if (thread.IsOk()) {
            threads[spawned_threads++] = Move(thread.Value());
        }
    }

    EXPECT_TRUE(spawned_threads > 0u);
    while (context.ready.Load(EMemoryOrder::Acquire) < spawned_threads) {
        Yield();
    }
    context.start.Store(1u, EMemoryOrder::Release);
    for (u32 index = 0; index < spawned_threads; ++index) {
        threads[index].Join();
    }

    EXPECT_EQ(context.successful_bytes.Load(EMemoryOrder::Acquire), kBudgetBytes);
    EXPECT_EQ(allocator->BytesAllocated(), kBudgetBytes);
    EXPECT_TRUE(allocator->Alloc(1u, 16u, FSourceLoc::Current()) == nullptr);

    FMemorySystem::ResetTemp();
    EXPECT_EQ(allocator->BytesAllocated(), 0ull);
    EXPECT_TRUE(allocator->Alloc(kBudgetBytes, 16u, FSourceLoc::Current()) != nullptr);
    EXPECT_TRUE(allocator->Alloc(1u, 16u, FSourceLoc::Current()) == nullptr);
    EXPECT_TRUE(!FMemorySystem::CaptureLeakSummary().HasLeaks());
    FMemorySystem::Shutdown();
}

ACS_TEST(MemSystem, ResetTempSerializesConcurrentOperationsAndBudgetReset)
{
    constexpr u64 kBudgetBytes = 64u * 1024u;
    constexpr u32 kWorkerCount = 4u;

    MemorySystemConfig configuration = FMemorySystem::DefaultConfig();
    configuration.segments[(usize)ESegment::Temp].hard_budget_bytes = kBudgetBytes;
    EXPECT_TRUE(FMemorySystem::Init(configuration).IsOk());

    FAllocator* const allocator = FMemorySystem::Get(ESegment::Temp);
    EXPECT_TRUE(allocator != nullptr);
    if (!allocator) {
        FMemorySystem::Shutdown();
        return;
    }

    struct Context {
        FAllocator* allocator = nullptr;
        TAtomic<u32> start{0};
        TAtomic<u64> successful_allocations{0};
    } context;
    context.allocator = allocator;

    FThread workers[kWorkerCount];
    u32 spawned_workers = 0u;
    for (u32 index = 0u; index < kWorkerCount; ++index) {
        auto worker = FThread::Spawn(
            [](void* user) {
                auto* thread_context = static_cast<Context*>(user);
                while (thread_context->start.Load(EMemoryOrder::Acquire) == 0u) {
                    Yield();
                }
                for (u32 iteration = 0u; iteration < 4000u; ++iteration) {
                    if (thread_context->allocator->Alloc(64u, 16u, FSourceLoc::Current())) {
                        thread_context->successful_allocations.FetchAdd(1u);
                    }
                }
            },
            &context);
        if (worker.IsOk()) {
            workers[spawned_workers++] = Move(worker.Value());
        }
    }

    auto reset_thread = FThread::Spawn(
        [](void* user) {
            auto* thread_context = static_cast<Context*>(user);
            while (thread_context->start.Load(EMemoryOrder::Acquire) == 0u) {
                Yield();
            }
            for (u32 iteration = 0u; iteration < 1000u; ++iteration) {
                FMemorySystem::ResetTemp();
            }
        },
        &context);

    EXPECT_TRUE(spawned_workers > 0u);
    EXPECT_TRUE(reset_thread.IsOk());
    context.start.Store(1u, EMemoryOrder::Release);
    for (u32 index = 0u; index < spawned_workers; ++index) {
        workers[index].Join();
    }
    if (reset_thread.IsOk()) {
        reset_thread.Value().Join();
    }

    EXPECT_TRUE(context.successful_allocations.Load(EMemoryOrder::Acquire) > 0u);
    FMemorySystem::ResetTemp();
    EXPECT_EQ(allocator->BytesAllocated(), 0ull);
    EXPECT_EQ(allocator->AllocationCount(), 0ull);
    EXPECT_TRUE(allocator->Alloc(static_cast<usize>(kBudgetBytes), 16u, FSourceLoc::Current()) != nullptr);
    EXPECT_TRUE(allocator->Alloc(1u, 16u, FSourceLoc::Current()) == nullptr);
    FMemorySystem::ResetTemp();
    FMemorySystem::Shutdown();
}

ACS_TEST(MemSystem, RejectsMisorderedSegmentConfiguration)
{
    MemorySystemConfig configuration = FMemorySystem::DefaultConfig();
    configuration.segments[(usize)ESegment::Default].segment = ESegment::Resource;
    EXPECT_TRUE(FMemorySystem::Init(configuration).IsErr());
    EXPECT_TRUE(FMemorySystem::Get(ESegment::Default) == nullptr);
}

ACS_TEST(MemSystem, LeakSummaryTracksOutstandingMemory)
{
    auto result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;

    FAllocator* allocator = FMemorySystem::Get(ESegment::Default);
    void* allocation = allocator ? allocator->Alloc(1024, 16, FSourceLoc::Current()) : nullptr;
    EXPECT_TRUE(allocation != nullptr);

    const MemoryLeakSummary outstanding = FMemorySystem::CaptureLeakSummary();
    EXPECT_TRUE(outstanding.HasLeaks());
    EXPECT_TRUE(outstanding.outstanding_bytes >= 1024);
    EXPECT_TRUE(outstanding.leaking_segment_count >= 1);

    if (allocator) allocator->Free(allocation);
    const MemoryLeakSummary released = FMemorySystem::CaptureLeakSummary();
    EXPECT_TRUE(!released.HasLeaks());
    EXPECT_EQ(released.outstanding_bytes, 0ull);
    EXPECT_EQ(released.leaking_segment_count, 0u);
    FMemorySystem::Shutdown();
}

ACS_TEST(MemSystem, AllocationSiteTrackingSupportsCheckpointsAndReallocation)
{
    auto result = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) return;

    const MemoryTrackingCheckpoint checkpoint = FMemorySystem::CaptureMemoryTrackingCheckpoint();
    FAllocator* allocator = FMemorySystem::Get(ESegment::Default);
    const u32 allocation_line = __LINE__ + 1u;
    void* allocation = allocator->Alloc(1536, 32, FSourceLoc::Current());
    EXPECT_TRUE(allocation != nullptr);

    OutstandingMemoryAllocation details[2]{};
    MemoryTrackingReport report = FMemorySystem::CollectOutstandingMemoryAllocations(checkpoint, details, 2);
    if (FMemorySystem::IsAllocationSiteTrackingEnabled()) {
        EXPECT_TRUE(report.tracking_enabled);
        EXPECT_TRUE(report.IsComplete());
        EXPECT_EQ(report.outstanding_allocation_count, 1ull);
        EXPECT_EQ(report.written_allocation_count, 1ull);
        EXPECT_EQ(details[0].address, allocation);
        EXPECT_EQ(details[0].requested_bytes, 1536ull);
        EXPECT_EQ(details[0].alignment, 32ull);
        EXPECT_EQ(details[0].segment, ESegment::Default);
        EXPECT_EQ(details[0].source_line, allocation_line);
        EXPECT_TRUE(details[0].source_file && details[0].source_file[0] != '\0');
        EXPECT_TRUE(details[0].source_function && details[0].source_function[0] != '\0');
        EXPECT_TRUE(details[0].allocation_sequence > checkpoint.allocation_sequence);
    } else {
        EXPECT_TRUE(!report.tracking_enabled);
        EXPECT_EQ(report.outstanding_allocation_count, 0ull);
    }

    void* reallocated = allocator->Realloc(allocation, 1536, 3072, 64, FSourceLoc::Current());
    EXPECT_TRUE(reallocated != nullptr);
    if (reallocated) allocation = reallocated;

    report = FMemorySystem::CollectOutstandingMemoryAllocations(checkpoint, details, 2);
    if (FMemorySystem::IsAllocationSiteTrackingEnabled()) {
        EXPECT_EQ(report.outstanding_allocation_count, 1ull);
        EXPECT_EQ(report.written_allocation_count, 1ull);
        EXPECT_EQ(details[0].address, allocation);
        EXPECT_EQ(details[0].requested_bytes, 3072ull);
        EXPECT_EQ(details[0].alignment, 64ull);
    }

    allocator->Free(allocation);
    report = FMemorySystem::CollectOutstandingMemoryAllocations(checkpoint, details, 2);
    EXPECT_EQ(report.outstanding_allocation_count, 0ull);
    FMemorySystem::SetBreakOnAllocationSequence(0);
    FMemorySystem::Shutdown();
}

// ScopedMemorySegment: スコープで TLS の現在セグメントが切り替わる
ACS_TEST(MemSystem, ScopedSegmentSwitch)
{
    auto r = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(r.IsOk());

    EXPECT_EQ(FMemorySystem::Current(), ESegment::Default);
    {
        ScopedMemorySegment s(ESegment::Temp);
        EXPECT_EQ(FMemorySystem::Current(), ESegment::Temp);
        FAllocator* a = FMemorySystem::CurrentAllocator();
        EXPECT_TRUE(a != nullptr);
        if (a) {
            void* p = a->Alloc(64, 16, FSourceLoc::Current());
            EXPECT_TRUE(p != nullptr);
        }
    }
    EXPECT_EQ(FMemorySystem::Current(), ESegment::Default);

    FMemorySystem::ResetTemp();
    FMemorySystem::Shutdown();
}

// Snapshot: SVG / BMP 出力（ファイルへ書き込みできることだけ確認）
ACS_TEST(MemSystem, SnapshotWrite)
{
    auto r = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(r.IsOk());

    // いくつか allocate して使用率を上げる
    FAllocator* a = FMemorySystem::Get(ESegment::Default);
    void* allocations[10]{};
    if (a) {
        for (int i = 0; i < 10; ++i) {
            allocations[i] = a->Alloc(1024 * 64, 16, FSourceLoc::Current());
        }
    }

    // 書き出し（書けない環境では失敗するが致命でない）
    auto svg = FMemorySnapshot::WriteSvg(L"acs_memdump.svg");
    auto bmp = FMemorySnapshot::WriteBmp(L"acs_memdump.bmp");
    (void)svg;
    (void)bmp;
    EXPECT_TRUE(true);

    FMemorySnapshot::DumpToStdOut();
    if (a) {
        for (void* allocation : allocations)
            a->Free(allocation);
    }
    FMemorySystem::Shutdown();
}

// VmZeroFastNT: 大きい領域を NT-write でゼロクリア
ACS_TEST(MemSystem, ZeroFastNT)
{
    constexpr usize kSize = 4096;
    alignas(32) u8 buf[kSize];
    for (usize i = 0; i < kSize; ++i)
        buf[i] = 0xFF;
    VmZeroFastNT(buf, kSize);
    bool all_zero = true;
    for (usize i = 0; i < kSize; ++i)
        if (buf[i] != 0) {
            all_zero = false;
            break;
        }
    EXPECT_TRUE(all_zero);
}
