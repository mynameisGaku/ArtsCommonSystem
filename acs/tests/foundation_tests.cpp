// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "foundation/SourceLoc.h"
#include "foundation/Log.h"
#include "foundation/Move.h"
#include "foundation/StackTrace.h"
#include "memory/SystemAllocator.h"
#include "gameframework/VoiceChat.h"
#include "assetpack/AcpakReader.h"
#include "assetpack/AcpakWriter.h"
#include "platform/FileSystem.h"
#include "threading/Atomic.h"
#include "threading/Thread.h"

using namespace acs;

namespace {

/** Logger lifecycle の同時操作を揃えて開始するテスト用状態。 */
struct FLoggerLifecycleContext {
    FLogConfig config{};
    TAtomic<u32> phase{0};
    TAtomic<u32> operation{0};
    TAtomic<u32> ready{0};
    TAtomic<u32> done{0};
};

TAtomic<u32> g_logger_sink_hits{0};
TAtomic<u32> g_blocking_sink_entered{0};
TAtomic<u32> g_blocking_sink_release{0};
TAtomic<u32> g_set_sink_completed{0};
TAtomic<u32> g_ordering_sink_hits{0};
TAtomic<u32> g_ordering_sink_observed_incomplete{0};
TAtomic<u32> g_flush_started{0};
TAtomic<u32> g_flush_completed{0};

/** lifecycle 競合中に呼ばれたログ件数を記録する。 */
void CountingLoggerSink(ELogSeverity, const char*)
{
    g_logger_sink_hits.FetchAdd(1);
}

/** SetSink の待機契約を決定的に検証するため、解除指示まで callback 内で待つ。 */
void BlockingLoggerSink(ELogSeverity, const char*)
{
    g_blocking_sink_entered.Store(1);
    while (g_blocking_sink_release.Load() == 0)
        Yield();
}

/** 別スレッドから sink を解除し、戻った時点を記録する。 */
void ClearLoggerSinkWorker(void*)
{
    FLogger::SetSink(nullptr);
    g_set_sink_completed.Store(1);
}

/** 新 sink が SetSink の復帰前に開始していないことを検証する。 */
void OrderingLoggerSink(ELogSeverity, const char*)
{
    g_ordering_sink_hits.FetchAdd(1);
    for (u32 i = 0; i < 200 && g_set_sink_completed.Load() == 0; ++i)
        SleepMs(1);
    if (g_set_sink_completed.Load() == 0) g_ordering_sink_observed_incomplete.Store(1);
}

/** 旧 callback の完了を待ってから検証用 sink を登録する。 */
void InstallOrderingLoggerSinkWorker(void*)
{
    FLogger::SetSink(&OrderingLoggerSink);
    g_set_sink_completed.Store(1);
}

/** 別スレッドから Flush を呼び、復帰した時点を記録する。 */
void FlushLoggerWorker(void*)
{
    g_flush_started.Store(1);
    FLogger::Flush();
    g_flush_completed.Store(1);
}

/** phase ごとに全 worker が同じ Logger 操作を実行する。 */
void LoggerLifecycleWorker(void* user)
{
    auto& context = *static_cast<FLoggerLifecycleContext*>(user);
    u32 observed_phase = 0;
    context.ready.FetchAdd(1);

    for (;;) {
        const u32 phase = context.phase.Load();
        if (phase == observed_phase) {
            Yield();
            continue;
        }
        observed_phase = phase;

        switch (context.operation.Load()) {
        case 0:
            return;
        case 1:
            FLogger::Init(context.config);
            break;
        case 2:
            FLogger::SetSink(&CountingLoggerSink);
            break;
        case 3:
            FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "logger lifecycle race");
            break;
        case 4:
            FLogger::Shutdown();
            break;
        default:
            break;
        }
        context.done.FetchAdd(1);
    }
}

/** 全 worker へ1操作を発行し、全員の完了まで待つ。 */
void RunLoggerLifecyclePhase(FLoggerLifecycleContext& context, u32 operation, u32 worker_count)
{
    context.done.Store(0);
    context.operation.Store(operation);
    context.phase.FetchAdd(1);
    while (context.done.Load() != worker_count)
        Yield();
}

