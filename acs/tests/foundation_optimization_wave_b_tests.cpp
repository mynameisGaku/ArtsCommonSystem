// SPDX-License-Identifier: Apache-2.0
// ThreadPool、JobGraph、MessagePipe、Timer、File I/O の性能・寿命・並行性契約を検証する。
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

static_assert(CFileSystem::ClassifyExtension("content/material.INI") == EFileExtensionKind::Ini);
static_assert(CFileSystem::ClassifyExtension(L"C:\\雲\\設定.AcPaK") == EFileExtensionKind::AssetPack);
static_assert(CFileSystem::ClassifyExtension("content/.ini") == EFileExtensionKind::Unknown);
static_assert(CFileSystem::ClassifyExtension("content/name.") == EFileExtensionKind::Unknown);
static_assert(CFileSystem::ClassifyExtension(L"content/設定.データ") == EFileExtensionKind::Unknown);
static_assert(CFileSystem::IsAscii(static_cast<signed char>(0x7f)));
static_assert(!CFileSystem::IsAscii(static_cast<signed char>(-1)));
static_assert(!CFileSystem::IsAscii(static_cast<wchar_t>(0x80)));

namespace {

/** worker を待機させて一括解放するテスト状態。 */
struct FGateTaskContext {
    /** worker の待機を解除するフラグ。 */
    TAtomic<u32> release{0};
    /** 実行を完了したタスク数。 */
    TAtomic<u32> executed{0};
};

/**
 * 解放フラグを待って実行回数を記録する。
 *
 * @param user FGateTaskContext のアドレス。
 * @param worker_index 実行 worker 番号。検証では使用しない。
 */
void RunGatedTask(void* user, u32 worker_index) noexcept
{
    (void)worker_index;
    /** 呼び出し側が共有するテスト状態。 */
    auto& context = *static_cast<FGateTaskContext*>(user);
    while (context.release.Load(EMemoryOrder::Acquire) == 0)
        Yield();
    context.executed.FetchAdd(1);
}

/** MPMC producer が共有する入力範囲。 */
struct FPipeProducerContext {
    /** 値を追加するパイプ。 */
    TMessagePipe<u32>* pipe = nullptr;
    /** 追加する最初の値。 */
    u32 first = 0;
    /** 追加する値の個数。 */
    u32 count = 0;
};

/**
 * 指定範囲の値を MPMC パイプへ追加する。
 *
 * @param user FPipeProducerContext のアドレス。
 */
void ProduceMpmcValues(void* user) noexcept
{
    /** producer が共有する入力範囲。 */
    auto& context = *static_cast<FPipeProducerContext*>(user);
    /** 今回追加する範囲内位置。 */
    for (u32 i = 0; i < context.count; ++i) {
        /** パイプへ追加する一意な値。 */
        const u32 value = context.first + i;
        while (!context.pipe->Push(value)) {
            if (context.pipe->IsClosed()) return;
            Yield();
        }
    }
}

/** MPMC consumer が共有する検証状態。 */
struct FPipeConsumerContext {
    /** 値を取り出すパイプ。 */
    TMessagePipe<u32>* pipe = nullptr;
    /** 値ごとの観測回数。 */
    TAtomic<u32>* seen = nullptr;
    /** 全 consumer の取り出し総数。 */
    TAtomic<u32>* consumed = nullptr;
    /** 有効な値の範囲。 */
    u32 value_count = 0;
};

/**
 * MPMC パイプを閉じるまで値を取り出して観測回数を記録する。
 *
 * @param user FPipeConsumerContext のアドレス。
 */
void ConsumeMpmcValues(void* user) noexcept
{
    /** consumer が共有する検証状態。 */
    auto& context = *static_cast<FPipeConsumerContext*>(user);
    /** 直近に取り出した値。 */
    u32 value = 0;
    while (context.pipe->Pop(value)) {
        if (value < context.value_count) context.seen[value].FetchAdd(1);
        context.consumed->FetchAdd(1);
    }
}

/** 順序保証を検証する固定容量 SPSC パイプ。 */
using FSpscPipe = TMessagePipe<u32, EMessagePipePolicy::Spsc, 1024>;

/** SPSC producer と consumer が共有する状態。 */
struct FSpscContext {
    /** 検証対象の SPSC パイプ。 */
    FSpscPipe* pipe = nullptr;
    /** producer 完了フラグ。 */
    TAtomic<u32> producer_done{0};
    /** 順序不一致の検出数。 */
    TAtomic<u32> failures{0};
    /** 送受信する値の個数。 */
    u32 count = 0;
};

/**
 * 0 から count 未満の値を SPSC パイプへ追加する。
 *
 * @param user FSpscContext のアドレス。
 */
void ProduceSpscValues(void* user) noexcept
{
    /** producer と consumer が共有する状態。 */
    auto& context = *static_cast<FSpscContext*>(user);
    /** 今回追加する連番値。 */
    for (u32 i = 0; i < context.count; ++i) {
        while (!context.pipe->Push(i))
            Yield();
    }
    context.producer_done.Store(1, EMemoryOrder::Release);
}

/**
 * SPSC パイプから値を取り出して順序を検証する。
 *
 * @param user FSpscContext のアドレス。
 */
void ConsumeSpscValues(void* user) noexcept
{
    /** producer と consumer が共有する状態。 */
    auto& context = *static_cast<FSpscContext*>(user);
    /** 次に期待する連番値。 */
    u32 expected = 0;
    while (expected < context.count) {
        /** 直近に取り出した値。 */
        u32 value = 0;
        if (context.pipe->TryPop(value)) {
            if (value != expected) context.failures.FetchAdd(1);
            ++expected;
        } else {
            Yield();
        }
    }
}

/** inline 容量を超える JobGraph callable の寿命検証型。 */
struct FLargeGraphCallable {
    /** 実行回数の書き込み先。 */
    TAtomic<u32>* executed = nullptr;
    /** 破棄回数の書き込み先。 */
    u32* destroyed = nullptr;
    /** 破棄回数を記録する所有権。 */
    bool owns_lifetime = true;
    /** callable を inline 容量より大きくする詰め物。 */
    u8 padding[160]{};

