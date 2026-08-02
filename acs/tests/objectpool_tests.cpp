// SPDX-License-Identifier: Apache-2.0
// TObjectPool<T> / FObjectHandle / TPoolRef<T> のテスト (世代付きスロットマッププール)。
#include "test/Test.h"
#include "test/Expect.h"
#include "memory/ObjectPool.h"
#include "memory/New.h"
#include "memory/SystemAllocator.h"
#include "platform/Time.h"
#include "foundation/Log.h"

using namespace acs;

namespace {

int g_live = 0;   // 生存中インスタンス数 (各テスト先頭で 0 に戻す)

struct FThing {
    int value;
    explicit FThing(int v = 0) noexcept : value(v) { ++g_live; }
    ~FThing() noexcept { --g_live; }
};

/** 指定した次回確保だけを失敗させ、rollback を検証する backing。 */
class FControllableFailAllocator final : public IAllocator {
public:
    void FailNextAllocation() noexcept
    {
        m_bFailNext = true;
    }

    void* Alloc(usize Size, usize Alignment, FSourceLoc Location) noexcept override
    {
        if (m_bFailNext) {
            m_bFailNext = false;
            return nullptr;
        }
        return m_Backing.Alloc(Size, Alignment, Location);
    }

    void Free(void* Pointer) noexcept override
    {
        m_Backing.Free(Pointer);
    }

    u64 AllocationCount() const noexcept override
    {
        return m_Backing.AllocationCount();
    }

private:
    CSystemAllocator m_Backing;
    bool m_bFailNext = false;
};

} // namespace

ACS_TEST(ObjectPool, CreateGetDestroy) {
    g_live = 0;
    TObjectPool<FThing> pool;
    EXPECT_EQ(pool.Count(), (u32)0);

    FObjectHandle h = pool.Create(42);
    EXPECT_TRUE(h.IsSet());
    EXPECT_EQ(pool.Count(), (u32)1);
    EXPECT_EQ(g_live, 1);

    FThing* t = pool.Get(h);
    EXPECT_TRUE(t != nullptr);
    EXPECT_EQ(t->value, 42);
    EXPECT_TRUE(pool.IsAlive(h));

    EXPECT_TRUE(pool.Destroy(h));
    EXPECT_EQ(pool.Count(), (u32)0);
    EXPECT_EQ(g_live, 0);
    EXPECT_TRUE(pool.Get(h) == nullptr);   // 解放後は nullptr
    EXPECT_TRUE(!pool.IsAlive(h));
    EXPECT_TRUE(!pool.Destroy(h));         // 二重解放は false
}

ACS_TEST(ObjectPool, StaleHandleAfterReuse) {
    g_live = 0;
    TObjectPool<FThing> pool;
    FObjectHandle h1 = pool.Create(1);
    pool.Destroy(h1);
    // スロットが再利用されても、古いハンドル h1 は世代不一致で無効。
    FObjectHandle h2 = pool.Create(2);
    EXPECT_EQ(h2.index, h1.index);         // 同じスロットを再利用
    EXPECT_TRUE(h2.gen != h1.gen);         // ただし世代が違う
    EXPECT_TRUE(pool.Get(h1) == nullptr);  // 古いハンドルは無効
    EXPECT_TRUE(pool.Get(h2) != nullptr);
    EXPECT_EQ(pool.Get(h2)->value, 2);
}

ACS_TEST(ObjectPool, StableAddressesAcrossChunks) {
    g_live = 0;
    TObjectPool<FThing> pool;
    constexpr int N = 1000;   // kChunkSize(256) を跨ぐ
    FObjectHandle hs[N];
    FThing* ptrs[N];
    for (int i = 0; i < N; ++i) { hs[i] = pool.Create(i); ptrs[i] = pool.Get(hs[i]); }
    EXPECT_EQ(pool.Count(), (u32)N);
    EXPECT_EQ(g_live, N);
    // さらに確保してもアドレスは動かない (チャンク格納)。
    for (int i = 0; i < 500; ++i) pool.Create(9999);
    bool stable = true, values = true;
    for (int i = 0; i < N; ++i) {
        if (pool.Get(hs[i]) != ptrs[i]) stable = false;
        if (pool.Get(hs[i])->value != i) values = false;
    }
    EXPECT_TRUE(stable);
    EXPECT_TRUE(values);
}

ACS_TEST(ObjectPool, ForEachLiveOnly) {
    g_live = 0;
    TObjectPool<FThing> pool;
    FObjectHandle a = pool.Create(10);
    FObjectHandle b = pool.Create(20);
    FObjectHandle c = pool.Create(30);
    pool.Destroy(b);   // 中央を消す → 密リストの swap-remove

    int sum = 0, count = 0;
    pool.ForEach([&](FThing& t, FObjectHandle) { sum += t.value; ++count; });
    EXPECT_EQ(count, 2);
    EXPECT_EQ(sum, 40);            // 10 + 30 (b=20 は除外)
    EXPECT_EQ(pool.Count(), (u32)2);
    EXPECT_TRUE(pool.Get(a) != nullptr && pool.Get(c) != nullptr);
    (void)a; (void)c;
}

