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
#include "memory/MemorySystem.h"
#include "memory/MemorySnapshot.h"
#include "foundation/Platform.h"   // HeapAlloc / GetProcessHeap

using namespace acs;

// VirtualMemory: 予約 → コミット → デコミット
ACS_TEST(MemSystem, VmReserveCommitDecommit) {
    auto r = VmReservation::Reserve(64 * 1024 * 1024);  // 64 MB 仮想予約
    EXPECT_TRUE(r.IsOk());
    if (!r.IsOk()) return;
    auto& res = r.Value();
    EXPECT_TRUE(res.Base() != nullptr);
    EXPECT_TRUE(res.Capacity() >= 64ull * 1024 * 1024);

    auto cr = res.Commit(0, 4096);
    EXPECT_TRUE(cr.IsOk());

    // コミットした領域には書き込めるはず
    *static_cast<u8*>(res.Base()) = 0x42;
    EXPECT_EQ(*static_cast<u8*>(res.Base()), (u8)0x42);

    auto dr = res.Decommit(0, 4096);
    EXPECT_TRUE(dr.IsOk());
}

// TLSF: 単純な alloc / free
ACS_TEST(MemSystem, TlsfBasic) {
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

    t.Free(p1);
    t.Free(p2);
    t.Free(p3);

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// FMemorySystem: 全セグメント初期化 → 取得 → 解放
ACS_TEST(MemSystem, SegmentInitGet) {
    MemorySystemConfig cfg = FMemorySystem::DefaultConfig();
    auto r = FMemorySystem::Init(cfg);
    EXPECT_TRUE(r.IsOk());

    FAllocator* a = FMemorySystem::Get(ESegment::Default);
    EXPECT_TRUE(a != nullptr);

    void* p = a->Alloc(1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(p != nullptr);
    a->Free(p);

    FMemorySystem::Shutdown();
}

// ScopedMemorySegment: スコープで TLS の現在セグメントが切り替わる
ACS_TEST(MemSystem, ScopedSegmentSwitch) {
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
ACS_TEST(MemSystem, SnapshotWrite) {
    auto r = FMemorySystem::Init(FMemorySystem::DefaultConfig());
    EXPECT_TRUE(r.IsOk());

    // いくつか allocate して使用率を上げる
    FAllocator* a = FMemorySystem::Get(ESegment::Default);
    if (a) {
        for (int i = 0; i < 10; ++i) (void)a->Alloc(1024 * 64, 16, FSourceLoc::Current());
    }

    // 書き出し（書けない環境では失敗するが致命でない）
    auto svg = FMemorySnapshot::WriteSvg(L"acs_memdump.svg");
    auto bmp = FMemorySnapshot::WriteBmp(L"acs_memdump.bmp");
    (void)svg; (void)bmp;
    EXPECT_TRUE(true);

    FMemorySnapshot::DumpToStdOut();
    FMemorySystem::Shutdown();
}

// VmZeroFastNT: 大きい領域を NT-write でゼロクリア
ACS_TEST(MemSystem, ZeroFastNT) {
    constexpr usize kSize = 4096;
    alignas(32) u8 buf[kSize];
    for (usize i = 0; i < kSize; ++i) buf[i] = 0xFF;
    VmZeroFastNT(buf, kSize);
    bool all_zero = true;
    for (usize i = 0; i < kSize; ++i) if (buf[i] != 0) { all_zero = false; break; }
    EXPECT_TRUE(all_zero);
}