    /**
     * 実行回数と破棄回数の書き込み先を設定する。
     *
     * @param execution_count 実行回数の書き込み先。
     * @param destruction_count 破棄回数の書き込み先。
     */
    FLargeGraphCallable(TAtomic<u32>* execution_count, u32* destruction_count) noexcept
        : executed(execution_count), destroyed(destruction_count)
    {
    }

    /** 複製を禁止する。 */
    FLargeGraphCallable(const FLargeGraphCallable&) = delete;
    /** 複製代入を禁止する。 */
    FLargeGraphCallable& operator=(const FLargeGraphCallable&) = delete;

    /** 寿命記録の所有権を移動する。 */
    FLargeGraphCallable(FLargeGraphCallable&& other) noexcept
        : executed(other.executed), destroyed(other.destroyed), owns_lifetime(other.owns_lifetime)
    {
        other.owns_lifetime = false;
    }

    /** 所有中なら破棄回数を記録する。 */
    ~FLargeGraphCallable() noexcept
    {
        if (owns_lifetime && destroyed) ++*destroyed;
    }

    /** JobGraph からの実行を記録する。 */
    void operator()(u32 worker_index) noexcept
    {
        (void)worker_index;
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
        CThreadPool::Shutdown();
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

/** 型付き broker 配送に使うメッセージ。 */
struct FTypedMessage {
    /** 入れ子配送を制御する値。 */
    i32 value = 0;
};

/** 型付き配送と購読解除の検証状態。 */
struct FTypedBrokerContext {
    /** 検証対象の broker。 */
    CMessageBroker* broker = nullptr;
    /** 最初の配送で解除する購読。 */
    FSubscriptionHandle cancel_on_first{};
    /** 型付き購読者の呼び出し回数。 */
    i32 typed_calls = 0;
    /** 生ポインタ購読者の呼び出し回数。 */
    i32 raw_calls = 0;
    /** 入れ子配送を開始済みなら true。 */
    bool nested = false;
};

/**
 * 型付き配送を記録し、最初の値で購読解除と入れ子配送を行う。
 *
 * @param message 配送されたメッセージ。
 * @param user FTypedBrokerContext のアドレス。
 */
void OnTypedMessage(const FTypedMessage& message, void* user) noexcept
{
    /** broker と呼び出し回数を共有する状態。 */
    auto& context = *static_cast<FTypedBrokerContext*>(user);
    ++context.typed_calls;
    if (message.value == 1 && !context.nested) {
        context.nested = true;
        context.broker->Unsubscribe(context.cancel_on_first);
        context.broker->Publish(FTypedMessage{2});
    }
}

/**
 * 生ポインタ購読者の呼び出しを記録する。
 *
 * @param message 配送されたメッセージ。検証では使用しない。
 * @param user FTypedBrokerContext のアドレス。
 */
void OnRawMessage(const void* message, void* user) noexcept
{
    (void)message;
    ++static_cast<FTypedBrokerContext*>(user)->raw_calls;
}

/** Publish 中の購読追加が次回配送まで遅延することを記録する状態。 */
struct FSubscribeDuringPublishContext {
    /** 検証対象の broker。 */
    CMessageBroker* broker = nullptr;

    /** 発行開始時から存在する購読者の呼び出し回数。 */
    i32 initial_calls = 0;

    /** 発行中に追加した購読者の呼び出し回数。 */
    i32 added_calls = 0;

    /** 発行中の購読追加を一度だけ行ったか。 */
    bool subscribed = false;
};

/**
 * 発行中に追加される購読者の呼び出しを記録する。
 *
 * @param message 配送されたメッセージ。検証では使用しない。
 * @param user FSubscribeDuringPublishContext のアドレス。
 */
void OnAddedDuringPublish(const FTypedMessage& message, void* user) noexcept
{
    (void)message;
    ++static_cast<FSubscribeDuringPublishContext*>(user)->added_calls;
}

/**
 * 最初の配送で新しい購読者を追加する。
 *
 * @param message 配送されたメッセージ。検証では使用しない。
 * @param user FSubscribeDuringPublishContext のアドレス。
 */
void OnSubscribeDuringPublish(const FTypedMessage& message, void* user) noexcept
{
    (void)message;
    /** broker と呼び出し回数を共有する状態。 */
    auto& context = *static_cast<FSubscribeDuringPublishContext*>(user);
    ++context.initial_calls;
    if (context.subscribed) return;
    context.subscribed = true;
    context.broker->SubscribeTyped<FTypedMessage, &OnAddedDuringPublish>(&context);
}

/** timer の呼び出し回数を保持する。 */
struct FTimerCounter {
    /** callback の呼び出し回数。 */
    u32 hits = 0;
};

/**
 * 生ポインタ timer callback の呼び出しを記録する。
 *
 * @param user FTimerCounter のアドレス。
 */
void CountTimer(void* user) noexcept
{
    ++static_cast<FTimerCounter*>(user)->hits;
}

/**
 * 型付き timer callback の呼び出しを記録する。
 *
 * @param user 呼び出し回数の書き込み先。
 */
void CountTypedTimer(FTimerCounter* user) noexcept
{
    ++user->hits;
}

/**
 * 二つの NUL 終端文字列を比較する。
 *
 * @param left 左辺文字列。
 * @param right 右辺文字列。
 * @return 同じ内容または同じ null 状態なら true。
 */
bool TextEquals(const char* left, const char* right) noexcept
{
    if (!left || !right) return left == right;
    while (*left != '\0' && *right != '\0') {
        if (*left++ != *right++) return false;
    }
    return *left == *right;
}

} // namespace

/** ThreadPool の一括投入、通知抑制、callable 寿命を検証する。 */
ACS_TEST(FoundationOptimizationWaveB, ThreadPoolBatchesAndAvoidsRedundantWakeups)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4).IsOk());
    /** park 完了を待つワーカー数。 */
    const u32 worker_count = CThreadPool::WorkerCount();
    /** 初期ワーカーが park へ入るまでの待機位置。 */
    for (u32 wait = 0; wait < 10000 && CThreadPool::Diagnostics().worker_parks < worker_count; ++wait) {
        SleepMs(1);
    }
    /** burst 前に観測した park 回数。 */
    const u64 initial_worker_parks = CThreadPool::Diagnostics().worker_parks;
    EXPECT_EQ(worker_count, 4u);
    EXPECT_TRUE(initial_worker_parks >= worker_count);
    CThreadPool::ResetDiagnostics();

    /** 投入する待機タスク数。 */
    constexpr u32 kTaskCount = 512;
    /** 全タスクが共有する待機状態。 */
    FGateTaskContext context;
    /** 全タスクの完了カウンタ。 */
    FCompletionCounter completed;
    /** 投入するタスク位置。 */
    for (u32 i = 0; i < kTaskCount; ++i) {
        EXPECT_TRUE(CThreadPool::Submit(FTask{&RunGatedTask, &context, &completed}).IsOk());
    }
    context.release.Store(1, EMemoryOrder::Release);

    /** 実行完了を待つ再試行位置。 */
    for (u32 wait = 0; wait < 10000 && context.executed.Load(EMemoryOrder::Acquire) != kTaskCount; ++wait) {
        SleepMs(1);
    }
    CThreadPool::Wait(completed);

    /** 一括投入後の ThreadPool 診断値。 */
    const FThreadPoolDiagnostics diagnostics = CThreadPool::Diagnostics();
    EXPECT_EQ(context.executed.Load(EMemoryOrder::Acquire), kTaskCount);
    EXPECT_EQ(diagnostics.external_tasks_drained, u64{kTaskCount});
    EXPECT_TRUE(diagnostics.submission_drain_lock_acquisitions < kTaskCount);
    EXPECT_TRUE(diagnostics.wake_one_calls > 0);
    EXPECT_TRUE(diagnostics.wake_one_calls <= 4);
    EXPECT_EQ(diagnostics.submission_heap_fallbacks, 0ull);
    EXPECT_EQ(diagnostics.queued_work, 0u);

    /** callable の実行回数。 */
    TAtomic<u32> callable_executed{0};
    /** 大型 callable の破棄回数。 */
    u32 large_callable_destructions = 0;
    /** callable 群の完了カウンタ。 */
    FCompletionCounter callable_completed;
    CThreadPool::ResetDiagnostics();
    EXPECT_TRUE(CThreadPool::SubmitCallable([&callable_executed](u32) noexcept { callable_executed.FetchAdd(1); }, &callable_completed).IsOk());
    EXPECT_TRUE(CThreadPool::SubmitCallable(FLargeGraphCallable{&callable_executed, &large_callable_destructions}, &callable_completed).IsOk());
    CThreadPool::Wait(callable_completed);
    /** callable 実行後の ThreadPool 診断値。 */
    const FThreadPoolDiagnostics callable_diagnostics = CThreadPool::Diagnostics();
    EXPECT_EQ(callable_executed.Load(EMemoryOrder::Acquire), 2u);
    EXPECT_EQ(large_callable_destructions, 1u);
    EXPECT_EQ(callable_diagnostics.callable_inline_submissions, 1ull);
    EXPECT_EQ(callable_diagnostics.callable_heap_submissions, 1ull);
    EXPECT_EQ(callable_diagnostics.callable_node_heap_fallbacks, 0ull);

    /** move 構築中に終了要求した callable の実行回数。 */
    TAtomic<u32> shutdown_during_move_executed{0};
    /** move 構築中終了要求の完了カウンタ。 */
    FCompletionCounter shutdown_during_move_completed;
    EXPECT_TRUE(CThreadPool::SubmitCallable(FShutdownDuringMoveCallable{&shutdown_during_move_executed}, &shutdown_during_move_completed).IsOk());
    CThreadPool::Wait(shutdown_during_move_completed);
    EXPECT_EQ(shutdown_during_move_executed.Load(EMemoryOrder::Acquire), 1u);
    EXPECT_EQ(CThreadPool::WorkerCount(), 4u);

    std::printf("wave_b_threadpool tasks=%u drain_locks=%llu wakes=%llu contentions=%llu heap_fallbacks=%llu inline_callables=%llu heap_callables=%llu\n", kTaskCount, static_cast<unsigned long long>(diagnostics.submission_drain_lock_acquisitions), static_cast<unsigned long long>(diagnostics.wake_one_calls), static_cast<unsigned long long>(diagnostics.submission_lock_contentions), static_cast<unsigned long long>(diagnostics.submission_heap_fallbacks), static_cast<unsigned long long>(callable_diagnostics.callable_inline_submissions), static_cast<unsigned long long>(callable_diagnostics.callable_heap_submissions));
    CThreadPool::Shutdown();
}

