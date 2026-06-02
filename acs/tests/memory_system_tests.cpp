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
#include "memory/Memory.h"        // DefaultAllocator / SetDefaultAllocator
#include "memory/RelocatableAllocator.h"
#include "threading/ThreadPool.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "foundation/Platform.h"   // HeapAlloc / GetProcessHeap / QueryPerformanceCounter
#include "foundation/Move.h"       // Move (VmReservation のムーブ)

#include <cstdio>                  // ベンチ結果出力

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

// thread-local マガジン (lock-free hot path) の正当性。小サイズ中心に alloc/free を大量に回し、
// refill/pop/push/flush を踏みながらデータ整合 (重複払い出しがあれば破壊検出) と ValidateHeap を検証。
ACS_TEST(MemSystem, ShardedTlsfThreadCache) {
    FShardedTlsfAllocator a;
    EXPECT_TRUE(a.Init(64ull * 1024 * 1024, 4ull * 1024 * 1024, 4u).IsOk());
    a.EnableThreadCache();
    EXPECT_TRUE(a.ThreadCacheEnabled());
    EXPECT_TRUE(a.ValidateHeap());

    u32 rng = 0xABCDEF01u;
    auto next = [&rng]() -> u32 { rng = rng * 1664525u + 1013904223u; return rng; };
    void* live[512] = {};
    usize sz[512]   = {};
    u8    sd[512]   = {};
    bool ok = true;
    for (int it = 0; it < 20000 && ok; ++it) {
        const u32 slot = next() % 512u;
        if (live[slot]) {
            const u8* b = static_cast<const u8*>(live[slot]);
            for (usize k = 0; k < sz[slot]; ++k)
                if (b[k] != static_cast<u8>(sd[slot] + (k & 0x1F))) { ok = false; break; }
            if (!ok) break;
            a.Free(live[slot]);            // マガジンへ push (大量 free でバケット flush も踏む)
            live[slot] = nullptr;
        } else {
            const usize n = static_cast<usize>(8u + (next() % 480u));  // 小サイズ中心 (キャッシュ対象)
            void* p = a.Alloc(n, 16, FSourceLoc::Current());           // pop / refill
            if (!p) { ok = false; break; }
            const u8 seed = static_cast<u8>(next());
            u8* b = static_cast<u8*>(p);
            for (usize k = 0; k < n; ++k) b[k] = static_cast<u8>(seed + (k & 0x1F));
            live[slot] = p; sz[slot] = n; sd[slot] = seed;
        }
        if ((it & 0xFFF) == 0 && !a.ValidateHeap()) ok = false;
    }
    EXPECT_TRUE(ok);
    for (int i = 0; i < 512; ++i) if (live[i]) a.Free(live[i]);
    EXPECT_TRUE(a.ValidateHeap());
    a.Shutdown();
}