/** 後続テスト用に main と同じ Logger 設定を復元する。 */
void RestoreTestLogger()
{
    FLogger::Shutdown();
    FLogConfig config{};
    config.console = true;
    config.debug_output = false;
    config.min_severity = ELogSeverity::Info;
    FLogger::Init(config);
}

} // namespace

ACS_TEST(Foundation, ResultOk)
{
    TResult<int> r = Ok(42);
    EXPECT_TRUE(r.IsOk());
    EXPECT_FALSE(r.IsErr());
    EXPECT_EQ(r.Value(), 42);
}

ACS_TEST(Foundation, ResultErr)
{
    TResult<int> r = Err<int>(ACS_ERR(Generic, 7, "boom"));
    EXPECT_FALSE(r.IsOk());
    EXPECT_TRUE(r.IsErr());
    EXPECT_EQ(r.Error().subcode, (u16)7);
}

ACS_TEST(Foundation, ResultVoid)
{
    TResult<void> r = Ok();
    EXPECT_TRUE(r.IsOk());
    TResult<void> e = Err(ACS_ERR(IO, 3, "io"));
    EXPECT_TRUE(e.IsErr());
}

ACS_TEST(Foundation, SourceLocCaptures)
{
    FSourceLoc s = FSourceLoc::Current();
    EXPECT_TRUE(s.Line() > 0);
    EXPECT_TRUE(s.File() != nullptr);
}

ACS_TEST(Foundation, LoggerEmits)
{
    ACS_LOG_INFO("foundation log smoke test value=%d", 7);
    FLogger::Flush();
    EXPECT_TRUE(true);
}

ACS_TEST(Foundation, LoggerOversizedCapacityIsBounded)
{
    FLogger::Shutdown();
    FLogConfig config{};
    config.console = false;
    config.debug_output = false;
    config.ring_capacity = ~u32{0};

    // 最大値を渡しても 2 のべき乗化が桁あふれせず、有限の上限で初期化できる。
    FLogger::Init(config);
    EXPECT_TRUE(FLogger::IsInitialized());
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "bounded logger capacity");
    FLogger::Flush();

    RestoreTestLogger();
}

ACS_TEST(Foundation, LoggerConcurrentLifecycleIsSerialized)
{
    FLogger::Shutdown();
    EXPECT_TRUE(!FLogger::IsInitialized());
    g_logger_sink_hits.Store(0);

    FLoggerLifecycleContext context;
    context.config.console = false;
    context.config.debug_output = false;
    context.config.min_severity = ELogSeverity::Trace;
    context.config.ring_capacity = 64;

    constexpr u32 kWorkerCount = 4;
    FThread workers[kWorkerCount];
    u32 worker_count = 0;
    for (u32 i = 0; i < kWorkerCount; ++i) {
        auto result = FThread::Spawn(&LoggerLifecycleWorker, &context);
        if (result.IsOk()) workers[worker_count++] = Move(result.Value());
    }
    EXPECT_EQ(worker_count, kWorkerCount);
    while (context.ready.Load() != worker_count)
        Yield();

    constexpr u32 kLifecycleCount = 32;
    for (u32 i = 0; i < kLifecycleCount && worker_count != 0; ++i) {
        RunLoggerLifecyclePhase(context, 1, worker_count);
        EXPECT_TRUE(FLogger::IsInitialized());
        EXPECT_TRUE(FLogger::Enabled(ELogSeverity::Info));
        RunLoggerLifecyclePhase(context, 2, worker_count);
        RunLoggerLifecyclePhase(context, 3, worker_count);
        RunLoggerLifecyclePhase(context, 4, worker_count);
        EXPECT_TRUE(!FLogger::IsInitialized());
        EXPECT_TRUE(!FLogger::Enabled(ELogSeverity::Info));
    }

    context.operation.Store(0);
    context.phase.FetchAdd(1);
    for (u32 i = 0; i < worker_count; ++i)
        workers[i].Join();
    EXPECT_TRUE(g_logger_sink_hits.Load() != 0);

    // 非稼働中の SetSink は次 lifecycle へ持ち越されない。
    const u32 hits_before_inactive_sink = g_logger_sink_hits.Load();
    FLogger::Shutdown();
    FLogger::Shutdown();
    FLogger::SetSink(&CountingLoggerSink);
    FLogger::Init(context.config);
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "inactive sink ignored");
    FLogger::Flush();
    EXPECT_EQ(g_logger_sink_hits.Load(), hits_before_inactive_sink);

    FLogger::SetSink(&CountingLoggerSink);
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "active sink accepted");
    FLogger::Flush();
    EXPECT_TRUE(g_logger_sink_hits.Load() > hits_before_inactive_sink);
    FLogger::SetSink(nullptr);

    RestoreTestLogger();
}

