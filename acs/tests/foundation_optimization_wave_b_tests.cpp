// SPDX-License-Identifier: Apache-2.0
// Foundation Optimization Wave B の性能契約・寿命・並行性テスト
#include "test/Test.h"
#include "test/Expect.h"
#include "event/MessageBroker.h"
#include "event/MessagePipe.h"
#include "event/MessagePipePolicy.h"
#include "event/Timer.h"
#include "event/TimerDiagnostics.h"
#include "event/TimerSchedulePolicy.h"
#include "foundation/Move.h"
#include "foundation/Platform.h"
#include "platform/FileSystem.h"
#include "platform/FileExtensionKind.h"
#include "platform/FileSystemDiagnostics.h"
#include "platform/Storage.h"
#include "platform/Time.h"
#include "threading/Atomic.h"
#include "threading/JobGraph.h"
#include "threading/JobGraphDiagnostics.h"
#include "threading/Thread.h"
#include "threading/ThreadPool.h"
#include "threading/ThreadPoolDiagnostics.h"

#include <cstdio>

using namespace acs;

static_assert(kIsValidMessagePipeCapacity<2>);
static_assert(kIsValidMessagePipeCapacity<1024>);
static_assert(!kIsValidMessagePipeCapacity<0>);
static_assert(!kIsValidMessagePipeCapacity<7>);

static_assert(FFileSystem::ClassifyExtension("content/material.INI") == EFileExtensionKind::Ini);
static_assert(FFileSystem::ClassifyExtension(L"C:\\雲\\設定.AcPaK") == EFileExtensionKind::AssetPack);
static_assert(FFileSystem::ClassifyExtension("content/.ini") == EFileExtensionKind::Unknown);
static_assert(FFileSystem::ClassifyExtension("content/name.") == EFileExtensionKind::Unknown);
static_assert(FFileSystem::ClassifyExtension(L"content/設定.データ") == EFileExtensionKind::Unknown);

namespace {

struct FGateTaskContext {
    TAtomic<u32> release{0};
    TAtomic<u32> executed{0};
};

void RunGatedTask(void* user, u32) noexcept
{
    auto& context = *static_cast<FGateTaskContext*>(user);
    while (context.release.Load(EMemoryOrder::Acquire) == 0)
        Yield();
    context.executed.FetchAdd(1);
}

struct FPipeProducerContext {
    TMessagePipe<u32>* pipe = nullptr;
    u32 first = 0;
    u32 count = 0;
};

void ProduceMpmcValues(void* user) noexcept
{
    auto& context = *static_cast<FPipeProducerContext*>(user);
    for (u32 i = 0; i < context.count; ++i) {
        const u32 value = context.first + i;
        while (!context.pipe->Push(value)) {
            if (context.pipe->IsClosed()) return;
            Yield();
        }
    }
}

struct FPipeConsumerContext {
    TMessagePipe<u32>* pipe = nullptr;
    TAtomic<u32>* seen = nullptr;
    TAtomic<u32>* consumed = nullptr;
    u32 value_count = 0;
};

void ConsumeMpmcValues(void* user) noexcept
{
    auto& context = *static_cast<FPipeConsumerContext*>(user);
    u32 value = 0;
    while (context.pipe->Pop(value)) {
        if (value < context.value_count) context.seen[value].FetchAdd(1);
        context.consumed->FetchAdd(1);
    }
}

using FSpscPipe =
    TMessagePipe<u32, EMessagePipePolicy::Spsc, 1024>;

struct FSpscContext {
    FSpscPipe* pipe = nullptr;
    TAtomic<u32> producer_done{0};
    TAtomic<u32> failures{0};
    u32 count = 0;
};

void ProduceSpscValues(void* user) noexcept
{
    auto& context = *static_cast<FSpscContext*>(user);
    for (u32 i = 0; i < context.count; ++i) {
        while (!context.pipe->Push(i))
            Yield();
    }
    context.producer_done.Store(1, EMemoryOrder::Release);
}

void ConsumeSpscValues(void* user) noexcept
{
    auto& context = *static_cast<FSpscContext*>(user);
    u32 expected = 0;
    while (expected < context.count) {
        u32 value = 0;
        if (context.pipe->TryPop(value)) {
            if (value != expected) context.failures.FetchAdd(1);
            ++expected;
        } else {
            Yield();
        }
    }
}

struct FLargeGraphCallable {
    TAtomic<u32>* executed = nullptr;
    u32* destroyed = nullptr;
    bool owns_lifetime = true;
    u8 padding[160]{};

