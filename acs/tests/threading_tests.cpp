// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "threading/Atomic.h"
#include "threading/Mutex.h"
#include "threading/ScopedLock.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"

#ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#    define NOMINMAX
#endif
#include <windows.h>

using namespace acs;

namespace {

/** プロセスヒープ上で現在使用中のバイト数を取得する。取得失敗時は 0。 */
usize ProcessHeapBusyBytes() noexcept
{
    HANDLE heap = ::GetProcessHeap();
    if (heap == nullptr || !::HeapLock(heap)) return 0;

    usize total = 0;
    PROCESS_HEAP_ENTRY entry{};
    while (::HeapWalk(heap, &entry)) {
        if ((entry.wFlags & PROCESS_HEAP_ENTRY_BUSY) != 0) {
            total += static_cast<usize>(entry.cbData);
        }
    }
    ::HeapUnlock(heap);
    return total;
}

/** ThreadPool を指定回数だけ起動・終了する。 */
bool CycleThreadPool(u32 cycle_count) noexcept
{
    for (u32 i = 0; i < cycle_count; ++i) {
        auto result = FThreadPool::Init(2);
        if (result.IsErr()) return false;
        FThreadPool::Shutdown();
    }
    return true;
}

struct BlockingTaskContext {
    TAtomic<u32> entered{0};
    TAtomic<u32> release{0};
};

void BlockingTask(void* user, u32) noexcept
{
    auto* context = static_cast<BlockingTaskContext*>(user);
    context->entered.Store(1, EMemoryOrder::Release);
    while (context->release.Load(EMemoryOrder::Acquire) == 0)
        SleepMs(1);
}

struct OwnedTaskPayload {
    TAtomic<u32>* destroyed = nullptr;
    ~OwnedTaskPayload() noexcept
    {
        destroyed->FetchAdd(1);
    }
};

void DeleteOwnedTask(void* user, u32) noexcept
{
    delete static_cast<OwnedTaskPayload*>(user);
}

struct DelayedReleaseContext {
    BlockingTaskContext* blocked = nullptr;
};

void DelayedRelease(void* user) noexcept
{
    auto* context = static_cast<DelayedReleaseContext*>(user);
    SleepMs(20);
    context->blocked->release.Store(1, EMemoryOrder::Release);
}

struct SubmitRaceContext {
    TAtomic<u32> stop{0};
    TAtomic<u32> accepted{0};
    TAtomic<u32> executed{0};
};

void CountRaceTask(void* user, u32) noexcept
{
    static_cast<SubmitRaceContext*>(user)->executed.FetchAdd(1);
}

void SubmitRaceMain(void* user) noexcept
{
    auto* context = static_cast<SubmitRaceContext*>(user);
    while (context->stop.Load(EMemoryOrder::Acquire) == 0) {
        Task task{&CountRaceTask, context, nullptr};
        if (FThreadPool::Submit(task).IsOk())
            context->accepted.FetchAdd(1);
        else
            Yield();
    }
}

void ShutdownFromTask(void* user, u32) noexcept
{
    FThreadPool::Shutdown();
    static_cast<TAtomic<u32>*>(user)->Store(1, EMemoryOrder::Release);
}

} // namespace

ACS_TEST(Threading, AtomicAddCounts) {
    TAtomic<u32> c{0};
    constexpr u32 N = 100;
    for (u32 i = 0; i < N; ++i) c.FetchAdd(1);
    EXPECT_EQ(c.Load(), N);
}

ACS_TEST(Threading, MutexExclusive) {
    FMutex m;
    int x = 0;
    {
        FScopedLock lk(m);
        x = 42;
    }
    EXPECT_EQ(x, 42);
}

ACS_TEST(Threading, ThreadJoin) {
    struct Ctx { TAtomic<u32> v{0}; };
    Ctx ctx;
    auto r = FThread::Spawn([](void* p){
        static_cast<Ctx*>(p)->v.Store(123);
    }, &ctx);
    EXPECT_TRUE(r.IsOk());
    if (r.IsOk()) {
        r.Value().Join();
        EXPECT_EQ(ctx.v.Load(), 123u);
    }
}

ACS_TEST(Threading, ThreadNameIsOwnedBySpawnContext)
{
    struct NameCapture {
        wchar_t text[96] = {};
        u32 length = 0;
    } capture;

    wchar_t requested[96] = {};
    for (u32 i = 0; i < 95; ++i)
        requested[i] = static_cast<wchar_t>(L'A' + (i % 26));

    ThreadConfig config{};
    config.name = requested;
    auto result = FThread::Spawn(
        [](void* user) {
            auto* output = static_cast<NameCapture*>(user);
            PWSTR description = nullptr;
            if (SUCCEEDED(::GetThreadDescription(::GetCurrentThread(), &description)) && description != nullptr) {
                while (output->length + 1u < 96u && description[output->length] != L'\0') {
                    output->text[output->length] = description[output->length];
                    ++output->length;
                }
                output->text[output->length] = L'\0';
                ::LocalFree(description);
            }
        },
        &capture, config);

    EXPECT_TRUE(result.IsOk());
    if (result.IsOk()) result.Value().Join();
    EXPECT_EQ(capture.length, 63u);
    for (u32 i = 0; i < capture.length; ++i)
        EXPECT_EQ(capture.text[i], requested[i]);
}

