// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/SystemAllocator.h"
#include "memory/LinearAllocator.h"
#include "memory/PoolAllocator.h"
#include "memory/ArenaAllocator.h"
#include "memory/UniquePtr.h"
#include "memory/Rc.h"
#include "memory/New.h"

using namespace acs;

ACS_TEST(Memory, SystemAllocatorRoundtrips) {
    SystemAllocator a;
    void* p = a.Alloc(128, 16, SourceLoc::Current());
    EXPECT_TRUE(p != nullptr);
    EXPECT_TRUE(((uptr)p & 15ull) == 0);
    a.Free(p);
    EXPECT_EQ(a.BytesAllocated(), (u64)0);
}

ACS_TEST(Memory, LinearAllocatorBumps) {
    LinearAllocator la(4096);
    void* a = la.Alloc(64, 8, SourceLoc::Current());
    void* b = la.Alloc(64, 8, SourceLoc::Current());
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    EXPECT_TRUE(b > a);
    la.Reset();
    void* c = la.Alloc(64, 8, SourceLoc::Current());
    EXPECT_EQ(c, a);
}

ACS_TEST(Memory, PoolAllocatorReusesSlots) {
    PoolAllocator p(64, 16);
    void* a = p.Alloc(64, 8, SourceLoc::Current());
    void* b = p.Alloc(64, 8, SourceLoc::Current());
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    p.Free(a);
    void* c = p.Alloc(64, 8, SourceLoc::Current());
    EXPECT_EQ(c, a);  // LIFO reuse
    p.Free(b);
    p.Free(c);
}

ACS_TEST(Memory, ArenaGrows) {
    ArenaAllocator ar(1024);
    void* a = ar.Alloc(800, 16, SourceLoc::Current());
    void* b = ar.Alloc(800, 16, SourceLoc::Current()); // forces new page
    EXPECT_TRUE(a != nullptr);
    EXPECT_TRUE(b != nullptr);
    ar.Reset(/*release_pages*/ false);
    void* c = ar.Alloc(64, 16, SourceLoc::Current());
    EXPECT_TRUE(c != nullptr);
}

struct Probe {
    static int destroyed;
    int v;
    Probe(int x = 0) noexcept : v(x) {}
    ~Probe() noexcept { ++destroyed; }
};
int Probe::destroyed = 0;

ACS_TEST(Memory, UniquePtrDestroys) {
    Probe::destroyed = 0;
    {
        auto p = MakeUnique<Probe>(99);
        EXPECT_EQ(p->v, 99);
    }
    EXPECT_EQ(Probe::destroyed, 1);
}

ACS_TEST(Memory, RcSharesAndReleases) {
    Probe::destroyed = 0;
    {
        TRc<Probe> a = MakeRc<Probe>(7);
        TRc<Probe> b = a;
        EXPECT_EQ(a.UseCount(), 2u);
        EXPECT_EQ(a->v, 7);
    }
    EXPECT_EQ(Probe::destroyed, 1);
}