    FLargeGraphCallable(TAtomic<u32>* execution_count, u32* destruction_count) noexcept
        : executed(execution_count), destroyed(destruction_count)
    {
    }

    FLargeGraphCallable(const FLargeGraphCallable&) = delete;
    FLargeGraphCallable& operator=(const FLargeGraphCallable&) = delete;

    FLargeGraphCallable(FLargeGraphCallable&& other) noexcept
        : executed(other.executed),
          destroyed(other.destroyed),
          owns_lifetime(other.owns_lifetime)
    {
        other.owns_lifetime = false;
    }

    ~FLargeGraphCallable() noexcept
    {
        if (owns_lifetime && destroyed) ++*destroyed;
    }

    void operator()(u32) noexcept
    {
        executed->FetchAdd(1);
    }
};

/** move 構築中の Shutdown 要求が自己待機せず、後続タスクを維持することを検証する callable。 */
struct FShutdownDuringMoveCallable {
    /** 実行回数の書き込み先。 */
    TAtomic<u32>* executed = nullptr;

    /** 実行回数の書き込み先を設定する。 */
    explicit FShutdownDuringMoveCallable(TAtomic<u32>* in_executed) noexcept
        : executed(in_executed)
    {
    }

    /** 所有権移動中に終了要求を発行し、構築中ガードを検証する。 */
    FShutdownDuringMoveCallable(FShutdownDuringMoveCallable&& other) noexcept
        : executed(other.executed)
    {
        other.executed = nullptr;
        FThreadPool::Shutdown();
    }

    /** 複製を禁止する。 */
    FShutdownDuringMoveCallable(const FShutdownDuringMoveCallable&) = delete;

    /** 複製代入を禁止する。 */
    FShutdownDuringMoveCallable& operator=(const FShutdownDuringMoveCallable&) = delete;

    /** 移動代入を禁止する。 */
    FShutdownDuringMoveCallable& operator=(FShutdownDuringMoveCallable&&) = delete;

    /** タスク実行を記録する。 */
    void operator()() noexcept
    {
        if (executed) executed->FetchAdd(1);
    }
};

struct FTypedMessage {
    i32 value = 0;
};

struct FTypedBrokerContext {
    FMessageBroker* broker = nullptr;
    FSubscriptionHandle cancel_on_first{};
    i32 typed_calls = 0;
    i32 raw_calls = 0;
    bool nested = false;
};

void OnTypedMessage(const FTypedMessage& message, void* user) noexcept
{
    auto& context = *static_cast<FTypedBrokerContext*>(user);
    ++context.typed_calls;
    if (message.value == 1 && !context.nested) {
        context.nested = true;
        context.broker->Unsubscribe(context.cancel_on_first);
        context.broker->Publish(FTypedMessage{2});
    }
}

void OnRawMessage(const void*, void* user) noexcept
{
    ++static_cast<FTypedBrokerContext*>(user)->raw_calls;
}

/** Publish 中の購読追加が次回配送まで遅延することを記録する状態。 */
struct FSubscribeDuringPublishContext {
    /** 検証対象の broker。 */
    FMessageBroker* broker = nullptr;

    /** 発行開始時から存在する購読者の呼び出し回数。 */
    i32 initial_calls = 0;

    /** 発行中に追加した購読者の呼び出し回数。 */
    i32 added_calls = 0;

