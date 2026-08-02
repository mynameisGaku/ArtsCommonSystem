// SPDX-License-Identifier: Apache-2.0
// TLSF の破損診断が不正メタデータを安全に拒否することを検証する。
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/Tlsf.h"

using namespace acs;

ACS_TEST(MemSystem, TlsfValidationRejectsOverflowingBlockSize)
{
    alignas(tlsf::ALIGN_SIZE) u8 Storage[4096] = {};
    CTlsfAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(Storage, sizeof(Storage)).IsOk());

    void* const Allocation = Allocator.Alloc(128u, tlsf::ALIGN_SIZE, FSourceLoc::Current());
    EXPECT_TRUE(Allocation != nullptr);
    EXPECT_TRUE(Allocator.ValidateHeap());

    constexpr usize kBlockStartOffset = sizeof(tlsf::FBlockHeader*) + sizeof(usize);
    auto* const Header = reinterpret_cast<tlsf::FBlockHeader*>(
        static_cast<u8*>(Allocation) - kBlockStartOffset);
    const usize OriginalState = Header->size_and_flags;

    // NextBlock の加算が同じアドレスへ wrap する値でも、診断は読み進めず拒否する。
    Header->size_and_flags = (~usize(0) & ~usize(7)) | (OriginalState & usize(7));
    EXPECT_FALSE(Allocator.ValidateHeap());

    Header->size_and_flags = OriginalState;
    EXPECT_TRUE(Allocator.ValidateHeap());
    Allocator.Free(Allocation);
    EXPECT_TRUE(Allocator.ValidateHeap());
}

ACS_TEST(MemSystem, TlsfReallocationClampsUntrustedOldSizeToPayloadCapacity)
{
    alignas(tlsf::ALIGN_SIZE) u8 Storage[8192] = {};
    CTlsfAllocator Allocator;
    EXPECT_TRUE(Allocator.Init(Storage, sizeof(Storage)).IsOk());

    auto* const Allocation = static_cast<u8*>(Allocator.Alloc(17u, tlsf::ALIGN_SIZE, FSourceLoc::Current()));
    auto* const BlockingAllocation =
        static_cast<u8*>(Allocator.Alloc(256u, tlsf::ALIGN_SIZE, FSourceLoc::Current()));
    auto* const ReallocationTarget =
        static_cast<u8*>(Allocator.Alloc(512u, tlsf::ALIGN_SIZE, FSourceLoc::Current()));
    EXPECT_TRUE(Allocation != nullptr);
    EXPECT_TRUE(BlockingAllocation != nullptr);
    EXPECT_TRUE(ReallocationTarget != nullptr);
    if (!Allocation || !BlockingAllocation || !ReallocationTarget) return;

    EXPECT_EQ(CTlsfAllocator::PayloadCapacity(Allocation), 32u);
    for (usize Index = 0u; Index < 17u; ++Index) Allocation[Index] = static_cast<u8>(Index ^ 0x5au);
    for (usize Index = 0u; Index < 256u; ++Index) BlockingAllocation[Index] = 0xa5u;
    for (usize Index = 0u; Index < 512u; ++Index) ReallocationTarget[Index] = 0x11u;
    Allocator.Free(ReallocationTarget);

    auto* const Reallocated =
        static_cast<u8*>(Allocator.Realloc(Allocation, ~usize(0), 384u, tlsf::ALIGN_SIZE, FSourceLoc::Current()));
    EXPECT_TRUE(Reallocated != nullptr);
    EXPECT_TRUE(Reallocated == ReallocationTarget);
    if (Reallocated) {
        for (usize Index = 0u; Index < 17u; ++Index) {
            EXPECT_EQ(Reallocated[Index], static_cast<u8>(Index ^ 0x5au));
        }
        // 旧 payload の直後にある別確保の 0xa5 をコピーしていないことを確認する。
        EXPECT_TRUE(Reallocated[48u] != 0xa5u);
    }

    Allocator.Free(BlockingAllocation);
    Allocator.Free(Reallocated);
    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
    EXPECT_TRUE(Allocator.ValidateHeap());
}