ACS_TEST(Foundation, LoggerSetSinkWaitsForInFlightCallback)
{
    FLogger::Shutdown();
    FLogConfig config{};
    config.console = false;
    config.debug_output = false;
    config.ring_capacity = 16;
    FLogger::Init(config);

    g_blocking_sink_entered.Store(0);
    g_blocking_sink_release.Store(0);
    g_set_sink_completed.Store(0);
    FLogger::SetSink(&BlockingLoggerSink);
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "blocking sink");
    while (g_blocking_sink_entered.Load() == 0)
        Yield();

    auto result = FThread::Spawn(&ClearLoggerSinkWorker, nullptr);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) {
        g_blocking_sink_release.Store(1);
        RestoreTestLogger();
        return;
    }

    SleepMs(10);
    EXPECT_EQ(g_set_sink_completed.Load(), 0u);
    g_blocking_sink_release.Store(1);
    result.Value().Join();
    EXPECT_EQ(g_set_sink_completed.Load(), 1u);

    RestoreTestLogger();
}

ACS_TEST(Foundation, LoggerSetSinkDoesNotWaitForReplacementCallback)
{
    FLogger::Shutdown();
    FLogConfig config{};
    config.console = false;
    config.debug_output = false;
    config.ring_capacity = 16;
    FLogger::Init(config);

    g_blocking_sink_entered.Store(0);
    g_blocking_sink_release.Store(0);
    g_set_sink_completed.Store(0);
    g_ordering_sink_hits.Store(0);
    g_ordering_sink_observed_incomplete.Store(0);
    FLogger::SetSink(&BlockingLoggerSink);
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "old blocking sink");
    while (g_blocking_sink_entered.Load() == 0)
        Yield();

    // 旧 callback の後ろにもレコードを積み、交換後の callback が SetSink の
    // 復帰待ちへ誤って含まれる実装を決定的に通す。
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "queued during sink exchange");
    auto result = FThread::Spawn(&InstallOrderingLoggerSinkWorker, nullptr);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) {
        g_blocking_sink_release.Store(1);
        RestoreTestLogger();
        return;
    }

    SleepMs(10);
    EXPECT_EQ(g_set_sink_completed.Load(), 0u);
    g_blocking_sink_release.Store(1);
    result.Value().Join();
    EXPECT_EQ(g_set_sink_completed.Load(), 1u);
    EXPECT_EQ(g_ordering_sink_observed_incomplete.Load(), 0u);

    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "replacement sink active");
    FLogger::Flush();
    EXPECT_TRUE(g_ordering_sink_hits.Load() > 0);

    RestoreTestLogger();
}

ACS_TEST(Foundation, LoggerFlushWaitsForInFlightCallback)
{
    FLogger::Shutdown();
    FLogConfig config{};
    config.console = false;
    config.debug_output = false;
    config.ring_capacity = 16;
    FLogger::Init(config);

    g_blocking_sink_entered.Store(0);
    g_blocking_sink_release.Store(0);
    g_flush_started.Store(0);
    g_flush_completed.Store(0);
    FLogger::SetSink(&BlockingLoggerSink);
    FLogger::Write(ELogSeverity::Info, FSourceLoc::Current(), "blocking flush sink");
    while (g_blocking_sink_entered.Load() == 0)
        Yield();

    auto result = FThread::Spawn(&FlushLoggerWorker, nullptr);
    EXPECT_TRUE(result.IsOk());
    if (result.IsErr()) {
        g_blocking_sink_release.Store(1);
        RestoreTestLogger();
        return;
    }

    while (g_flush_started.Load() == 0)
        Yield();
    SleepMs(50);
    EXPECT_EQ(g_flush_completed.Load(), 0u);
    g_blocking_sink_release.Store(1);
    result.Value().Join();
    EXPECT_EQ(g_flush_completed.Load(), 1u);

    RestoreTestLogger();
}