    /** 発行中の購読追加を一度だけ行ったか。 */
    bool subscribed = false;
};

/** 発行中に追加される購読者の呼び出しを記録する。 */
void OnAddedDuringPublish(const FTypedMessage&, void* user) noexcept
{
    ++static_cast<FSubscribeDuringPublishContext*>(user)->added_calls;
}

/** 最初の配送で新しい購読者を追加する。 */
void OnSubscribeDuringPublish(const FTypedMessage&, void* user) noexcept
{
    auto& context = *static_cast<FSubscribeDuringPublishContext*>(user);
    ++context.initial_calls;
    if (context.subscribed) return;
    context.subscribed = true;
    context.broker->SubscribeTyped<FTypedMessage, &OnAddedDuringPublish>(&context);
}

struct FTimerCounter {
    u32 hits = 0;
};

void CountTimer(void* user) noexcept
{
    ++static_cast<FTimerCounter*>(user)->hits;
}

void CountTypedTimer(FTimerCounter* user) noexcept
{
    ++user->hits;
}

bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return left == right;
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

} // namespace

ACS_TEST(FoundationOptimizationWaveB, ThreadPoolBatchesAndAvoidsRedundantWakeups)
{
    FThreadPool::Shutdown();
    EXPECT_TRUE(FThreadPool::Init(4).IsOk());
    // 全 worker を park まで進め、最初の投入で実通知経路を必ず通す。
    SleepMs(20);
    FThreadPool::ResetDiagnostics();

    constexpr u32 kTaskCount = 512;
    FGateTaskContext context;
    FCompletionCounter completed;
    for (u32 i = 0; i < kTaskCount; ++i) {
        EXPECT_TRUE(FThreadPool::Submit(FTask{&RunGatedTask, &context, &completed}).IsOk());
    }
    context.release.Store(1, EMemoryOrder::Release);

    for (u32 wait = 0; wait < 10000 && context.executed.Load(EMemoryOrder::Acquire) != kTaskCount; ++wait) {
        SleepMs(1);
    }
    FThreadPool::Wait(completed);

    const FThreadPoolDiagnostics diagnostics =
        FThreadPool::Diagnostics();
    EXPECT_EQ(context.executed.Load(EMemoryOrder::Acquire), kTaskCount);
    EXPECT_EQ(diagnostics.external_tasks_drained, u64{kTaskCount});
    EXPECT_TRUE(diagnostics.submission_drain_lock_acquisitions < kTaskCount);
    EXPECT_TRUE(diagnostics.wake_one_calls > 0);
    EXPECT_TRUE(diagnostics.wake_one_calls <= 4);
    EXPECT_EQ(diagnostics.submission_heap_fallbacks, 0ull);
    EXPECT_EQ(diagnostics.queued_work, 0u);

    TAtomic<u32> callable_executed{0};
    u32 large_callable_destructions = 0;
    FCompletionCounter callable_completed;
    FThreadPool::ResetDiagnostics();
    EXPECT_TRUE(FThreadPool::SubmitCallable([&callable_executed](u32) noexcept { callable_executed.FetchAdd(1); }, &callable_completed).IsOk());
    EXPECT_TRUE(FThreadPool::SubmitCallable(FLargeGraphCallable{&callable_executed, &large_callable_destructions}, &callable_completed).IsOk());
    FThreadPool::Wait(callable_completed);
    const FThreadPoolDiagnostics callable_diagnostics =
        FThreadPool::Diagnostics();
    EXPECT_EQ(callable_executed.Load(EMemoryOrder::Acquire), 2u);
    EXPECT_EQ(large_callable_destructions, 1u);
    EXPECT_EQ(callable_diagnostics.callable_inline_submissions, 1ull);
    EXPECT_EQ(callable_diagnostics.callable_heap_submissions, 1ull);
    EXPECT_EQ(callable_diagnostics.callable_node_heap_fallbacks, 0ull);

    TAtomic<u32> shutdown_during_move_executed{0};
    FCompletionCounter shutdown_during_move_completed;
    EXPECT_TRUE(FThreadPool::SubmitCallable(FShutdownDuringMoveCallable{&shutdown_during_move_executed}, &shutdown_during_move_completed).IsOk());
    FThreadPool::Wait(shutdown_during_move_completed);
    EXPECT_EQ(shutdown_during_move_executed.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(FThreadPool::WorkerCount(), 4u);

    std::printf("wave_b_threadpool tasks=%u drain_locks=%llu wakes=%llu contentions=%llu heap_fallbacks=%llu inline_callables=%llu heap_callables=%llu\n", kTaskCount, static_cast<unsigned long long>(diagnostics.submission_drain_lock_acquisitions), static_cast<unsigned long long>(diagnostics.wake_one_calls), static_cast<unsigned long long>(diagnostics.submission_lock_contentions), static_cast<unsigned long long>(diagnostics.submission_heap_fallbacks), static_cast<unsigned long long>(callable_diagnostics.callable_inline_submissions), static_cast<unsigned long long>(callable_diagnostics.callable_heap_submissions));
    FThreadPool::Shutdown();
}

ACS_TEST(FoundationOptimizationWaveB, JobGraphReusesTopologyAndOwnsCallables)
{
    FThreadPool::Shutdown();
    EXPECT_TRUE(FThreadPool::Init(4).IsOk());

    constexpr u32 kJobCount = 40;
    constexpr u32 kRunCount = 3;
    TAtomic<u32> executed{0};
    u32 large_callable_destructions = 0;
    {
        FJobGraph graph;
        FJobHandle handles[kJobCount]{};
        for (u32 i = 0; i + 1 < kJobCount; ++i) {
            handles[i] = graph.AddCallable([&executed](u32) noexcept { executed.FetchAdd(1); });
            EXPECT_TRUE(handles[i].IsValid());
        }
        handles[kJobCount - 1] = graph.AddCallable(FLargeGraphCallable{&executed, &large_callable_destructions});
        EXPECT_TRUE(handles[kJobCount - 1].IsValid());

        for (u32 i = 1; i < kJobCount; ++i)
            handles[i].DependOn(handles[i - 1]);

        for (u32 run = 0; run < kRunCount; ++run) {
            if (run != 0) graph.Reset();
            EXPECT_TRUE(graph.Submit().IsOk());
            graph.Wait();
        }

        const FJobGraphDiagnostics diagnostics =
            graph.Diagnostics();
        EXPECT_EQ(executed.Load(EMemoryOrder::Acquire), kJobCount * kRunCount);
        EXPECT_EQ(diagnostics.topology_compilations, 1ull);
        EXPECT_EQ(diagnostics.submit_full_graph_scans, 1ull);
        EXPECT_EQ(diagnostics.reset_job_visits, u64{kJobCount} * (kRunCount - 1));
        EXPECT_EQ(diagnostics.inline_job_count, 32u);
        EXPECT_EQ(diagnostics.heap_job_count, kJobCount - 32u);
        EXPECT_EQ(diagnostics.inline_callable_count, kJobCount - 1u);
        EXPECT_EQ(diagnostics.heap_callable_count, 1u);
        EXPECT_EQ(large_callable_destructions, 0u);

        std::printf("wave_b_jobgraph runs=%u jobs=%u topology_builds=%llu submit_scans=%llu inline_jobs=%u heap_jobs=%u\n", kRunCount, kJobCount, static_cast<unsigned long long>(diagnostics.topology_compilations), static_cast<unsigned long long>(diagnostics.submit_full_graph_scans), diagnostics.inline_job_count, diagnostics.heap_job_count);
    }
    EXPECT_EQ(large_callable_destructions, 1u);
    FThreadPool::Shutdown();
}

ACS_TEST(FoundationOptimizationWaveB, MessagePipeBatchingIsBoundedAndFifo)
{
    TMessagePipe<i32> pipe(8);
    i32 input[8]{0, 1, 2, 3, 4, 5, 6, 7};
    EXPECT_EQ(pipe.PushBatch(input, 8), usize{8});
    EXPECT_FALSE(pipe.Push(8));

    i32 first_batch[3]{};
    EXPECT_EQ(pipe.TryPopBatch(first_batch, 3), usize{3});
    EXPECT_EQ(first_batch[0], 0);
    EXPECT_EQ(first_batch[1], 1);
    EXPECT_EQ(first_batch[2], 2);

    i32 second_input[3]{8, 9, 10};
    EXPECT_EQ(pipe.PushBatch(second_input, 3), usize{3});
    i32 output[8]{};
    EXPECT_EQ(pipe.TryPopBatch(output, 8), usize{8});
    for (i32 i = 0; i < 8; ++i)
        EXPECT_EQ(output[i], i + 3);
    EXPECT_EQ(pipe.Size(), usize{0});
}

ACS_TEST(FoundationOptimizationWaveB, MessagePipeMpmcStressIsExactlyOnce)
{
    constexpr u32 kProducerCount = 2;
    constexpr u32 kConsumerCount = 2;
    constexpr u32 kValuesPerProducer = 2000;
    constexpr u32 kValueCount =
        kProducerCount * kValuesPerProducer;

    TMessagePipe<u32> pipe(256);
    TAtomic<u32> seen[kValueCount]{};
    TAtomic<u32> consumed{0};
    FPipeConsumerContext consumer_context{
        &pipe, seen, &consumed, kValueCount};
    FPipeProducerContext producer_contexts[kProducerCount]{
        {&pipe, 0, kValuesPerProducer},
        {&pipe, kValuesPerProducer, kValuesPerProducer},
    };

    FThread consumers[kConsumerCount];
    for (u32 i = 0; i < kConsumerCount; ++i) {
        auto result =
            FThread::Spawn(&ConsumeMpmcValues, &consumer_context);
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) {
            pipe.Close();
            return;
        }
        consumers[i] = Move(result.Value());
    }

    FThread producers[kProducerCount];
    for (u32 i = 0; i < kProducerCount; ++i) {
        auto result = FThread::Spawn(&ProduceMpmcValues, &producer_contexts[i]);
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) {
            pipe.Close();
            return;
        }
        producers[i] = Move(result.Value());
    }

    for (u32 i = 0; i < kProducerCount; ++i)
        producers[i].Join();
    pipe.Close();
    for (u32 i = 0; i < kConsumerCount; ++i)
        consumers[i].Join();

    EXPECT_EQ(consumed.Load(EMemoryOrder::Acquire), kValueCount);
    for (u32 i = 0; i < kValueCount; ++i)
        EXPECT_EQ(seen[i].Load(EMemoryOrder::Acquire), 1u);
}

