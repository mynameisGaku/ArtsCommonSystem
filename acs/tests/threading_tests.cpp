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
        auto result = CThreadPool::Init(2);
        if (result.IsErr()) return false;
        CThreadPool::Shutdown();
    }
    return true;
}

struct FBlockingTaskContext {
    TAtomic<u32> entered{0};
    TAtomic<u32> release{0};
};

void BlockingTask(void* user, u32) noexcept
{
    auto* context = static_cast<FBlockingTaskContext*>(user);
    context->entered.Store(1, EMemoryOrder::Release);
    while (context->release.Load(EMemoryOrder::Acquire) == 0)
        SleepMs(1);
}

struct FOwnedTaskPayload {
    TAtomic<u32>* destroyed = nullptr;
    ~FOwnedTaskPayload() noexcept
    {
        destroyed->FetchAdd(1);
    }
};

void DeleteOwnedTask(void* user, u32) noexcept
{
    delete static_cast<FOwnedTaskPayload*>(user);
}

struct FDelayedReleaseContext {
    FBlockingTaskContext* blocked = nullptr;
};

void DelayedRelease(void* user) noexcept
{
    auto* context = static_cast<FDelayedReleaseContext*>(user);
    SleepMs(20);
    context->blocked->release.Store(1, EMemoryOrder::Release);
}

struct FSubmitRaceContext {
    TAtomic<u32> stop{0};
    TAtomic<u32> accepted{0};
    TAtomic<u32> executed{0};
};

struct FAtomicPublicationContext
{
    static constexpr u32 kIterationCount = 50000u;

    u32 Payload = 0u;
    TAtomic<u32> Sequence{0u};
    TAtomic<u32> Acknowledged{0u};
    TAtomic<u32> FailureCount{0u};
    TAtomic<u32> StopRequested{0u};
};

void PublishAtomicPayload(void* User) noexcept
{
    auto* const Context = static_cast<FAtomicPublicationContext*>(User);
    for (u32 Iteration = 1u; Iteration <= FAtomicPublicationContext::kIterationCount; ++Iteration)
    {
        while (Context->Acknowledged.Load(EMemoryOrder::Acquire) != Iteration - 1u)
        {
            if (Context->StopRequested.Load(EMemoryOrder::Acquire) != 0u)
            {
                return;
            }
            Yield();
        }
        Context->Payload = Iteration ^ 0xA5A55A5Au;
        Context->Sequence.Store(Iteration, EMemoryOrder::Release);
    }
}

void ConsumeAtomicPayload(void* User) noexcept
{
    auto* const Context = static_cast<FAtomicPublicationContext*>(User);
    for (u32 Iteration = 1u; Iteration <= FAtomicPublicationContext::kIterationCount; ++Iteration)
    {
        while (Context->Sequence.Load(EMemoryOrder::Acquire) != Iteration)
        {
            if (Context->StopRequested.Load(EMemoryOrder::Acquire) != 0u)
            {
                return;
            }
            Yield();
        }
        if (Context->Payload != (Iteration ^ 0xA5A55A5Au))
        {
            Context->FailureCount.FetchAdd(1u);
        }
        Context->Acknowledged.Store(Iteration, EMemoryOrder::Release);
    }
}

void CountRaceTask(void* user, u32) noexcept
{
    static_cast<FSubmitRaceContext*>(user)->executed.FetchAdd(1);
}

void SubmitRaceMain(void* user) noexcept
{
    auto* context = static_cast<FSubmitRaceContext*>(user);
    while (context->stop.Load(EMemoryOrder::Acquire) == 0) {
        FTask task{&CountRaceTask, context, nullptr};
        if (CThreadPool::Submit(task).IsOk())
            context->accepted.FetchAdd(1);
        else
            Yield();
    }
}

void ShutdownFromTask(void* user, u32) noexcept
{
    CThreadPool::Shutdown();
    static_cast<TAtomic<u32>*>(user)->Store(1, EMemoryOrder::Release);
}

} // namespace

ACS_TEST(Threading, AtomicAddCounts) {
    TAtomic<u32> c{0};
    constexpr u32 N = 100;
    for (u32 i = 0; i < N; ++i) c.FetchAdd(1);
    EXPECT_EQ(c.Load(), N);
}