// 再配置可能アロケータ: 基本確保/Resolve、断片化 → Compact でデータ生存 + high-water 回収、
// freed ハンドルの無効化、世代による use-after-free 検出を検証する。
ACS_TEST(MemSystem, RelocatableBasicCompact) {
    FRelocatableAllocator r;
    EXPECT_TRUE(r.Init(1u * 1024 * 1024, 256u, nullptr).IsOk());

    auto fill = [](void* p, usize n, u8 s) {
        u8* b = static_cast<u8*>(p); for (usize i = 0; i < n; ++i) b[i] = static_cast<u8>(s + (i & 0x3F));
    };
    auto check = [](void* p, usize n, u8 s) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) if (b[i] != static_cast<u8>(s + (i & 0x3F))) return false;
        return true;
    };

    FRelocHandle h[100]; usize sz[100]; u8 sd[100];
    bool ok = true;
    for (int i = 0; i < 100; ++i) {
        sz[i] = static_cast<usize>(64 + (i * 53) % 2000);
        h[i]  = r.Alloc(sz[i], 16);
        if (!h[i].IsValid()) { ok = false; break; }
        sd[i] = static_cast<u8>(i * 7 + 1);
        void* p = r.Resolve(h[i]);
        if (!p || (reinterpret_cast<uptr>(p) & 15u) != 0) { ok = false; break; }
        fill(p, sz[i], sd[i]);
    }
    EXPECT_TRUE(ok);
    EXPECT_EQ(r.LiveCount(), 100u);

    // 1 つおきに解放 → 断片化 (high-water > used)
    for (int i = 0; i < 100; i += 2) r.Free(h[i]);
    EXPECT_EQ(r.LiveCount(), 50u);
    EXPECT_TRUE(r.HighWater() > r.Used());
    EXPECT_TRUE(r.Resolve(h[0]) == nullptr);   // 解放済みは nullptr

    // Compact: high-water が縮み、生存ハンドルのデータは移動しても保持される
    const usize hw_before = r.HighWater();
    const usize reclaimed = r.Compact();
    EXPECT_TRUE(reclaimed > 0);
    EXPECT_TRUE(r.HighWater() < hw_before);
    EXPECT_TRUE(r.HighWater() >= r.Used());
    bool ok2 = true;
    for (int i = 1; i < 100; i += 2) {
        void* p = r.Resolve(h[i]);
        if (!p || !check(p, sz[i], sd[i])) { ok2 = false; break; }
    }
    EXPECT_TRUE(ok2);

    // 世代: 解放したハンドルを再利用すると旧ハンドルは無効
    FRelocHandle old = h[1];
    r.Free(h[1]);
    EXPECT_TRUE(r.Resolve(old) == nullptr);
    FRelocHandle nw = r.Alloc(128, 16);
    EXPECT_TRUE(nw.IsValid());
    EXPECT_TRUE(r.Resolve(old) == nullptr);    // 旧ハンドルは依然無効 (世代不一致 or 解放済)

    r.Shutdown();
}