ACS_TEST(FoundationOptimizationWaveB, MessagePipeSpscSpecializationPreservesOrder)
{
    constexpr u32 kValueCount = 100000;
    FSpscPipe pipe;
    FSpscContext context;
    context.pipe = &pipe;
    context.count = kValueCount;

    auto consumer_result =
        FThread::Spawn(&ConsumeSpscValues, &context);
    auto producer_result =
        FThread::Spawn(&ProduceSpscValues, &context);
    EXPECT_TRUE(consumer_result.IsOk());
    EXPECT_TRUE(producer_result.IsOk());
    if (consumer_result.IsErr() || producer_result.IsErr()) {
        pipe.Close();
        return;
    }

    FThread consumer = Move(consumer_result.Value());
    FThread producer = Move(producer_result.Value());
    producer.Join();
    consumer.Join();
    pipe.Close();

    EXPECT_EQ(context.failures.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(pipe.Size(), usize{0});
}

ACS_TEST(FoundationOptimizationWaveB, TypedBrokerPreservesNestedPublishCancellation)
{
    FMessageBroker broker;
    FTypedBrokerContext context;
    context.broker = &broker;
    const FSubscriptionHandle typed =
        broker.SubscribeTyped<FTypedMessage, &OnTypedMessage>(&context);
    context.cancel_on_first =
        broker.Subscribe<FTypedMessage>(&OnRawMessage, &context);
    EXPECT_TRUE(typed.IsValid());
    EXPECT_TRUE(context.cancel_on_first.IsValid());

    broker.Publish(FTypedMessage{1});
    EXPECT_EQ(context.typed_calls, 2);
    EXPECT_EQ(context.raw_calls, 0);
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FTypedMessage>()), 1u);
    EXPECT_TRUE(broker.Unsubscribe(typed));
}

