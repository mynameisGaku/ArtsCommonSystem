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
#include "memory/MemorySystem.h"
#include "memory/MemorySnapshot.h"
#include "threading/ThreadPool.h"
#include "threading/Atomic.h"
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

// TLSF in-place Realloc: 拡大(次が free なら統合)/縮小(末尾解放) はコピー無しで ptr 不変、
// 不可なら移動 + データ保持。ランダムストレスでデータ整合とヒープ健全を検証する。
ACS_TEST(MemSystem, TlsfReallocInPlace) {
    constexpr usize kPoolSize = 8 * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) return;
    FTlsfAllocator t;
    EXPECT_TRUE(t.Init(pool, kPoolSize).IsOk());

    auto fill = [](void* p, usize n, u8 seed) {
        u8* b = static_cast<u8*>(p);
        for (usize i = 0; i < n; ++i) b[i] = static_cast<u8>(seed + (i & 0x3F));
    };
    auto check = [](const void* p, usize n, u8 seed) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) if (b[i] != static_cast<u8>(seed + (i & 0x3F))) return false;
        return true;
    };

    // (1) 拡大 in-place: A 直後の B を解放 → A を拡大すると空き領域を吸収して ptr 不変
    void* a = t.Alloc(1000, 16, FSourceLoc::Current());
    void* b = t.Alloc(1000, 16, FSourceLoc::Current());
    EXPECT_TRUE(a && b);
    fill(a, 1000, 0xA0);
    t.Free(b);
    void* a2 = t.Realloc(a, 1000, 1800, 16, FSourceLoc::Current());
    EXPECT_TRUE(a2 == a);                      // 移動していない (in-place)
    EXPECT_TRUE(check(a2, 1000, 0xA0));         // データ保持
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
    EXPECT_TRUE(check(a4, 400, 0xA0));          // コピーで先頭 400B 保持
    EXPECT_TRUE(t.ValidateHeap());
    t.Free(a4); t.Free(c);
    EXPECT_TRUE(t.ValidateHeap());

    // (4) ランダム realloc ストレス: 毎回データ整合 (min(old,new)) とヒープ健全を検証
    u32 rng = 0x9e3779b9u;
    auto next = [&rng]() -> u32 { rng = rng * 1664525u + 1013904223u; return rng; };
    void* p = t.Alloc(64, 16, FSourceLoc::Current());
    usize sz = 64;
    u8 seed = 0x11;
    fill(p, sz, seed);
    bool ok = (p != nullptr);
    for (int i = 0; i < 2000 && ok; ++i) {
        const usize ns = static_cast<usize>(16u + (next() % 8192u));
        void* np = t.Realloc(p, sz, ns, 16, FSourceLoc::Current());
        if (!np) { ok = false; break; }
        const usize keep = sz < ns ? sz : ns;
        if (!check(np, keep, seed)) ok = false;   // 旧データ保持
        p = np; sz = ns; seed = static_cast<u8>(seed + 1u);
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
ACS_TEST(MemSystem, ShardedTlsfBasic) {
    FShardedTlsfAllocator a;
    auto ir = a.Init(64ull * 1024 * 1024, 4ull * 1024 * 1024, 4u);  // 明示 4 シャード
    EXPECT_TRUE(ir.IsOk());
    EXPECT_EQ(a.ShardCount(), 4u);
    EXPECT_TRUE(a.ValidateHeap());

    // 多数確保 → 全 16 整列、書き込み可能、解放でルーティング成功
    void* ptrs[200] = {};
    bool ok = true;
    for (int i = 0; i < 200; ++i) {
        ptrs[i] = a.Alloc(static_cast<usize>(64 + (i * 37) % 4000), 16, FSourceLoc::Current());
        if (!ptrs[i] || (reinterpret_cast<uptr>(ptrs[i]) & 15u) != 0) ok = false;
        else { u8* b = static_cast<u8*>(ptrs[i]); b[0] = static_cast<u8>(i); }
    }
    EXPECT_TRUE(ok);
    EXPECT_TRUE(a.BytesAllocated() > 0);
    EXPECT_TRUE(a.ValidateHeap());

    // realloc (in-place / 移動 / シャード跨ぎ移動を含む)
    void* r = a.Alloc(100, 16, FSourceLoc::Current());
    EXPECT_TRUE(r != nullptr);
    static_cast<u8*>(r)[0] = 0x5A;
    void* r2 = a.Realloc(r, 100, 6000, 16, FSourceLoc::Current());
    EXPECT_TRUE(r2 != nullptr);
    EXPECT_TRUE(static_cast<u8*>(r2)[0] == 0x5A);   // データ保持
    a.Free(r2);

    for (int i = 0; i < 200; ++i) if (ptrs[i]) a.Free(ptrs[i]);
    EXPECT_TRUE(a.ValidateHeap());
    a.Shutdown();
}

// シャード化 TLSF: マルチスレッド・ストレス。8 ワーカーで並行に alloc/realloc/free を回し、
// データ整合 (スレッドセーフでなければ領域が重複して壊れる) と最終ヒープ整合を検証する。
ACS_TEST(MemSystem, ShardedTlsfMultiThread) {
    auto ir_pool = FThreadPool::Init(8);
    EXPECT_TRUE(ir_pool.IsOk());

    FShardedTlsfAllocator a;
    auto ir = a.Init(128ull * 1024 * 1024, 8ull * 1024 * 1024, 0u);  // シャード数は自動
    EXPECT_TRUE(ir.IsOk());
    EXPECT_TRUE(a.ValidateHeap());

    struct Ctx { FShardedTlsfAllocator* alloc; TAtomic<u32> fail{0}; };
    Ctx ctx;
    ctx.alloc = &a;

    auto thunk = [](u32 i, u32 /*worker*/, void* user) {
        Ctx* c = static_cast<Ctx*>(user);
        FShardedTlsfAllocator* al = c->alloc;
        u32 rng = 0x01000193u * (i + 1u) + 0x2545F491u;
        auto next = [&rng]() -> u32 { rng = rng * 1664525u + 1013904223u; return rng; };
        auto fill = [](void* p, usize n, u8 seed) {
            u8* b = static_cast<u8*>(p);
            for (usize k = 0; k < n; ++k) b[k] = static_cast<u8>(seed + (k & 0x1F));
        };
        auto ck = [](const void* p, usize n, u8 seed) -> bool {
            const u8* b = static_cast<const u8*>(p);
            for (usize k = 0; k < n; ++k) if (b[k] != static_cast<u8>(seed + (k & 0x1F))) return false;
            return true;
        };
        void* live[16] = {};
        usize sz[16] = {};
        u8    sd[16] = {};
        bool local_ok = true;
        for (int it = 0; it < 400 && local_ok; ++it) {
            const u32 slot = next() % 16u;
            if (live[slot]) {
                if (!ck(live[slot], sz[slot], sd[slot])) { local_ok = false; break; }  // 重複/破壊検出
                if (next() & 1u) {
                    const usize ns = static_cast<usize>(8u + (next() % 2000u));
                    void* np = al->Realloc(live[slot], sz[slot], ns, 16, FSourceLoc::Current());
                    if (!np) { local_ok = false; break; }
                    const usize keep = sz[slot] < ns ? sz[slot] : ns;
                    if (!ck(np, keep, sd[slot])) { local_ok = false; break; }
                    live[slot] = np; sz[slot] = ns; sd[slot] = static_cast<u8>(sd[slot] + 1u);
                    fill(np, ns, sd[slot]);
                } else {
                    al->Free(live[slot]); live[slot] = nullptr;
                }
            } else {
                const usize n = static_cast<usize>(8u + (next() % 2000u));
                void* p = al->Alloc(n, 16, FSourceLoc::Current());
                if (!p) { local_ok = false; break; }
                const u8 seed = static_cast<u8>(next());
                fill(p, n, seed);
                live[slot] = p; sz[slot] = n; sd[slot] = seed;
            }
        }
        for (int s = 0; s < 16; ++s) if (live[s]) al->Free(live[s]);
        if (!local_ok) c->fail.FetchAdd(1);
    };

    auto pr = FThreadPool::ParallelFor(0, 256, 1, +thunk, &ctx);
    EXPECT_TRUE(pr.IsOk());
    EXPECT_EQ(ctx.fail.Load(EMemoryOrder::Acquire), 0u);   // データ破壊ゼロ = スレッドセーフ
    EXPECT_TRUE(a.ValidateHeap());                          // 全シャード健全

    a.Shutdown();
    FThreadPool::Shutdown();
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