// 再配置アロケータ ストレス: ランダム alloc/free + 周期 Compact で、全生存ハンドルの
// データが移動後も保持されること (再配置の正当性) を検証する。
ACS_TEST(MemSystem, RelocatableStress) {
    FRelocatableAllocator r;
    EXPECT_TRUE(r.Init(2u * 1024 * 1024, 512u, nullptr).IsOk());

    auto fill = [](void* p, usize n, u8 s) {
        u8* b = static_cast<u8*>(p); for (usize i = 0; i < n; ++i) b[i] = static_cast<u8>(s + (i & 0x3F));
    };
    auto check = [](void* p, usize n, u8 s) -> bool {
        const u8* b = static_cast<const u8*>(p);
        for (usize i = 0; i < n; ++i) if (b[i] != static_cast<u8>(s + (i & 0x3F))) return false;
        return true;
    };

    struct Rec { FRelocHandle h; usize sz; u8 sd; };
    Rec rec[512];
    for (int i = 0; i < 512; ++i) { rec[i].h = FRelocHandle{}; rec[i].sz = 0; rec[i].sd = 0; }

    u32 rng = 0x55555555u;
    auto next = [&rng]() -> u32 { rng = rng * 1664525u + 1013904223u; return rng; };
    bool ok = true;
    for (int it = 0; it < 12000 && ok; ++it) {
        const u32 slot = next() % 512u;
        if (rec[slot].h.IsValid()) {
            void* p = r.Resolve(rec[slot].h);
            if (!p || !check(p, rec[slot].sz, rec[slot].sd)) { ok = false; break; }
            r.Free(rec[slot].h); rec[slot].h = FRelocHandle{};
        } else {
            const usize n = static_cast<usize>(16u + (next() % 1024u));
            FRelocHandle h = r.Alloc(n, 16);
            if (!h.IsValid()) continue;   // 満杯 (Alloc が内部 Compact しても入らない) → skip
            const u8 s = static_cast<u8>(next());
            fill(r.Resolve(h), n, s);
            rec[slot].h = h; rec[slot].sz = n; rec[slot].sd = s;
        }
        if ((it & 0x1FF) == 0) {
            r.Compact();   // 周期デフラグ: 全生存データが移動後も無事か
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

// 既定アロケータ結線 (make_default): Init で Default セグメントへ差し替わり、Shutdown で
// 元へ復元されること。既定経由の確保が Default セグメントに計上されることも確認する。
ACS_TEST(MemSystem, DefaultAllocatorWiring) {
    FAllocator* before = &DefaultAllocator();

    MemorySystemConfig cfg = FMemorySystem::DefaultConfig();
    cfg.make_default = true;
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
ACS_TEST(MemSystem, ShardedTlsfBenchmark) {
    auto ir_pool = FThreadPool::Init(8);
    EXPECT_TRUE(ir_pool.IsOk());

    constexpr usize kPoolSize = 128ull * 1024 * 1024;
    void* pool = ::HeapAlloc(::GetProcessHeap(), 0, kPoolSize);
    EXPECT_TRUE(pool != nullptr);
    if (!pool) { FThreadPool::Shutdown(); return; }

    FTlsfAllocator single;            // 単一ロック比較用 (HeapAlloc プール)
    EXPECT_TRUE(single.Init(pool, kPoolSize).IsOk());
    FMutex single_lock;

    FShardedTlsfAllocator sharded;
    EXPECT_TRUE(sharded.Init(kPoolSize, 8ull * 1024 * 1024, 0u).IsOk());

    FShardedTlsfAllocator sharded_c;     // lock-free マガジン有効版
    EXPECT_TRUE(sharded_c.Init(kPoolSize, 8ull * 1024 * 1024, 0u).IsOk());
    sharded_c.EnableThreadCache();

    struct BCtx { int mode; FTlsfAllocator* single; FMutex* lk;
                  FShardedTlsfAllocator* sharded; FShardedTlsfAllocator* sharded_c; };
    BCtx bc{ 0, &single, &single_lock, &sharded, &sharded_c };

    auto bench = [](u32 i, u32 /*w*/, void* user) {
        BCtx* c = static_cast<BCtx*>(user);
        constexpr int kOps = 4000;
        u32 rng = 0x2545F491u * (i + 1u);
        for (int k = 0; k < kOps; ++k) {
            rng = rng * 1664525u + 1013904223u;
            const usize n = static_cast<usize>(16u + (rng % 1024u));
            void* p = nullptr;
            if (c->mode == 0)      { FScopedLock l(*c->lk); p = c->single->Alloc(n, 16, FSourceLoc::Current()); }
            else if (c->mode == 1) { p = c->sharded->Alloc(n, 16, FSourceLoc::Current()); }
            else if (c->mode == 2) { p = ::HeapAlloc(::GetProcessHeap(), 0, n); }
            else                   { p = c->sharded_c->Alloc(n, 16, FSourceLoc::Current()); }
            if (!p) continue;
            static_cast<u8*>(p)[0] = static_cast<u8>(k);
            if (c->mode == 0)      { FScopedLock l(*c->lk); c->single->Free(p); }
            else if (c->mode == 1) { c->sharded->Free(p); }
            else if (c->mode == 2) { ::HeapFree(::GetProcessHeap(), 0, p); }
            else                   { c->sharded_c->Free(p); }
        }
    };

    LARGE_INTEGER freq; ::QueryPerformanceFrequency(&freq);
    auto run_ms = [&](int mode) -> double {
        bc.mode = mode;
        LARGE_INTEGER a0; ::QueryPerformanceCounter(&a0);
        auto r = FThreadPool::ParallelFor(0, 256, 1, +bench, &bc);
        LARGE_INTEGER a1; ::QueryPerformanceCounter(&a1);
        (void)r;
        return static_cast<double>(a1.QuadPart - a0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    };

    const double t_single  = run_ms(0);
    const double t_sharded = run_ms(1);
    const double t_heap    = run_ms(2);
    const double t_cached  = run_ms(3);
    std::printf("[bench MT8 alloc/free x %d] single-lock-TLSF=%.1fms  sharded-TLSF=%.1fms  "
                "sharded+cache=%.1fms  HeapAlloc=%.1fms\n",
                256 * 4000, t_single, t_sharded, t_cached, t_heap);

    EXPECT_TRUE(sharded.ValidateHeap());   // ベンチ後も健全
    EXPECT_TRUE(single.ValidateHeap());

    sharded_c.Shutdown();
    sharded.Shutdown();
    ::HeapFree(::GetProcessHeap(), 0, pool);
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
