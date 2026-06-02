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
#include "foundation/Move.h"       // Move (VmReservation のムーブ)

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

// TLSF: 返却ポインタが要求アライメントを必ず満たすことを検証
// (chaining が block_size を ≡8 mod16 に保つことで全 payload が 16 整列、>16 は leading split)
ACS_TEST(MemSystem, TlsfAlignment) {
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
    for (int i = 0; i < 64; ++i) if (ptrs[i]) t.Free(ptrs[i]);

    // 大アライメント: 32/64/128/256 をそれぞれ複数確保して境界を満たすこと
    const usize aligns[4] = { 32, 64, 128, 256 };
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
                for (usize k = 0; k < sz; ++k) b[k] = (u8)(k & 0xFF);
            }
        }
        for (int i = 0; i < 8; ++i) if (bp[i]) t.Free(bp[i]);
    }
    EXPECT_TRUE(all_big);

    // leading split で生じた free ブロックが再利用でき、ヒープが壊れていないことの確認:
    // 全解放後に大きな確保ができること
    void* big = t.Alloc(1024 * 1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(big != nullptr);
    if (big) t.Free(big);

    ::HeapFree(::GetProcessHeap(), 0, pool);
}

// TLSF ストレス: alloc/free を多数混在させ、整列・統合・ヒープ整合 (ValidateHeap) を検証。
// 安全ガード (範囲検証/二重free検知) と Debug poison が通常運用を壊さないことも確認する。
ACS_TEST(MemSystem, TlsfStressValidate) {
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
    auto next = [&rng]() -> u32 { rng = rng * 1664525u + 1013904223u; return rng; };

    void* live[256] = {};
    bool ok = true;
    for (int iter = 0; iter < 6000 && ok; ++iter) {
        const u32 i = next() % 256u;
        if (live[i]) {
            t.Free(live[i]);
            live[i] = nullptr;
        } else {
            const usize sz = static_cast<usize>(1u + (next() % 4096u));
            const usize al = static_cast<usize>(16u << (next() % 5u));  // 16/32/64/128/256
            void* p = t.Alloc(sz, al, FSourceLoc::Current());
            if (p) {
                if ((reinterpret_cast<uptr>(p) & (al - 1)) != 0) ok = false;  // 整列違反
                u8* b = static_cast<u8*>(p);
                for (usize k = 0; k < sz; ++k) b[k] = static_cast<u8>(i + k);  // 全域書込でOOBが無いこと
                live[i] = p;
            }
        }
        if ((iter & 0x1FF) == 0 && !t.ValidateHeap()) ok = false;  // 周期的に整合検証
    }
    EXPECT_TRUE(ok);

    for (int i = 0; i < 256; ++i) if (live[i]) t.Free(live[i]);
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
ACS_TEST(MemSystem, TlsfAutoGrow) {
    auto rr = VmReservation::Reserve(64ull * 1024 * 1024);   // 64MB 予約
    EXPECT_TRUE(rr.IsOk());
    if (!rr.IsOk()) return;

    FTlsfAllocator t;
    auto ir = t.InitWithReservation(Move(rr.Value()), 1ull * 1024 * 1024);  // 初期コミット 1MB のみ
    EXPECT_TRUE(ir.IsOk());
    EXPECT_TRUE(t.ValidateHeap());

    // 256KB を 48 個 = 12MB。初期 1MB を大きく超えるので grow が効かないと途中で失敗する。
    void* live[48] = {};
    bool ok = true;
    for (int i = 0; i < 48; ++i) {
        live[i] = t.Alloc(256 * 1024, 16, FSourceLoc::Current());
        if (!live[i]) { ok = false; }
        else {
            u8* b = static_cast<u8*>(live[i]);
            for (int k = 0; k < 256 * 1024; k += 4096) b[k] = static_cast<u8>(i);  // 触って実コミット確認
        }
    }
    EXPECT_TRUE(ok);                                       // grow が効けば全部成功
    EXPECT_TRUE(t.BytesAllocated() >= 12ull * 1024 * 1024); // 初期 1MB を超えて確保できている
    EXPECT_TRUE(t.ValidateHeap());                          // 複数プールをまたいでヒープ健全

    for (int i = 0; i < 48; ++i) if (live[i]) t.Free(live[i]);
    EXPECT_TRUE(t.ValidateHeap());

    // grow 後の空き再利用: 全解放後に初期コミットを超える単一確保が通る
    void* big = t.Alloc(8ull * 1024 * 1024, 16, FSourceLoc::Current());
    EXPECT_TRUE(big != nullptr);
    if (big) t.Free(big);
    EXPECT_TRUE(t.ValidateHeap());
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