ACS_TEST(Foundation, StackTraceSymbolResolverLifecycle)
{
    FStackTrace::ShutdownSymbolResolver();
    FStackTrace::ShutdownSymbolResolver();
    EXPECT_FALSE(FStackTrace::IsSymbolResolverInitialized());

    FStackTrace first;
    first.Capture(0);
    EXPECT_TRUE(first.FrameCount() > 0);
    first.Resolve();
    const bool first_init_succeeded = FStackTrace::IsSymbolResolverInitialized();

    FStackTrace::ShutdownSymbolResolver();
    EXPECT_FALSE(FStackTrace::IsSymbolResolverInitialized());

    // 最初の SymInitialize が成功した環境では、明示終了後も Resolve で再初期化できる。
    FStackTrace second;
    second.Capture(0);
    second.Resolve();
    if (first_init_succeeded) {
        EXPECT_TRUE(FStackTrace::IsSymbolResolverInitialized());
    }

    FStackTrace::ShutdownSymbolResolver();
    EXPECT_FALSE(FStackTrace::IsSymbolResolverInitialized());
}

ACS_TEST(Foundation, VoiceLoopbackReleasesInjectedAllocator)
{
    FSystemAllocator allocator;
    {
        game::FVoiceChatLoopbackBackend backend(allocator);
        constexpr const char* kChannel = "allocator-lifetime-contract-party-channel";
        constexpr const char* kUser = "allocator-lifetime-contract-local-user";
        constexpr const char* kDisplay = "Allocator Lifetime Contract User";

        for (u32 lifecycle = 0; lifecycle < 2; ++lifecycle) {
            EXPECT_TRUE(backend.Init(game::EVoiceProvider::OpusSelf).IsOk());
            EXPECT_TRUE(backend.JoinChannel(game::EVoiceChannel::Party, kChannel).IsOk());
            EXPECT_TRUE(backend.AddParticipant(game::EVoiceChannel::Party, kUser, kDisplay).IsOk());
            EXPECT_TRUE(allocator.BytesAllocated() > 0u);

            backend.Shutdown();
            EXPECT_EQ(allocator.BytesAllocated(), 0ull);
        }
    }
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
}

ACS_TEST(Foundation, AcpakReaderWriterReleaseInjectedAllocator)
{
    constexpr const wchar_t* kPath = L"acs_static_lifetime_contract.acpak";
    constexpr const wchar_t* kVirtualPath = L"contracts/process-lifetime.txt";
    constexpr u8 kPayload[] = {'a', 'c', 's'};

    (void)FFileSystem::Delete(kPath);
    FSystemAllocator allocator;
    {
        assetpack::FAcpakWriter writer(allocator);
        auto opened = writer.Open(kPath, assetpack::AcpakFlagNone);
        EXPECT_TRUE(opened.IsOk());
        if (opened.IsErr()) return;

        EXPECT_TRUE(writer.AddFile(kVirtualPath, kPayload, sizeof(kPayload)).IsOk());
        EXPECT_TRUE(allocator.BytesAllocated() > 0u);
        EXPECT_TRUE(writer.Finalize().IsOk());
        writer.Close();
        EXPECT_EQ(allocator.BytesAllocated(), 0ull);

        assetpack::FAcpakReader reader(allocator);
        auto mounted = reader.Open(kPath);
        EXPECT_TRUE(mounted.IsOk());
        if (mounted.IsOk()) {
            EXPECT_TRUE(allocator.BytesAllocated() > 0u);
            reader.Close();
            EXPECT_EQ(allocator.BytesAllocated(), 0ull);
        }
    }
    EXPECT_EQ(allocator.BytesAllocated(), 0ull);
    (void)FFileSystem::Delete(kPath);
}
