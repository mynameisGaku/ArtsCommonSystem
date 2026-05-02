#include "test/Test.h"
#include "test/Expect.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"

using namespace acs;

ACS_TEST(Threading, AtomicAddCounts) {
    Atomic<u32> c{0};
    constexpr u32 N = 100;
    for (u32 i = 0; i < N; ++i) c.FetchAdd(1);
    EXPECT_EQ(c.Load(), N);
}

ACS_TEST(Threading, MutexExclusive) {
    Mutex m;
    int x = 0;
    {
        ScopedLock lk(m);
        x = 42;
    }
    EXPECT_EQ(x, 42);
}

ACS_TEST(Threading, ThreadJoin) {
    struct Ctx { Atomic<u32> v{0}; };
    Ctx ctx;
    auto r = Thread::Spawn([](void* p){
        static_cast<Ctx*>(p)->v.Store(123);
    }, &ctx);
    EXPECT_TRUE(r.IsOk());
    if (r.IsOk()) {
        r.Value().Join();
        EXPECT_EQ(ctx.v.Load(), 123u);
    }
}

ACS_TEST(Threading, ThreadPoolSubmitMany) {
    auto rinit = ThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());

    Atomic<u32> counter{0};
    CompletionCounter done;
    constexpr u32 N = 1000;
    for (u32 i = 0; i < N; ++i) {
        Task t {};
        t.fn = [](void* p, u32){
            static_cast<Atomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        ThreadPool::Submit(t);
    }
    ThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);

    ThreadPool::Shutdown();
}

ACS_TEST(Threading, ParallelForCovers) {
    ThreadPool::Init(4);
    Atomic<u32> seen{0};
    ThreadPool::ParallelFor(0, 10000, 64,
        [](u32 /*i*/, u32 /*w*/, void* user){
            static_cast<Atomic<u32>*>(user)->FetchAdd(1);
        }, &seen);
    EXPECT_EQ(seen.Load(), 10000u);
    ThreadPool::Shutdown();
}

// ノードプール経由で大量タスクを処理（Heap フォールバックが起きても破綻しない）
ACS_TEST(Threading, ThreadPoolHighLoad) {
    ThreadPool::Init(4);
    Atomic<u32> counter{0};
    CompletionCounter done;
    constexpr u32 N = 50000;
    for (u32 i = 0; i < N; ++i) {
        Task t {};
        t.fn = [](void* p, u32){
            static_cast<Atomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        ThreadPool::Submit(t);
    }
    ThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);
    ThreadPool::Shutdown();
}

// 入れ子 ParallelFor がデッドロックしないこと（help-stealing が効く）
ACS_TEST(Threading, NestedParallelFor) {
    ThreadPool::Init(4);
    Atomic<u32> total{0};
    ThreadPool::ParallelFor(0, 10, 1,
        [](u32 /*i*/, u32 /*w*/, void* user){
            ThreadPool::ParallelFor(0, 100, 16,
                [](u32 /*j*/, u32 /*w*/, void* u2){
                    static_cast<Atomic<u32>*>(u2)->FetchAdd(1);
                }, user);
        }, &total);
    EXPECT_EQ(total.Load(), 1000u);
    ThreadPool::Shutdown();
}