ACS_TEST(Threading, AtomicSupportedWidthsAndPointerOperations)
{
    TAtomic<u8> Value8{1u};
    TAtomic<u16> Value16{2u};
    TAtomic<u32> Value32{3u};
    TAtomic<u64> Value64{4u};

    Value8.Store(11u, EMemoryOrder::Release);
    Value16.Store(12u, EMemoryOrder::SeqCst);
    Value32.Store(13u, EMemoryOrder::Relaxed);
    Value64.Store(14u, EMemoryOrder::Release);
    EXPECT_EQ(Value8.Load(EMemoryOrder::Acquire), static_cast<u8>(11u));
    EXPECT_EQ(Value16.Load(EMemoryOrder::SeqCst), static_cast<u16>(12u));
    EXPECT_EQ(Value32.Load(EMemoryOrder::Relaxed), 13u);
    EXPECT_EQ(Value64.Load(EMemoryOrder::Acquire), 14ull);

    EXPECT_EQ(Value8.Exchange(21u), static_cast<u8>(11u));
    EXPECT_EQ(Value16.Exchange(22u), static_cast<u16>(12u));
    EXPECT_EQ(Value32.Exchange(23u), 13u);
    EXPECT_EQ(Value64.Exchange(24u), 14ull);

    u8 Expected8 = 21u;
    u16 Expected16 = 22u;
    u32 Expected32 = 23u;
    u64 Expected64 = 24u;
    EXPECT_TRUE(Value8.CompareExchange(Expected8, 31u));
    EXPECT_TRUE(Value16.CompareExchange(Expected16, 32u));
    EXPECT_TRUE(Value32.CompareExchange(Expected32, 33u));
    EXPECT_TRUE(Value64.CompareExchange(Expected64, 34u));

    u32 First = 41u;
    u32 Second = 42u;
    TAtomic<u32*> Pointer{&First};
    EXPECT_TRUE(Pointer.Load(EMemoryOrder::Acquire) == &First);
    EXPECT_TRUE(Pointer.Exchange(&Second) == &First);
    u32* ExpectedPointer = &Second;
    EXPECT_TRUE(Pointer.CompareExchange(ExpectedPointer, &First));
    Pointer.Store(&Second, EMemoryOrder::SeqCst);
    EXPECT_TRUE(Pointer.Load(EMemoryOrder::SeqCst) == &Second);
}