ACS_TEST(FoundationOptimizationWaveB, BrokerDefersSubscriptionAddedDuringPublish)
{
    FMessageBroker broker;
    FSubscribeDuringPublishContext context;
    context.broker = &broker;

    const FSubscriptionHandle initial =
        broker.SubscribeTyped<FTypedMessage, &OnSubscribeDuringPublish>(&context);
    const FSubscriptionHandle reusable =
        broker.SubscribeTyped<FTypedMessage, &OnAddedDuringPublish>(&context);
    EXPECT_TRUE(initial.IsValid());
    EXPECT_TRUE(reusable.IsValid());
    EXPECT_TRUE(broker.Unsubscribe(reusable));

    broker.Publish(FTypedMessage{1});
    EXPECT_EQ(context.initial_calls, 1);
    EXPECT_EQ(context.added_calls, 0);
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FTypedMessage>()), 2u);

    broker.Publish(FTypedMessage{2});
    EXPECT_EQ(context.initial_calls, 2);
    EXPECT_EQ(context.added_calls, 1);
}

ACS_TEST(FoundationOptimizationWaveB, TimerUsesDirectCancelAndActiveBitset)
{
    constexpr u32 kTimerCount = 8192;
    FTimerManager timers;
    FTimerCounter counter;
    FTimerHandle handles[kTimerCount]{};
    for (u32 i = 0; i < kTimerCount; ++i)
        handles[i] = timers.SetTimeout(10.0f, &CountTimer, &counter);

    timers.ResetDiagnostics();
    const u64 cancel_start = FClock::Ticks();
    for (u32 i = 0; i + 1 < kTimerCount; ++i)
        EXPECT_TRUE(timers.Cancel(handles[i]));
    const u64 cancel_end = FClock::Ticks();
    const FTimerDiagnostics cancel_diagnostics =
        timers.Diagnostics();
    EXPECT_EQ(cancel_diagnostics.cancel_slot_probes, u64{kTimerCount - 1});
    EXPECT_EQ(timers.ActiveCount(), 1u);

    timers.ResetDiagnostics();
    timers.Tick(0.0f);
    const FTimerDiagnostics tick_diagnostics =
        timers.Diagnostics();
    EXPECT_EQ(tick_diagnostics.active_slots_visited, 1ull);
    EXPECT_EQ(tick_diagnostics.active_words_loaded, u64{kTimerCount / 64});
    EXPECT_EQ(counter.hits, 0u);
    EXPECT_TRUE(timers.Cancel(handles[kTimerCount - 1]));

    const FTimerHandle typed =
        timers.Schedule<
            ETimerSchedulePolicy::Once,
            &CountTypedTimer>(0.0f, &counter);
    EXPECT_TRUE(typed.IsValid());
    timers.Tick(0.0f);
    EXPECT_EQ(counter.hits, 1u);

    const f64 cancel_ms =
        static_cast<f64>(cancel_end - cancel_start) * 1000.0 /
        static_cast<f64>(FClock::TicksPerSecond());
    std::printf("wave_b_timer timers=%u cancel_probes=%llu tick_active_visits=%llu tick_words=%llu cancel_ms=%.3f\n", kTimerCount, static_cast<unsigned long long>(cancel_diagnostics.cancel_slot_probes), static_cast<unsigned long long>(tick_diagnostics.active_slots_visited), static_cast<unsigned long long>(tick_diagnostics.active_words_loaded), cancel_ms);
}