/** JobGraph の topology 再利用と callable 寿命を検証する。 */
ACS_TEST(FoundationOptimizationWaveB, JobGraphReusesTopologyAndOwnsCallables)
{
    CThreadPool::Shutdown();
    EXPECT_TRUE(CThreadPool::Init(4).IsOk());

    /** graph へ登録する job 数。 */
    constexpr u32 kJobCount = 40;
    /** 同じ topology を実行する回数。 */
    constexpr u32 kRunCount = 3;
    /** 全 job の実行回数。 */
    TAtomic<u32> executed{0};
    /** 大型 callable の破棄回数。 */
    u32 large_callable_destructions = 0;
    {
        /** 再利用する JobGraph。 */
        CJobGraph graph;
        /** 登録した job の handle。 */
        FJobHandle handles[kJobCount]{};
        /** job を登録する位置。 */
        for (u32 i = 0; i + 1 < kJobCount; ++i) {
            handles[i] = graph.AddCallable([&executed](u32) noexcept { executed.FetchAdd(1); });
            EXPECT_TRUE(handles[i].IsValid());
        }
        handles[kJobCount - 1] = graph.AddCallable(FLargeGraphCallable{&executed, &large_callable_destructions});
        EXPECT_TRUE(handles[kJobCount - 1].IsValid());

        /** 直列依存を設定する job 位置。 */
        for (u32 i = 1; i < kJobCount; ++i)
            handles[i].DependOn(handles[i - 1]);

        /** graph を再実行する位置。 */
        for (u32 run = 0; run < kRunCount; ++run) {
            if (run != 0) graph.Reset();
            EXPECT_TRUE(graph.Submit().IsOk());
            graph.Wait();
        }

        /** graph 再利用後の診断値。 */
        const FJobGraphDiagnostics diagnostics = graph.Diagnostics();
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
    CThreadPool::Shutdown();
}

/** MPMC パイプの上限、バッチ処理、FIFO 順序を検証する。 */
ACS_TEST(FoundationOptimizationWaveB, MessagePipeBatchingIsBoundedAndFifo)
{
    /** 八件を上限とする検証対象パイプ。 */
    TMessagePipe<i32> pipe(8);
    /** 最初に追加する連番値。 */
    i32 input[8]{0, 1, 2, 3, 4, 5, 6, 7};
    EXPECT_EQ(pipe.PushBatch(input, 8), usize{8});
    EXPECT_FALSE(pipe.Push(8));

    /** 最初に取り出す三件。 */
    i32 first_batch[3]{};
    EXPECT_EQ(pipe.TryPopBatch(first_batch, 3), usize{3});
    EXPECT_EQ(first_batch[0], 0);
    EXPECT_EQ(first_batch[1], 1);
    EXPECT_EQ(first_batch[2], 2);

    /** 空きへ追加する後続三件。 */
    i32 second_input[3]{8, 9, 10};
    EXPECT_EQ(pipe.PushBatch(second_input, 3), usize{3});
    /** 残りを取り出す出力領域。 */
    i32 output[8]{};
    EXPECT_EQ(pipe.TryPopBatch(output, 8), usize{8});
    /** 出力順序を確認する位置。 */
    for (i32 i = 0; i < 8; ++i)
        EXPECT_EQ(output[i], i + 3);
    EXPECT_EQ(pipe.Size(), usize{0});
}

/** MPMC パイプが並行配送で各値を一度だけ届けることを検証する。 */
ACS_TEST(FoundationOptimizationWaveB, MessagePipeMpmcStressIsExactlyOnce)
{
    /** 並行 producer 数。 */
    constexpr u32 kProducerCount = 2;
    /** 並行 consumer 数。 */
    constexpr u32 kConsumerCount = 2;
    /** producer 一つが追加する値の個数。 */
    constexpr u32 kValuesPerProducer = 2000;
    /** 全 producer が追加する値の総数。 */
    constexpr u32 kValueCount = kProducerCount * kValuesPerProducer;

    /** 負荷時の増加を制限する検証対象パイプ。 */
    TMessagePipe<u32> pipe(256);
    /** 値ごとの観測回数。 */
    TAtomic<u32> seen[kValueCount]{};
    /** 全 consumer の取り出し総数。 */
    TAtomic<u32> consumed{0};
    /** consumer が共有する検証状態。 */
    FPipeConsumerContext consumer_context{&pipe, seen, &consumed, kValueCount};
    /** producer ごとの入力範囲。 */
    FPipeProducerContext producer_contexts[kProducerCount]{{&pipe, 0, kValuesPerProducer}, {&pipe, kValuesPerProducer, kValuesPerProducer}};

    /** 並行 consumer スレッド。 */
    FThread consumers[kConsumerCount];
    /** consumer を生成する位置。 */
    for (u32 i = 0; i < kConsumerCount; ++i) {
        /** consumer の生成結果。 */
        auto result = FThread::Spawn(&ConsumeMpmcValues, &consumer_context);
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) {
            pipe.Close();
            return;
        }
        consumers[i] = Move(result.Value());
    }

    /** 並行 producer スレッド。 */
    FThread producers[kProducerCount];
    /** producer を生成する位置。 */
    for (u32 i = 0; i < kProducerCount; ++i) {
        /** producer の生成結果。 */
        auto result = FThread::Spawn(&ProduceMpmcValues, &producer_contexts[i]);
        EXPECT_TRUE(result.IsOk());
        if (result.IsErr()) {
            pipe.Close();
            return;
        }
        producers[i] = Move(result.Value());
    }

    /** 完了を待つ producer 位置。 */
    for (u32 i = 0; i < kProducerCount; ++i)
        producers[i].Join();
    pipe.Close();
    /** 完了を待つ consumer 位置。 */
    for (u32 i = 0; i < kConsumerCount; ++i)
        consumers[i].Join();

    EXPECT_EQ(consumed.Load(EMemoryOrder::Acquire), kValueCount);
    /** 一意配送を確認する値。 */
    for (u32 i = 0; i < kValueCount; ++i)
        EXPECT_EQ(seen[i].Load(EMemoryOrder::Acquire), 1u);
}