ACS_TEST(Threading, ThreadPoolSubmitMany) {
    auto rinit = FThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());

    TAtomic<u32> counter{0};
    CompletionCounter done;
    constexpr u32 N = 1000;
    for (u32 i = 0; i < N; ++i) {
        Task t {};
        t.fn = [](void* p, u32){
            static_cast<TAtomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        (void)FThreadPool::Submit(t);
    }
    FThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);

    FThreadPool::Shutdown();
}

ACS_TEST(Threading, ThreadPoolRepeatedShutdownReleasesWorkerStorage)
{
    // スレッド生成に伴う一度限りの遅延初期化を測定前に済ませる。
    EXPECT_TRUE(CycleThreadPool(4));
    (void)::HeapCompact(::GetProcessHeap(), 0);
    const usize before = ProcessHeapBusyBytes();
    EXPECT_TRUE(before != 0);

    EXPECT_TRUE(CycleThreadPool(24));
    (void)::HeapCompact(::GetProcessHeap(), 0);
    const usize after = ProcessHeapBusyBytes();
    EXPECT_TRUE(after != 0);

    // 修正前は約 4.5 MiB 増える。ヒープ管理の揺らぎだけを許容する。
    constexpr usize kAllowedGrowth = 1u << 20;
    EXPECT_TRUE(after <= before + kAllowedGrowth);
}

ACS_TEST(Threading, ThreadPoolShutdownDrainsOwnedPayloads)
{
    EXPECT_TRUE(FThreadPool::Init(1).IsOk());

    BlockingTaskContext blocked{};
    CompletionCounter completed;
    EXPECT_TRUE(FThreadPool::Submit(Task{&BlockingTask, &blocked, &completed}).IsOk());
    while (blocked.entered.Load(EMemoryOrder::Acquire) == 0)
        Yield();

    constexpr u32 kPayloadCount = 128;
    TAtomic<u32> destroyed{0};
    for (u32 i = 0; i < kPayloadCount; ++i) {
        auto* payload = new OwnedTaskPayload{&destroyed};
        auto result = FThreadPool::Submit(Task{&DeleteOwnedTask, payload, &completed});
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) delete payload;
    }

    DelayedReleaseContext release_context{&blocked};
    auto releaser = FThread::Spawn(&DelayedRelease, &release_context);
    EXPECT_TRUE(releaser.IsOk());

    // 修正前は running=0 でワーカーが即終了し、背後の payload と counter を残していた。
    FThreadPool::Shutdown();
    if (releaser.IsOk()) releaser.Value().Join();

    EXPECT_TRUE(completed.Finished());
    EXPECT_EQ(destroyed.Load(EMemoryOrder::Acquire), kPayloadCount);
}

ACS_TEST(Threading, ThreadPoolSubmitShutdownRaceIsLifetimeSafe)
{
    EXPECT_TRUE(FThreadPool::Init(4).IsOk());

    SubmitRaceContext context{};
    auto submitter = FThread::Spawn(&SubmitRaceMain, &context);
    EXPECT_TRUE(submitter.IsOk());
    SleepMs(20);

    FThreadPool::Shutdown();
    context.stop.Store(1, EMemoryOrder::Release);
    if (submitter.IsOk()) submitter.Value().Join();

    EXPECT_EQ(context.executed.Load(EMemoryOrder::Acquire), context.accepted.Load(EMemoryOrder::Acquire));
    EXPECT_EQ(FThreadPool::WorkerCount(), 0u);
}

ACS_TEST(Threading, WorkerShutdownAvoidsSelfJoin)
{
    EXPECT_TRUE(FThreadPool::Init(1).IsOk());

    TAtomic<u32> returned{0};
    CompletionCounter completed;
    EXPECT_TRUE(FThreadPool::Submit(Task{&ShutdownFromTask, &returned, &completed}).IsOk());
    FThreadPool::Wait(completed);

    EXPECT_EQ(returned.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(FThreadPool::WorkerCount(), 1u);
    FThreadPool::Shutdown();
}

ACS_TEST(Threading, ParallelForCovers) {
    auto rinit = FThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> seen{0};
    (void)FThreadPool::ParallelFor(0, 10000, 64,
        [](u32 /*i*/, u32 /*w*/, void* user){
            static_cast<TAtomic<u32>*>(user)->FetchAdd(1);
        }, &seen);
    EXPECT_EQ(seen.Load(), 10000u);
    FThreadPool::Shutdown();
}

// ノードプール経由で大量タスクを処理（Heap フォールバックが起きても破綻しない）
ACS_TEST(Threading, ThreadPoolHighLoad) {
    auto rinit = FThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> counter{0};
    CompletionCounter done;
    constexpr u32 N = 50000;
    for (u32 i = 0; i < N; ++i) {
        Task t {};
        t.fn = [](void* p, u32){
            static_cast<TAtomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        (void)FThreadPool::Submit(t);
    }
    FThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);
    FThreadPool::Shutdown();
}

// 入れ子 ParallelFor がデッドロックしないこと（help-stealing が効く）
ACS_TEST(Threading, NestedParallelFor) {
    auto rinit = FThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> total{0};
    (void)FThreadPool::ParallelFor(0, 10, 1,
        [](u32 /*i*/, u32 /*w*/, void* user){
            (void)FThreadPool::ParallelFor(0, 100, 16,
                [](u32 /*j*/, u32 /*w*/, void* u2){
                    static_cast<TAtomic<u32>*>(u2)->FetchAdd(1);
                }, user);
        }, &total);
    EXPECT_EQ(total.Load(), 1000u);
    FThreadPool::Shutdown();
}