ACS_TEST(ObjectPool, ClearDestroysAll) {
    g_live = 0;
    TObjectPool<FThing> pool;
    FObjectHandle hs[5];
    for (int i = 0; i < 5; ++i) hs[i] = pool.Create(i);
    EXPECT_EQ(g_live, 5);
    pool.Clear();
    EXPECT_EQ(pool.Count(), (u32)0);
    EXPECT_EQ(g_live, 0);
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(pool.Get(hs[i]) == nullptr);   // 全ハンドル無効
    // Clear 後も再利用できる。
    FObjectHandle h = pool.Create(77);
    EXPECT_TRUE(pool.Get(h) != nullptr);
    EXPECT_EQ(pool.Get(h)->value, 77);
}

ACS_TEST(ObjectPool, AllocationFailuresPreserveOwnershipAndState)
{
    g_live = 0;
    FControllableFailAllocator Allocator;

    {
        TObjectPool<FThing> Pool(Allocator);
        Allocator.FailNextAllocation();
        const FObjectHandle FailedCreate = Pool.Create(1);
        EXPECT_FALSE(FailedCreate.IsSet());
        EXPECT_EQ(Pool.Count(), 0u);
        EXPECT_EQ(g_live, 0);
        EXPECT_EQ(Allocator.AllocationCount(), 0ull);

        const FObjectHandle First = Pool.Create(10);
        const FObjectHandle Second = Pool.Create(20);
        EXPECT_TRUE(First.IsSet());
        EXPECT_TRUE(Second.IsSet());
        EXPECT_EQ(Pool.Count(), 2u);
        EXPECT_EQ(g_live, 2);

        Allocator.FailNextAllocation();
        EXPECT_FALSE(Pool.Destroy(First));
        EXPECT_TRUE(Pool.IsAlive(First));
        EXPECT_EQ(Pool.Count(), 2u);
        EXPECT_EQ(g_live, 2);

        Allocator.FailNextAllocation();
        EXPECT_FALSE(Pool.TryClear());
        EXPECT_TRUE(Pool.IsAlive(First));
        EXPECT_TRUE(Pool.IsAlive(Second));
        EXPECT_EQ(Pool.Count(), 2u);
        EXPECT_EQ(g_live, 2);

        EXPECT_TRUE(Pool.TryClear());
        EXPECT_EQ(Pool.Count(), 0u);
        EXPECT_EQ(g_live, 0);
    }

    EXPECT_EQ(Allocator.AllocationCount(), 0ull);
}

// 「速い」ことを数値で示す参考ベンチ (assert は churn が壊れないことのみ。数値はログ出力)。
ACS_TEST(ObjectPool, Benchmark_ChurnSpeed) {
    struct FCell { int a, b, c, d; };
    const int kIters = 1000000;
    const u64 freq = CClock::TicksPerSecond();

    // プール: create+destroy の churn (スロット再利用 → アロケータ呼び出し無し)。
    TObjectPool<FCell> pool;
    u64 t0 = CClock::Ticks();
    for (int i = 0; i < kIters; ++i) {
        FObjectHandle h = pool.Create();
        pool.Get(h)->a = i;
        pool.Destroy(h);
    }
    u64 t1 = CClock::Ticks();

    // ヒープ: New/Delete の churn (アロケータ往復)。
    IAllocator& a = DefaultAllocator();
    volatile int sink = 0;
    u64 t2 = CClock::Ticks();
    for (int i = 0; i < kIters; ++i) {
        FCell* c = New<FCell>(a);
        c->a = i; sink = static_cast<int>(sink + c->a);
        Delete(a, c);
    }
    u64 t3 = CClock::Ticks();
    (void)sink;

    const int poolNs = (freq > 0) ? static_cast<int>((t1 - t0) * 1000000000ull / freq / (u64)kIters) : 0;
    const int heapNs = (freq > 0) ? static_cast<int>((t3 - t2) * 1000000000ull / freq / (u64)kIters) : 0;
    ACS_LOG_INFO("ObjectPool churn (%d iters): pool=%d ns/op, heap New/Delete=%d ns/op", kIters, poolNs, heapNs);
    EXPECT_EQ(pool.Count(), (u32)0);
}

ACS_TEST(ObjectPool, PoolRefSafeAccess) {
    g_live = 0;
    TObjectPool<FThing> pool;
    FObjectHandle h = pool.Create(5);
    TPoolRef<FThing> ref(pool, h);
    EXPECT_TRUE(ref.IsValid());
    EXPECT_EQ(ref->value, 5);
    pool.Destroy(h);
    EXPECT_TRUE(!ref.IsValid());
    EXPECT_TRUE(ref.Get() == nullptr);   // 破棄後は安全に nullptr
}