/** SPSC 特殊化が大量配送でも順序を維持することを検証する。 */
ACS_TEST(FoundationOptimizationWaveB, MessagePipeSpscSpecializationPreservesOrder)
{
    /** 送受信する連番値の個数。 */
    constexpr u32 kValueCount = 100000;
    /** 検証対象の SPSC パイプ。 */
    FSpscPipe pipe;
    /** producer と consumer が共有する状態。 */
    FSpscContext context;
    context.pipe = &pipe;
    context.count = kValueCount;

    /** consumer の生成結果。 */
    auto consumer_result = FThread::Spawn(&ConsumeSpscValues, &context);
    /** producer の生成結果。 */
    auto producer_result = FThread::Spawn(&ProduceSpscValues, &context);
    EXPECT_TRUE(consumer_result.IsOk());
    EXPECT_TRUE(producer_result.IsOk());
    if (consumer_result.IsErr() || producer_result.IsErr()) {
        pipe.Close();
        return;
    }

    /** 生成済み consumer スレッド。 */
    FThread consumer = Move(consumer_result.Value());
    /** 生成済み producer スレッド。 */
    FThread producer = Move(producer_result.Value());
    producer.Join();
    consumer.Join();
    pipe.Close();

    EXPECT_EQ(context.failures.Load(EMemoryOrder::Acquire), 0u);
    EXPECT_EQ(pipe.Size(), usize{0});
}

