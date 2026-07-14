// SPDX-License-Identifier: Apache-2.0
// =============================================================================
// ACS Memory - VirtualMemory 単体契約テスト
// =============================================================================
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/VirtualMemory.h"
#include "foundation/Platform.h"

using namespace acs;

namespace {

/** 指定アドレスに対応する現在の仮想メモリ状態を返す。 */
DWORD QueryVirtualMemoryStateForTest(const void* Address) noexcept
{
    MEMORY_BASIC_INFORMATION Information{};
    return ::VirtualQuery(Address, &Information, sizeof(Information)) != 0 ? Information.State : 0;
}

} // namespace

// バイト上限を超えた最古範囲を実デコミットし、巨大な単一範囲は保持しない。
ACS_TEST(VirtualMemory, DecommitCacheHonorsByteLimit)
{
    const usize PageSize = VmPageSize();
    auto ReservationResult = VmReservation::Reserve(PageSize * 4);
    EXPECT_TRUE(ReservationResult.IsOk());
    if (ReservationResult.IsErr()) {
        return;
    }

    VmReservation& Reservation = ReservationResult.Value();
    EXPECT_EQ(Reservation.MaximumCachedDecommitBytes(), VmReservation::kDefaultMaximumCachedDecommitBytes);
    EXPECT_TRUE(Reservation.SetMaximumCachedDecommitBytes(PageSize * 2).IsOk());
    EXPECT_TRUE(Reservation.Commit(0, PageSize * 3).IsOk());

    EXPECT_TRUE(Reservation.Decommit(0, PageSize).IsOk());
    EXPECT_TRUE(Reservation.Decommit(PageSize, PageSize).IsOk());
    EXPECT_TRUE(Reservation.Decommit(PageSize * 2, PageSize).IsOk());
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize * 2);
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 2u);
    EXPECT_EQ(QueryVirtualMemoryStateForTest(Reservation.Base()), static_cast<DWORD>(MEM_RESERVE));
    EXPECT_EQ(QueryVirtualMemoryStateForTest(static_cast<u8*>(Reservation.Base()) + PageSize),
              static_cast<DWORD>(MEM_COMMIT));

    EXPECT_TRUE(Reservation.SetMaximumCachedDecommitBytes(PageSize).IsOk());
    EXPECT_EQ(Reservation.MaximumCachedDecommitBytes(), PageSize);
    EXPECT_EQ(Reservation.CachedCommittedBytes(), PageSize);
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 1u);

    EXPECT_TRUE(Reservation.SetMaximumCachedDecommitBytes(0).IsOk());
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 0u);
    EXPECT_TRUE(Reservation.Commit(0, PageSize * 3).IsOk());
    EXPECT_TRUE(Reservation.Decommit(0, PageSize * 3).IsOk());
    EXPECT_EQ(Reservation.CachedCommittedBytes(), static_cast<usize>(0));
    EXPECT_EQ(Reservation.DecommitCacheEntryCount(), 0u);
    EXPECT_EQ(QueryVirtualMemoryStateForTest(Reservation.Base()), static_cast<DWORD>(MEM_RESERVE));
}

// AVX の実行可否に関係なく、整列経路と memset フォールバックが同じ結果を返す。
ACS_TEST(VirtualMemory, ZeroFastNonTemporalStoresHaveSafeFallback)
{
    alignas(32) u8 Buffer[1024]{};
    for (usize Index = 0; Index < sizeof(Buffer); ++Index) {
        Buffer[Index] = 0xA5u;
    }

    VmZeroFastNT(nullptr, sizeof(Buffer));
    VmZeroFastNT(Buffer, sizeof(Buffer));
    for (usize Index = 0; Index < sizeof(Buffer); ++Index) {
        EXPECT_EQ(Buffer[Index], static_cast<u8>(0));
    }

    for (usize Index = 0; Index < sizeof(Buffer); ++Index) {
        Buffer[Index] = 0x5Au;
    }
    VmZeroFastNT(Buffer + 1, sizeof(Buffer) - 1);
    EXPECT_EQ(Buffer[0], static_cast<u8>(0x5Au));
    for (usize Index = 1; Index < sizeof(Buffer); ++Index) {
        EXPECT_EQ(Buffer[Index], static_cast<u8>(0));
    }
}