ACS_TEST(FoundationOptimizationWaveB, FileIoReadsTextWithoutIntermediateCopy)
{
    constexpr const wchar_t* kPath =
        L"acs_foundation_optimization_wave_b.txt";
    constexpr const wchar_t* kStoragePath =
        L"acs_foundation_optimization_wave_b.ini";
    constexpr const wchar_t* kParentFilePath =
        L"acs_foundation_optimization_wave_b_parent";
    constexpr const wchar_t* kBlockedChildPath =
        L"acs_foundation_optimization_wave_b_parent\\child";
    constexpr const char* kText =
        "ACS Foundation Optimization Wave B\n";
    (void)FFileSystem::Delete(kPath);
    (void)FFileSystem::Delete(kStoragePath);
    (void)FFileSystem::Delete(kParentFilePath);
    EXPECT_TRUE(FFileSystem::WriteAllText(kPath, kText).IsOk());

    FFileSystem::ResetDiagnostics();
    auto text_result = FFileSystem::ReadAllText(kPath);
    EXPECT_TRUE(text_result.IsOk());
    if (text_result.IsOk()) {
        EXPECT_TRUE(TextEquals(text_result.Value().Data(), kText));
    }
    const FFileSystemDiagnostics diagnostics =
        FFileSystem::Diagnostics();
    EXPECT_EQ(diagnostics.read_syscalls, 1ull);
    EXPECT_EQ(diagnostics.text_intermediate_copy_bytes, 0ull);
    EXPECT_TRUE(FFileSystem::WriteAllBytes(kPath, nullptr, 1).IsErr());
    constexpr byte kOneByte[]{0};
    EXPECT_TRUE(FFileSystem::WriteAllBytes(kPath, kOneByte, static_cast<usize>(0xffffffffull) + 1u).IsErr());

    FStorage storage;
    storage.SetInt("extension-compatibility", 7);
    /** 拡張子で従来の INI 内容保存を拒否しないことを確認するパス。 */
    constexpr const wchar_t* kCompatibleStoragePaths[]{L"acs_foundation_optimization_wave_b.json", L"acs_foundation_optimization_wave_b.bin", L"acs_foundation_optimization_wave_b.acpak"};
    for (const wchar_t* compatible_path : kCompatibleStoragePaths) {
        (void)FFileSystem::Delete(compatible_path);
        EXPECT_TRUE(storage.Save(compatible_path).IsOk());
        /** 拡張子に依存せず保存内容を復元する確認先。 */
        FStorage compatible_loaded;
        EXPECT_TRUE(compatible_loaded.Load(compatible_path).IsOk());
        EXPECT_EQ(compatible_loaded.GetInt("extension-compatibility"), 7ll);
        EXPECT_TRUE(FFileSystem::Delete(compatible_path).IsOk());
    }

    // 公開先を共有禁止で保持し、置換失敗時にも旧内容が残ることを確認する。
    EXPECT_TRUE(FFileSystem::WriteAllText(kStoragePath, "stable-before-failed-replace").IsOk());
    const HANDLE locked_file = ::CreateFileW(kStoragePath, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(locked_file != INVALID_HANDLE_VALUE);
    constexpr byte kReplacement[]{'n', 'e', 'w'};
    EXPECT_TRUE(FFileSystem::WriteAllBytesAtomic(kStoragePath, kReplacement, sizeof(kReplacement)).IsErr());
    if (locked_file != INVALID_HANDLE_VALUE) ::CloseHandle(locked_file);
    auto preserved_text = FFileSystem::ReadAllText(kStoragePath);
    EXPECT_TRUE(preserved_text.IsOk());
    if (preserved_text.IsOk()) {
        EXPECT_TRUE(TextEquals(preserved_text.Value().Data(), "stable-before-failed-replace"));
    }

    storage.SetInt("generation", 42);
    auto save_result = storage.Save(kStoragePath);
    if (save_result.IsErr()) {
        std::printf("wave_b_atomic_save_error message=%s os_error=%u\n", save_result.Error().message, save_result.Error().os_error);
    }
    EXPECT_TRUE(save_result.IsOk());
    FStorage loaded;
    EXPECT_TRUE(loaded.Load(kStoragePath).IsOk());
    EXPECT_EQ(loaded.GetInt("generation"), 42ll);

    EXPECT_TRUE(FFileSystem::ReadAllBytes(nullptr).IsErr());
    EXPECT_TRUE(FFileSystem::CreateDirectory(nullptr).IsErr());
    EXPECT_TRUE(FFileSystem::WriteAllBytesAtomic(L"", nullptr, 0).IsErr());
    EXPECT_TRUE(storage.Load(L"").IsErr());
    EXPECT_TRUE(storage.Save(L"").IsErr());
    EXPECT_TRUE(FFileSystem::WriteAllText(kParentFilePath, "parent-is-a-file").IsOk());
    EXPECT_TRUE(FFileSystem::CreateDirectory(kBlockedChildPath).IsErr());
    EXPECT_TRUE(FFileSystem::Delete(kPath).IsOk());
    EXPECT_TRUE(FFileSystem::Delete(kStoragePath).IsOk());
    EXPECT_TRUE(FFileSystem::Delete(kParentFilePath).IsOk());

    std::printf("wave_b_file_io read_syscalls=%llu text_intermediate_copy_bytes=%llu\n", static_cast<unsigned long long>(diagnostics.read_syscalls), static_cast<unsigned long long>(diagnostics.text_intermediate_copy_bytes));
}