/** 型付き broker が入れ子配送中の購読解除を安全に反映することを検証する。 */
ACS_TEST(FoundationOptimizationWaveB, TypedBrokerPreservesNestedPublishCancellation)
{
    /** 検証対象の message broker。 */
    CMessageBroker broker;
    /** 入れ子配送と呼び出し回数の状態。 */
    FTypedBrokerContext context;
    context.broker = &broker;
    /** 型付き callback の購読 handle。 */
    const FSubscriptionHandle typed = broker.SubscribeTyped<FTypedMessage, &OnTypedMessage>(&context);
    context.cancel_on_first = broker.Subscribe<FTypedMessage>(&OnRawMessage, &context);
    EXPECT_TRUE(typed.IsValid());
    EXPECT_TRUE(context.cancel_on_first.IsValid());

    broker.Publish(FTypedMessage{1});
    EXPECT_EQ(context.typed_calls, 2);
    EXPECT_EQ(context.raw_calls, 0);
    EXPECT_EQ(broker.SubscriberCount(GetEventTypeId<FTypedMessage>()), 1u);
    EXPECT_TRUE(broker.Unsubscribe(typed));
}

/** Publish 中の購読追加が次回配送まで遅延することを検証する。 */
ACS_TEST(FoundationOptimizationWaveB, BrokerDefersSubscriptionAddedDuringPublish)
{
    /** 検証対象の message broker。 */
    CMessageBroker broker;
    /** 発行中購読追加の検証状態。 */
    FSubscribeDuringPublishContext context;
    context.broker = &broker;

    /** 発行開始時から存在する購読 handle。 */
    const FSubscriptionHandle initial = broker.SubscribeTyped<FTypedMessage, &OnSubscribeDuringPublish>(&context);
    /** 購読解除後の slot 再利用を誘発する handle。 */
    const FSubscriptionHandle reusable = broker.SubscribeTyped<FTypedMessage, &OnAddedDuringPublish>(&context);
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

/** Timer の直接 cancel と active bitset 走査量を検証する。 */
ACS_TEST(FoundationOptimizationWaveB, TimerUsesDirectCancelAndActiveBitset)
{
    /** 登録して大半を解除する timer 数。 */
    constexpr u32 kTimerCount = 8192;
    /** 検証対象の timer manager。 */
    CTimerManager timers;
    /** callback 呼び出し回数。 */
    FTimerCounter counter;
    /** 登録した timer の handle。 */
    FTimerHandle handles[kTimerCount]{};
    /** timer を登録する位置。 */
    for (u32 i = 0; i < kTimerCount; ++i)
        handles[i] = timers.SetTimeout(10.0f, &CountTimer, &counter);

    timers.ResetDiagnostics();
    /** cancel 計測の開始 tick。 */
    const u64 cancel_start = CClock::Ticks();
    /** cancel する timer 位置。 */
    for (u32 i = 0; i + 1 < kTimerCount; ++i)
        EXPECT_TRUE(timers.Cancel(handles[i]));
    /** cancel 計測の終了 tick。 */
    const u64 cancel_end = CClock::Ticks();
    /** cancel 後の timer 診断値。 */
    const FTimerDiagnostics cancel_diagnostics = timers.Diagnostics();
    EXPECT_EQ(cancel_diagnostics.cancel_slot_probes, u64{kTimerCount - 1});
    EXPECT_EQ(timers.ActiveCount(), 1u);

    timers.ResetDiagnostics();
    timers.Tick(0.0f);
    /** 残存 timer 一件を走査した診断値。 */
    const FTimerDiagnostics tick_diagnostics = timers.Diagnostics();
    EXPECT_EQ(tick_diagnostics.active_slots_visited, 1ull);
    EXPECT_EQ(tick_diagnostics.active_words_loaded, u64{kTimerCount / 64});
    EXPECT_EQ(counter.hits, 0u);
    EXPECT_TRUE(timers.Cancel(handles[kTimerCount - 1]));

    /** 型付き callback で登録した単発 timer。 */
    const FTimerHandle typed = timers.Schedule<ETimerSchedulePolicy::Once, &CountTypedTimer>(0.0f, &counter);
    EXPECT_TRUE(typed.IsValid());
    timers.Tick(0.0f);
    EXPECT_EQ(counter.hits, 1u);

    /** cancel 群に要したミリ秒。 */
    const f64 cancel_ms = static_cast<f64>(cancel_end - cancel_start) * 1000.0 / static_cast<f64>(CClock::TicksPerSecond());
    std::printf("wave_b_timer timers=%u cancel_probes=%llu tick_active_visits=%llu tick_words=%llu cancel_ms=%.3f\n", kTimerCount, static_cast<unsigned long long>(cancel_diagnostics.cancel_slot_probes), static_cast<unsigned long long>(tick_diagnostics.active_slots_visited), static_cast<unsigned long long>(tick_diagnostics.active_words_loaded), cancel_ms);
}

/** FileSystem の単一確保読み込み、原子的保存、入力検証を確認する。 */
ACS_TEST(FoundationOptimizationWaveB, FileIoReadsTextWithoutIntermediateCopy)
{
    /** 読み書きの基本経路を検証する一時ファイル。 */
    constexpr const wchar_t* kPath = L"acs_foundation_optimization_wave_b.txt";
    /** 原子的な Storage 保存を検証する一時ファイル。 */
    constexpr const wchar_t* kStoragePath = L"acs_foundation_optimization_wave_b.ini";
    /** 子ディレクトリ作成を阻害する一時ファイル。 */
    constexpr const wchar_t* kParentFilePath = L"acs_foundation_optimization_wave_b_parent";
    /** 親がファイルの場合に拒否する子パス。 */
    constexpr const wchar_t* kBlockedChildPath = L"acs_foundation_optimization_wave_b_parent\\child";
    /** 読み書きするテキスト。 */
    constexpr const char* kText = "ACS transactional file fixture\n";
    (void)CFileSystem::Delete(kPath);
    (void)CFileSystem::Delete(kStoragePath);
    (void)CFileSystem::Delete(kParentFilePath);
    EXPECT_TRUE(CFileSystem::WriteAllText(kPath, kText).IsOk());

    CFileSystem::ResetDiagnostics();
    /** テキスト全体の読み取り結果。 */
    auto text_result = CFileSystem::ReadAllText(kPath);
    EXPECT_TRUE(text_result.IsOk());
    if (text_result.IsOk()) {
        EXPECT_TRUE(TextEquals(text_result.Value().GetData(), kText));
    }
    /** テキスト読み取り後の I/O 診断値。 */
    const FFileSystemDiagnostics diagnostics = CFileSystem::Diagnostics();
    EXPECT_EQ(diagnostics.read_syscalls, 1ull);
    EXPECT_EQ(diagnostics.text_intermediate_copy_bytes, 0ull);
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kPath, nullptr, 1).IsErr());
    /** サイズ上限検証へ渡す一 byte。 */
    constexpr byte kOneByte[]{0};
    EXPECT_TRUE(CFileSystem::WriteAllBytes(kPath, kOneByte, static_cast<usize>(0xffffffffull) + 1u).IsErr());

    /** 拡張子互換と原子的保存を検証する Storage。 */
    FStorage storage;
    storage.SetInt("extension-compatibility", 7);
    /** 拡張子で従来の INI 内容保存を拒否しないことを確認するパス。 */
    constexpr const wchar_t* kCompatibleStoragePaths[]{L"acs_foundation_optimization_wave_b.json", L"acs_foundation_optimization_wave_b.bin", L"acs_foundation_optimization_wave_b.acpak"};
    /** 内容互換を確認する保存先パス。 */
    for (const wchar_t* compatible_path : kCompatibleStoragePaths) {
        (void)CFileSystem::Delete(compatible_path);
        EXPECT_TRUE(storage.Save(compatible_path).IsOk());
        /** 拡張子に依存せず保存内容を復元する確認先。 */
        FStorage compatible_loaded;
        EXPECT_TRUE(compatible_loaded.Load(compatible_path).IsOk());
        EXPECT_EQ(compatible_loaded.GetInt("extension-compatibility"), 7ll);
        EXPECT_TRUE(CFileSystem::Delete(compatible_path).IsOk());
    }

    // 公開先を共有禁止で保持し、置換失敗時にも旧内容が残ることを確認する。
    EXPECT_TRUE(CFileSystem::WriteAllText(kStoragePath, "stable-before-failed-replace").IsOk());
    /** 置換を意図的に失敗させる共有禁止 handle。 */
    const HANDLE locked_file = ::CreateFileW(kStoragePath, GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    EXPECT_TRUE(locked_file != INVALID_HANDLE_VALUE);
    /** 原子的置換で書こうとする新内容。 */
    constexpr byte kReplacement[]{'n', 'e', 'w'};
    EXPECT_TRUE(CFileSystem::WriteAllBytesAtomic(kStoragePath, kReplacement, sizeof(kReplacement)).IsErr());
    if (locked_file != INVALID_HANDLE_VALUE) ::CloseHandle(locked_file);
    /** 置換失敗後に残った内容の読み取り結果。 */
    auto preserved_text = CFileSystem::ReadAllText(kStoragePath);
    EXPECT_TRUE(preserved_text.IsOk());
    if (preserved_text.IsOk()) {
        EXPECT_TRUE(TextEquals(preserved_text.Value().GetData(), "stable-before-failed-replace"));
    }

    storage.SetInt("generation", 42);
    /** Storage の原子的保存結果。 */
    auto save_result = storage.Save(kStoragePath);
    if (save_result.IsErr()) {
        std::printf("wave_b_atomic_save_error message=%s os_error=%u\n", save_result.Error().message, save_result.Error().os_error);
    }
    EXPECT_TRUE(save_result.IsOk());
    /** 保存内容を復元する Storage。 */
    FStorage loaded;
    EXPECT_TRUE(loaded.Load(kStoragePath).IsOk());
    EXPECT_EQ(loaded.GetInt("generation"), 42ll);

    EXPECT_TRUE(CFileSystem::ReadAllBytes(nullptr).IsErr());
    EXPECT_TRUE(CFileSystem::CreateDirectory(nullptr).IsErr());
    EXPECT_TRUE(CFileSystem::WriteAllBytesAtomic(L"", nullptr, 0).IsErr());
    EXPECT_TRUE(storage.Load(L"").IsErr());
    EXPECT_TRUE(storage.Save(L"").IsErr());
    EXPECT_TRUE(CFileSystem::WriteAllText(kParentFilePath, "parent-is-a-file").IsOk());
    EXPECT_TRUE(CFileSystem::CreateDirectory(kBlockedChildPath).IsErr());
    EXPECT_TRUE(CFileSystem::Delete(kPath).IsOk());
    EXPECT_TRUE(CFileSystem::Delete(kStoragePath).IsOk());
    EXPECT_TRUE(CFileSystem::Delete(kParentFilePath).IsOk());

    std::printf("wave_b_file_io read_syscalls=%llu text_intermediate_copy_bytes=%llu\n", static_cast<unsigned long long>(diagnostics.read_syscalls), static_cast<unsigned long long>(diagnostics.text_intermediate_copy_bytes));
}