ACS_TEST(Threading, AtomicReleaseAcquirePublishesPayload)
{
    FAtomicPublicationContext Context{};
    auto Producer = FThread::Spawn(&PublishAtomicPayload, &Context);
    auto Consumer = FThread::Spawn(&ConsumeAtomicPayload, &Context);

    EXPECT_TRUE(Producer.IsOk());
    EXPECT_TRUE(Consumer.IsOk());
    if (Producer.IsErr() || Consumer.IsErr())
    {
        Context.StopRequested.Store(1u, EMemoryOrder::Release);
    }
    if (Producer.IsOk())
    {
        Producer.Value().Join();
    }
    if (Consumer.IsOk())
    {
        Consumer.Value().Join();
    }

    EXPECT_EQ(Context.FailureCount.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(Context.Acknowledged.Load(), FAtomicPublicationContext::kIterationCount);

    u32 Value = 41u;
    TAtomic<u32*> Pointer{nullptr};
    Pointer.Store(&Value);
    EXPECT_TRUE(Pointer.Load() == &Value);
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
    struct FCtx { TAtomic<u32> v{0}; };
    FCtx ctx;
    auto r = FThread::Spawn([](void* p){
        static_cast<FCtx*>(p)->v.Store(123);
    }, &ctx);
    EXPECT_TRUE(r.IsOk());
    if (r.IsOk()) {
        r.Value().Join();
        EXPECT_EQ(ctx.v.Load(), 123u);
    }
}

ACS_TEST(Threading, ThreadNameIsOwnedBySpawnContext)
{
    struct FNameCapture {
        wchar_t text[96] = {};
        u32 length = 0;
    } capture;

    wchar_t requested[96] = {};
    for (u32 i = 0; i < 95; ++i)
        requested[i] = static_cast<wchar_t>(L'A' + (i % 26));

    FThreadConfig config{};
    config.name = requested;
    auto result = FThread::Spawn(
        [](void* user) {
            auto* output = static_cast<FNameCapture*>(user);
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
    auto rinit = CThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());

    TAtomic<u32> counter{0};
    FCompletionCounter done;
    constexpr u32 N = 1000;
    for (u32 i = 0; i < N; ++i) {
        FTask t {};
        t.fn = [](void* p, u32){
            static_cast<TAtomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        (void)CThreadPool::Submit(t);
    }
    CThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);

    CThreadPool::Shutdown();
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
    EXPECT_TRUE(CThreadPool::Init(1).IsOk());

    FBlockingTaskContext blocked{};
    FCompletionCounter completed;
    EXPECT_TRUE(CThreadPool::Submit(FTask{&BlockingTask, &blocked, &completed}).IsOk());
    while (blocked.entered.Load(EMemoryOrder::Acquire) == 0)
        Yield();

    constexpr u32 kPayloadCount = 128;
    TAtomic<u32> destroyed{0};
    for (u32 i = 0; i < kPayloadCount; ++i) {
        auto* payload = new FOwnedTaskPayload{&destroyed};
        auto result = CThreadPool::Submit(FTask{&DeleteOwnedTask, payload, &completed});
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) delete payload;
    }

    FDelayedReleaseContext release_context{&blocked};
    auto releaser = FThread::Spawn(&DelayedRelease, &release_context);
    EXPECT_TRUE(releaser.IsOk());

    // 修正前は running=0 でワーカーが即終了し、背後の payload と counter を残していた。
    CThreadPool::Shutdown();
    if (releaser.IsOk()) releaser.Value().Join();

    EXPECT_TRUE(completed.Finished());
    EXPECT_EQ(destroyed.Load(EMemoryOrder::Acquire), kPayloadCount);
}

ACS_TEST(Threading, ThreadPoolSubmitShutdownRaceIsLifetimeSafe)
{
    EXPECT_TRUE(CThreadPool::Init(4).IsOk());

    FSubmitRaceContext context{};
    auto submitter = FThread::Spawn(&SubmitRaceMain, &context);
    EXPECT_TRUE(submitter.IsOk());
    SleepMs(20);

    CThreadPool::Shutdown();
    context.stop.Store(1, EMemoryOrder::Release);
    if (submitter.IsOk()) submitter.Value().Join();

    EXPECT_EQ(context.executed.Load(EMemoryOrder::Acquire), context.accepted.Load(EMemoryOrder::Acquire));
    EXPECT_EQ(CThreadPool::WorkerCount(), 0u);
}

ACS_TEST(Threading, WorkerShutdownAvoidsSelfJoin)
{
    EXPECT_TRUE(CThreadPool::Init(1).IsOk());

    TAtomic<u32> returned{0};
    FCompletionCounter completed;
    EXPECT_TRUE(CThreadPool::Submit(FTask{&ShutdownFromTask, &returned, &completed}).IsOk());
    CThreadPool::Wait(completed);

    EXPECT_EQ(returned.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(CThreadPool::WorkerCount(), 1u);
    CThreadPool::Shutdown();
}

ACS_TEST(Threading, ParallelForCovers) {
    auto rinit = CThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> seen{0};
    (void)CThreadPool::ParallelFor(0, 10000, 64,
        [](u32 /*i*/, u32 /*w*/, void* user){
            static_cast<TAtomic<u32>*>(user)->FetchAdd(1);
        }, &seen);
    EXPECT_EQ(seen.Load(), 10000u);
    CThreadPool::Shutdown();
}

// ノードプール経由で大量タスクを処理（Heap フォールバックが起きても破綻しない）
ACS_TEST(Threading, ThreadPoolHighLoad) {
    auto rinit = CThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> counter{0};
    FCompletionCounter done;
    constexpr u32 N = 50000;
    for (u32 i = 0; i < N; ++i) {
        FTask t {};
        t.fn = [](void* p, u32){
            static_cast<TAtomic<u32>*>(p)->FetchAdd(1);
        };
        t.user = &counter;
        t.counter = &done;
        (void)CThreadPool::Submit(t);
    }
    CThreadPool::Wait(done);
    EXPECT_EQ(counter.Load(), N);
    CThreadPool::Shutdown();
}

// 入れ子 ParallelFor がデッドロックしないこと（help-stealing が効く）
ACS_TEST(Threading, NestedParallelFor) {
    auto rinit = CThreadPool::Init(4);
    EXPECT_TRUE(rinit.IsOk());
    TAtomic<u32> total{0};
    (void)CThreadPool::ParallelFor(0, 10, 1,
        [](u32 /*i*/, u32 /*w*/, void* user){
            (void)CThreadPool::ParallelFor(0, 100, 16,
                [](u32 /*j*/, u32 /*w*/, void* u2){
                    static_cast<TAtomic<u32>*>(u2)->FetchAdd(1);
                }, user);
        }, &total);
    EXPECT_EQ(total.Load(), 1000u);
    CThreadPool::Shutdown();
}
