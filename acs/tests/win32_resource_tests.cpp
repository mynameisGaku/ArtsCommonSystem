// SPDX-License-Identifier: Apache-2.0
// Win32 資源の取得・解放契約を、プロセスの実 HANDLE 数で検証する。
#include "test/Test.h"
#include "test/Expect.h"

#include "foundation/Platform.h"
#include "gameframework/HotReload.h"
#include "gameframework/NetSnapshot.h"
#include "gameframework/NetSnapshotDiagnosticsInternal.h"
#include "gameframework/StudioWorkflow.h"
#include "network/Network.h"
#include "network/UdpSocket.h"

#include <cstdio>

using namespace acs;
using namespace acs::game;

namespace {

/** 現在プロセスが保持する HANDLE 数を返す。取得失敗時は 0。 */
DWORD ProcessHandleCount() noexcept
{
    DWORD count = 0;
    if (!::GetProcessHandleCount(::GetCurrentProcess(), &count)) return 0;
    return count;
}

/** 非同期ローカルビルドが完了するまで短時間 poll する。 */
bool WaitForBuild(FLocalBuildRunner& runner, u64 build_identifier) noexcept
{
    for (u32 i = 0; i < 500; ++i) {
        auto result = runner.PollBuild(build_identifier);
        if (result.IsOk()) return true;
        ::Sleep(10);
    }
    return false;
}

/** FUdpTransport の WSA 参照計数を競合させるスレッド引数。 */
struct TransportThreadContext {
    HANDLE start_event = nullptr;
    u32 iteration_count = 0;
    bool success = false;
};

/** 同時開始後に UDP transport の接続・切断を反復する。 */
DWORD WINAPI TransportThreadMain(void* user) noexcept
{
    auto* context = static_cast<TransportThreadContext*>(user);
    if (context == nullptr || context->start_event == nullptr) return 1;
    if (::WaitForSingleObject(context->start_event, 10000) != WAIT_OBJECT_0) return 2;

    for (u32 i = 0; i < context->iteration_count; ++i) {
        FUdpTransport transport;
        auto connected = transport.Connect("127.0.0.1", 9);
        if (connected.IsErr()) return 3;
        transport.Disconnect();
        transport.Disconnect(); // 多重切断でも WSA 参照を二重に戻さない。
    }
    context->success = true;
    return 0;
}

/** 明示的に作成した HANDLE を全て回収し、UDP transport の同時実行を 1 周行う。 */
bool RunConcurrentUdpTransportRound(u32 iteration_count) noexcept
{
    constexpr DWORD kThreadCount = 8;
    HANDLE start_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (start_event == nullptr) return false;

    TransportThreadContext contexts[kThreadCount] = {};
    HANDLE threads[kThreadCount] = {};
    bool succeeded = true;
    bool created_all = true;

    for (DWORD i = 0; i < kThreadCount; ++i) {
        contexts[i].start_event = start_event;
        contexts[i].iteration_count = iteration_count;
        threads[i] = ::CreateThread(nullptr, 0, &TransportThreadMain, &contexts[i], 0, nullptr);
        if (threads[i] == nullptr) {
            created_all = false;
            succeeded = false;
            break;
        }
    }

    if (::SetEvent(start_event) == FALSE) succeeded = false;

    bool all_threads_completed = false;
    if (created_all) {
        all_threads_completed = ::WaitForMultipleObjects(kThreadCount, threads, TRUE, 30000) == WAIT_OBJECT_0;
        if (!all_threads_completed) succeeded = false;
    }

    for (DWORD i = 0; i < kThreadCount; ++i) {
        if (threads[i] == nullptr) continue;

        // 一括待機の失敗時も stack 上の context を破棄する前に必ず終了を確認する。
        if (!all_threads_completed && ::WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0) {
            succeeded = false;
        }

        DWORD exit_code = 0;
        if (::GetExitCodeThread(threads[i], &exit_code) == FALSE || exit_code != 0) {
            succeeded = false;
        }
        if (!contexts[i].success) succeeded = false;
        if (::CloseHandle(threads[i]) == FALSE) succeeded = false;
    }

    if (::CloseHandle(start_event) == FALSE) succeeded = false;
    return succeeded;
}

/** FUdpTransport の共有所有状態が完全に回収済みかを返す。 */
bool UdpTransportDiagnosticsAreClean(const UdpTransportDiagnostics& diagnostics) noexcept
{
    return diagnostics.active_winsock_reference_count == 0 && diagnostics.pending_cleanup_error == 0 &&
           !diagnostics.cleanup_debt_orphaned && diagnostics.orphaned_socket_count == 0 &&
           diagnostics.overflow_orphaned_socket_count == 0 && diagnostics.resource_release_failure_count == 0;
}

/**
 * UDP transport の共有所有値と、このテストが作成した全 HANDLE の明示解放を反復検証する。
 *
 * @details Winsock provider とテストプロセス内の別ランタイムは、負荷開始から離れた時点でも
 * 内部 Event を遅延生成して保持するため、少数のプロセス総 HANDLE 増加は直接リーク判定に
 * 使えない。総数は傾向を記録し、既知の遅延初期化幅を超える継続増加だけを guard する。
 * 主契約は ACS 内部値と RunConcurrentUdpTransportRound 内の CreateEvent / CreateThread に
 * 対応する全 CloseHandle の成功である。
 */
bool ValidateConcurrentUdpTransportOwnership() noexcept
{
    constexpr u32 kMeasurementRounds = 24;
    constexpr DWORD kMaximumExpectedLazyHandleGrowth = 4;

    if (!RunConcurrentUdpTransportRound(100)) return false;
    const UdpTransportDiagnostics warmup_diagnostics = FUdpTransport::CaptureDiagnostics();
    if (!UdpTransportDiagnosticsAreClean(warmup_diagnostics)) return false;

    const DWORD initial_handle_count = ProcessHandleCount();
    DWORD minimum_handle_count = initial_handle_count;
    DWORD maximum_handle_count = initial_handle_count;
    DWORD final_handle_count = initial_handle_count;
    DWORD previous_handle_count = initial_handle_count;
    DWORD sustained_growth = 0;
    DWORD maximum_sustained_growth = 0;

    for (u32 round = 0; round < kMeasurementRounds; ++round) {
        if (!RunConcurrentUdpTransportRound(100)) return false;

        const UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
        if (!UdpTransportDiagnosticsAreClean(diagnostics)) {
            ::acs::test::RecordInfo(::acs::FSourceLoc::Current(),
                                    "FUdpTransport diagnostics: references=%u cleanup_error=%u "
                                    "cleanup_orphaned=%u orphaned_sockets=%u release_failures=%llu round=%u",
                                    diagnostics.active_winsock_reference_count, diagnostics.pending_cleanup_error,
                                    diagnostics.cleanup_debt_orphaned ? 1u : 0u, diagnostics.orphaned_socket_count,
                                    static_cast<unsigned long long>(diagnostics.resource_release_failure_count),
                                    round + 1);
            return false;
        }

        const DWORD current_handle_count = ProcessHandleCount();
        if (current_handle_count != 0) {
            if (minimum_handle_count == 0 || current_handle_count < minimum_handle_count) {
                minimum_handle_count = current_handle_count;
            }
            if (current_handle_count > maximum_handle_count) maximum_handle_count = current_handle_count;
            if (previous_handle_count != 0 && current_handle_count > previous_handle_count) {
                sustained_growth += current_handle_count - previous_handle_count;
                if (sustained_growth > maximum_sustained_growth) maximum_sustained_growth = sustained_growth;
            } else if (previous_handle_count != 0 && current_handle_count < previous_handle_count) {
                sustained_growth = 0;
            }
            previous_handle_count = current_handle_count;
            final_handle_count = current_handle_count;
        }
    }

    ::acs::test::RecordInfo(::acs::FSourceLoc::Current(),
                            "FUdpTransport process HANDLE trend is informational: initial=%lu minimum=%lu maximum=%lu "
                            "final=%lu delta=%lld maximum_sustained_growth=%lu rounds=%u",
                            static_cast<unsigned long>(initial_handle_count),
                            static_cast<unsigned long>(minimum_handle_count),
                            static_cast<unsigned long>(maximum_handle_count),
                            static_cast<unsigned long>(final_handle_count),
                            static_cast<long long>(final_handle_count) - static_cast<long long>(initial_handle_count),
                            static_cast<unsigned long>(maximum_sustained_growth), kMeasurementRounds);
    if (maximum_sustained_growth > kMaximumExpectedLazyHandleGrowth) {
        ::acs::test::RecordInfo(::acs::FSourceLoc::Current(),
                                "FUdpTransport sustained HANDLE growth exceeded lazy-runtime allowance: "
                                "growth=%lu allowance=%lu",
                                static_cast<unsigned long>(maximum_sustained_growth),
                                static_cast<unsigned long>(kMaximumExpectedLazyHandleGrowth));
        return false;
    }
    return true;
}

/** 同一 transport の公開操作を競合させるスレッド引数。 */
struct SharedTransportThreadContext {
    HANDLE start_event = nullptr;
    FUdpTransport* transport = nullptr;
    u32 iteration_count = 0;
    bool success = false;
};

/** 同一 transport へ Connect / query / Disconnect を反復する。 */
DWORD WINAPI SharedTransportThreadMain(void* user) noexcept
{
    auto* context = static_cast<SharedTransportThreadContext*>(user);
    if (context == nullptr || context->start_event == nullptr || context->transport == nullptr) return 1;
    if (::WaitForSingleObject(context->start_event, 10000) != WAIT_OBJECT_0) return 2;

    for (u32 i = 0; i < context->iteration_count; ++i) {
        auto connected = context->transport->Connect("127.0.0.1", 9);
        if (connected.IsErr()) return 3;
        const u8 payload = static_cast<u8>(i);
        (void)context->transport->Send(&payload, sizeof(payload));
        u8 receive_buffer[16] = {};
        (void)context->transport->Receive(receive_buffer, sizeof(receive_buffer));
        (void)context->transport->IsConnected();
        (void)context->transport->PendingBytesIn();
        context->transport->Disconnect();
        context->transport->Disconnect();
    }
    context->success = true;
    return 0;
}

/** 同一 FUdpTransport の状態競合を 1 周させ、明示 HANDLE を全て回収する。 */
bool RunConcurrentSharedUdpTransportRound(u32 iteration_count) noexcept
{
    constexpr DWORD kThreadCount = 4;
    FUdpTransport transport;
    HANDLE start_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (start_event == nullptr) return false;

    SharedTransportThreadContext contexts[kThreadCount] = {};
    HANDLE threads[kThreadCount] = {};
    bool succeeded = true;
    bool created_all = true;

    for (DWORD i = 0; i < kThreadCount; ++i) {
        contexts[i].start_event = start_event;
        contexts[i].transport = &transport;
        contexts[i].iteration_count = iteration_count;
        threads[i] = ::CreateThread(nullptr, 0, &SharedTransportThreadMain, &contexts[i], 0, nullptr);
        if (threads[i] == nullptr) {
            created_all = false;
            succeeded = false;
            break;
        }
    }

    if (::SetEvent(start_event) == FALSE) succeeded = false;

    bool all_threads_completed = false;
    if (created_all) {
        all_threads_completed = ::WaitForMultipleObjects(kThreadCount, threads, TRUE, 30000) == WAIT_OBJECT_0;
        if (!all_threads_completed) succeeded = false;
    }

    for (DWORD i = 0; i < kThreadCount; ++i) {
        if (threads[i] == nullptr) continue;
        if (!all_threads_completed && ::WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0) {
            succeeded = false;
        }
        DWORD exit_code = 0;
        if (::GetExitCodeThread(threads[i], &exit_code) == FALSE || exit_code != 0) succeeded = false;
        if (!contexts[i].success) succeeded = false;
        if (::CloseHandle(threads[i]) == FALSE) succeeded = false;
    }

    transport.Disconnect();
    if (::CloseHandle(start_event) == FALSE) succeeded = false;
    return succeeded;
}

/** Network の同時 Init / Shutdown をフェーズ単位で開始する共有状態。 */
struct NetworkLifecycleRaceControl {
    volatile LONG phase = 0;
    volatile LONG ready_count = 0;
    volatile LONG completed_count = 0;
    volatile LONG stop_requested = 0;
    u32 iteration_count = 0;
};

/** Init 側または Shutdown 側を担当するスレッドの引数。 */
struct NetworkLifecycleThreadContext {
    NetworkLifecycleRaceControl* control = nullptr;
    bool initialize = false;
    bool success = true;
};

/** 指定値以上になるまで Interlocked 読み取りで待機する。 */
bool WaitForAtomicValueAtLeast(volatile LONG* value, LONG expected, DWORD timeout_ms) noexcept
{
    const ULONGLONG started_at = ::GetTickCount64();
    while (::_InterlockedCompareExchange(value, 0, 0) < expected) {
        if ((::GetTickCount64() - started_at) >= timeout_ms) return false;
        ::SwitchToThread();
    }
    return true;
}

/** 公開された各フェーズで Init または Shutdown を 1 回実行する。 */
DWORD WINAPI NetworkLifecycleThreadMain(void* user) noexcept
{
    auto* context = static_cast<NetworkLifecycleThreadContext*>(user);
    if (context == nullptr || context->control == nullptr) return 1;

    NetworkLifecycleRaceControl& control = *context->control;
    ::_InterlockedIncrement(&control.ready_count);

    for (u32 iteration = 0; iteration < control.iteration_count; ++iteration) {
        const LONG target_phase = static_cast<LONG>(iteration + 1);
        while (::_InterlockedCompareExchange(&control.phase, 0, 0) < target_phase) {
            if (::_InterlockedCompareExchange(&control.stop_requested, 0, 0) != 0) return 0;
            ::SwitchToThread();
        }

        if (context->initialize) {
            auto initialized = Network::Init();
            if (initialized.IsErr()) context->success = false;
        } else {
            Network::Shutdown();
        }

        ::_InterlockedIncrement(&control.completed_count);
    }

    return 0;
}

} // namespace

ACS_TEST(Win32Resource, HotReloadUnwatchReturnsDirectoryHandle)
{
    char temporary_root[MAX_PATH] = {};
    const DWORD root_length = ::GetTempPathA(MAX_PATH, temporary_root);
    EXPECT_TRUE(root_length > 0 && root_length < MAX_PATH);

    char directory[MAX_PATH] = {};
    std::snprintf(directory, sizeof(directory), "%sacs_hot_reload_%lu", temporary_root,
                  static_cast<unsigned long>(::GetCurrentProcessId()));
    (void)::RemoveDirectoryA(directory);
    EXPECT_TRUE(::CreateDirectoryA(directory, nullptr) != FALSE);

    HotReloadWatcher watcher;
    watcher.Init();
    const DWORD before = ProcessHandleCount();
    EXPECT_TRUE(before > 0);

    // ReadDirectoryChangesW 発行中に解除する経路を繰り返し、CancelIoEx の
    // 完了待ちと CloseHandle が毎回釣り合うことを確認する。
    for (u32 i = 0; i < 16; ++i) {
        watcher.WatchDirectory(directory, true);
        EXPECT_EQ(watcher.WatchedCount(), 1u);
        watcher.Unwatch(directory);
        EXPECT_EQ(watcher.WatchedCount(), 0u);
    }

    watcher.Shutdown();
    const DWORD after = ProcessHandleCount();
    EXPECT_EQ(after, before);
    EXPECT_TRUE(::RemoveDirectoryA(directory) != FALSE);
}

ACS_TEST(Win32Resource, CompletedBuildReleasesProcessHandle)
{
    FLocalBuildRunner runner;
    IBuildFarmBackend::BuildRequest request{};
    request.preset = "cmd.exe /D /C exit 0";
    request.branch = "local";
    request.commit_sha = "test";

    // 初回 CreateProcessW では OS / セキュリティ製品側の遅延初期化により、
    // プロセス寿命の内部 HANDLE が増える場合がある。同期経路を一度通してから
    // 基準値を取り、SubmitBuild 固有のプロセス HANDLE だけを測る。
    u32 warmup_exit_code = 1;
    auto warmup = runner.RunBuild(L"cmd.exe /D /C exit 0", warmup_exit_code);
    EXPECT_TRUE(warmup.IsOk());
    EXPECT_EQ(warmup_exit_code, 0u);

    const DWORD before = ProcessHandleCount();
    EXPECT_TRUE(before > 0);

    auto submitted = runner.SubmitBuild(request);
    EXPECT_TRUE(submitted.IsOk());
    if (submitted.IsErr()) return;

    EXPECT_TRUE(WaitForBuild(runner, submitted.Value()));
    // PollBuild が終了コードをラッチした時点で、プロセス HANDLE は不要になる。
    EXPECT_EQ(ProcessHandleCount(), before);
}

ACS_TEST(Win32Resource, NetworkAndUdpSocketReferencesAreBalanced)
{
    // 初回 Winsock 起動時の OS 内部遅延初期化を測定外へ出す。
    auto warmup_init = Network::Init();
    EXPECT_TRUE(warmup_init.IsOk());
    if (warmup_init.IsErr()) return;
    {
        auto warmup_socket = FUdpSocket::Bind(IpAddress::Loopback(), 0);
        EXPECT_TRUE(warmup_socket.IsOk());
    }
    Network::Shutdown();

    const DWORD before = ProcessHandleCount();
    for (u32 i = 0; i < 64; ++i) {
        auto initialized = Network::Init();
        EXPECT_TRUE(initialized.IsOk());
        if (initialized.IsErr()) break;
        {
            auto socket = FUdpSocket::Bind(IpAddress::Loopback(), 0);
            EXPECT_TRUE(socket.IsOk());
            if (socket.IsOk()) {
                socket.Value().Close();
                socket.Value().Close(); // 多重 Close でも二重解放しない。
            }
        }
        Network::Shutdown();
        EXPECT_FALSE(Network::IsInitialized());
    }
    const DWORD after = ProcessHandleCount();
    if (after != before) {
        ::acs::test::RecordInfo(::acs::FSourceLoc::Current(),
                                "Network/FUdpSocket handle count: before=%lu after=%lu delta=%lld",
                                static_cast<unsigned long>(before), static_cast<unsigned long>(after),
                                static_cast<long long>(after) - static_cast<long long>(before));
    }
    EXPECT_EQ(after, before);
    const NetworkDiagnostics diagnostics = Network::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.initialization_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 0ull);
}

ACS_TEST(Win32Resource, ConcurrentUdpTransportReferencesAreBalanced)
{
    EXPECT_TRUE(ValidateConcurrentUdpTransportOwnership());

    const UdpTransportDiagnostics after_diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(after_diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(after_diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(after_diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(after_diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(after_diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(after_diagnostics.resource_release_failure_count, 0ull);
}

ACS_TEST(Win32Resource, ConcurrentSharedUdpTransportStateIsBalanced)
{
    constexpr u32 kMeasurementRounds = 8;
    constexpr DWORD kMaximumExpectedLazyHandleGrowth = 4;

    EXPECT_TRUE(RunConcurrentSharedUdpTransportRound(100));
    const DWORD initial_handle_count = ProcessHandleCount();
    EXPECT_TRUE(initial_handle_count > 0);
    DWORD minimum_handle_count = initial_handle_count;
    DWORD maximum_handle_count = initial_handle_count;
    DWORD previous_handle_count = initial_handle_count;
    DWORD sustained_growth = 0;
    DWORD maximum_sustained_growth = 0;

    for (u32 round = 0; round < kMeasurementRounds; ++round) {
        EXPECT_TRUE(RunConcurrentSharedUdpTransportRound(100));
        const DWORD handle_count = ProcessHandleCount();
        if (handle_count != 0) {
            if (handle_count < minimum_handle_count) minimum_handle_count = handle_count;
            if (handle_count > maximum_handle_count) maximum_handle_count = handle_count;
            if (previous_handle_count != 0 && handle_count > previous_handle_count) {
                sustained_growth += handle_count - previous_handle_count;
                if (sustained_growth > maximum_sustained_growth) maximum_sustained_growth = sustained_growth;
            } else if (previous_handle_count != 0 && handle_count < previous_handle_count) {
                sustained_growth = 0;
            }
            previous_handle_count = handle_count;
        }

        const UdpTransportDiagnostics round_diagnostics = FUdpTransport::CaptureDiagnostics();
        EXPECT_TRUE(UdpTransportDiagnosticsAreClean(round_diagnostics));
    }

    ::acs::test::RecordInfo(::acs::FSourceLoc::Current(),
                            "shared FUdpTransport HANDLE trend: initial=%lu minimum=%lu maximum=%lu "
                            "maximum_sustained_growth=%lu rounds=%u",
                            static_cast<unsigned long>(initial_handle_count),
                            static_cast<unsigned long>(minimum_handle_count),
                            static_cast<unsigned long>(maximum_handle_count),
                            static_cast<unsigned long>(maximum_sustained_growth), kMeasurementRounds);
    EXPECT_TRUE(maximum_sustained_growth <= kMaximumExpectedLazyHandleGrowth);

    const UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 0ull);
}

ACS_TEST(Win32Resource, LiveUdpCleanupDebtCannotBeStolen)
{
    using namespace acs::game::internal;

    const bool reset_before = ResetUdpTransportDiagnosticsForTesting();
    EXPECT_TRUE(reset_before);
    if (!reset_before) return;

    UdpTransportFailureInjection injection{};
    injection.cleanup_failure_count = 1;
    EXPECT_TRUE(ConfigureUdpTransportFailureInjectionForTesting(injection));

    FUdpTransport owner;
    auto connected = owner.Connect("127.0.0.1", 9);
    EXPECT_TRUE(connected.IsOk());
    if (connected.IsErr()) return;

    owner.Disconnect();
    UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 1u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, injection.cleanup_error);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 1ull);

    FUdpTransport contender;
    auto contender_connected = contender.Connect("127.0.0.1", 9);
    EXPECT_TRUE(contender_connected.IsErr());
    if (contender_connected.IsErr()) {
        EXPECT_EQ(contender_connected.Error().subcode, static_cast<u16>(NetSnapshotError::kSub_CloseFailed));
    }

    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 1u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, injection.cleanup_error);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 1ull);

    owner.Disconnect();
    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 1ull);
    EXPECT_TRUE(ResetUdpTransportDiagnosticsForTesting());
}

ACS_TEST(Win32Resource, DestroyedUdpCleanupDebtIsExplicitlyDrained)
{
    using namespace acs::game::internal;

    const bool reset_before = ResetUdpTransportDiagnosticsForTesting();
    EXPECT_TRUE(reset_before);
    if (!reset_before) return;

    UdpTransportFailureInjection injection{};
    // destructor の初回失敗と自動 drain の再失敗を経て、明示 drain で回収する。
    injection.cleanup_failure_count = 2;
    EXPECT_TRUE(ConfigureUdpTransportFailureInjectionForTesting(injection));

    {
        FUdpTransport owner;
        auto connected = owner.Connect("127.0.0.1", 9);
        EXPECT_TRUE(connected.IsOk());
    }

    UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 1u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, injection.cleanup_error);
    EXPECT_TRUE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 2ull);

    auto drained = FUdpTransport::DrainDeferredResources();
    EXPECT_TRUE(drained.IsOk());

    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 2ull);
    EXPECT_TRUE(ResetUdpTransportDiagnosticsForTesting());
}

ACS_TEST(Win32Resource, DestroyedUdpSocketRemainsTrackedUntilExplicitDrain)
{
    using namespace acs::game::internal;

    const bool reset_before = ResetUdpTransportDiagnosticsForTesting();
    EXPECT_TRUE(reset_before);
    if (!reset_before) return;

    UdpTransportFailureInjection injection{};
    // destructor と自動 drain、最初の明示 drain を失敗させ、2 回目で回収する。
    injection.close_socket_failure_count = 3;
    EXPECT_TRUE(ConfigureUdpTransportFailureInjectionForTesting(injection));

    {
        FUdpTransport owner;
        auto connected = owner.Connect("127.0.0.1", 9);
        EXPECT_TRUE(connected.IsOk());
    }

    UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 1u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 1u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 2ull);

    auto first_drain = FUdpTransport::DrainDeferredResources();
    EXPECT_TRUE(first_drain.IsErr());
    if (first_drain.IsErr()) {
        EXPECT_EQ(first_drain.Error().subcode, static_cast<u16>(NetSnapshotError::kSub_CloseFailed));
    }
    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 1u);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 1u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 3ull);

    auto second_drain = FUdpTransport::DrainDeferredResources();
    EXPECT_TRUE(second_drain.IsOk());

    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 3ull);
    EXPECT_TRUE(ResetUdpTransportDiagnosticsForTesting());
}

ACS_TEST(Win32Resource, UdpOrphanRegistryOverflowRetainsEveryOwnershipToken)
{
    using namespace acs::game::internal;

    const bool reset_before = ResetUdpTransportDiagnosticsForTesting();
    EXPECT_TRUE(reset_before);
    if (!reset_before) return;

    UdpTransportFailureInjection injection{};
    injection.close_socket_failure_count = 5;
    injection.primary_orphaned_socket_capacity = 1;
    EXPECT_TRUE(ConfigureUdpTransportFailureInjectionForTesting(injection));

    {
        FUdpTransport first;
        FUdpTransport second;
        EXPECT_TRUE(first.Connect("127.0.0.1", 9).IsOk());
        EXPECT_TRUE(second.Connect("127.0.0.1", 9).IsOk());
    }

    UdpTransportDiagnostics diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 2u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 2u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 1u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 5ull);

    auto drained = FUdpTransport::DrainDeferredResources();
    EXPECT_TRUE(drained.IsOk());
    diagnostics = FUdpTransport::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.active_winsock_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_FALSE(diagnostics.cleanup_debt_orphaned);
    EXPECT_EQ(diagnostics.orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.overflow_orphaned_socket_count, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 5ull);
    EXPECT_TRUE(ResetUdpTransportDiagnosticsForTesting());
}

ACS_TEST(Win32Resource, UdpFailureInjectionRejectsTerminalOwnershipErrors)
{
    using namespace acs::game::internal;

    const bool reset_before = ResetUdpTransportDiagnosticsForTesting();
    EXPECT_TRUE(reset_before);
    if (!reset_before) return;

    UdpTransportFailureInjection close_injection{};
    close_injection.close_socket_failure_count = 1;
    close_injection.close_socket_error = 10038u; // WSAENOTSOCK
    EXPECT_FALSE(ConfigureUdpTransportFailureInjectionForTesting(close_injection));

    UdpTransportFailureInjection cleanup_injection{};
    cleanup_injection.cleanup_failure_count = 1;
    cleanup_injection.cleanup_error = 10093u; // WSANOTINITIALISED
    EXPECT_FALSE(ConfigureUdpTransportFailureInjectionForTesting(cleanup_injection));
    EXPECT_TRUE(ResetUdpTransportDiagnosticsForTesting());
}

ACS_TEST(Win32Resource, ConcurrentNetworkInitAndShutdownRemainBalanced)
{
    constexpr u32 kIterationCount = 512;

    EXPECT_FALSE(Network::IsInitialized());
    auto initialized = Network::Init();
    EXPECT_TRUE(initialized.IsOk());
    if (initialized.IsErr()) return;

    NetworkLifecycleRaceControl control{};
    control.iteration_count = kIterationCount;
    NetworkLifecycleThreadContext contexts[2] = {};
    contexts[0].control = &control;
    contexts[0].initialize = true;
    contexts[1].control = &control;
    contexts[1].initialize = false;

    HANDLE threads[2] = {};
    bool created_all = true;
    for (u32 i = 0; i < 2; ++i) {
        threads[i] = ::CreateThread(nullptr, 0, &NetworkLifecycleThreadMain, &contexts[i], 0, nullptr);
        if (threads[i] == nullptr) {
            created_all = false;
            break;
        }
    }

    bool lifecycle_balanced = created_all;
    if (created_all && !WaitForAtomicValueAtLeast(&control.ready_count, 2, 10000)) {
        lifecycle_balanced = false;
    }

    if (lifecycle_balanced) {
        for (u32 iteration = 0; iteration < kIterationCount; ++iteration) {
            ::_InterlockedExchange(&control.completed_count, 0);
            ::_InterlockedExchange(&control.phase, static_cast<LONG>(iteration + 1));
            if (!WaitForAtomicValueAtLeast(&control.completed_count, 2, 10000) || !Network::IsInitialized()) {
                lifecycle_balanced = false;
                break;
            }
        }
    }

    ::_InterlockedExchange(&control.stop_requested, 1);
    for (u32 i = 0; i < 2; ++i) {
        if (threads[i] == nullptr) continue;
        // context は stack 上にあるため、異常経路でも worker 終了前に破棄しない。
        if (::WaitForSingleObject(threads[i], INFINITE) != WAIT_OBJECT_0) lifecycle_balanced = false;
        if (!contexts[i].success) lifecycle_balanced = false;
        if (::CloseHandle(threads[i]) == FALSE) lifecycle_balanced = false;
    }

    EXPECT_TRUE(lifecycle_balanced);
    EXPECT_TRUE(Network::IsInitialized());
    Network::Shutdown();
    EXPECT_FALSE(Network::IsInitialized());

    // 途中失敗時も後続テストへ参照を残さない。
    for (u32 cleanup = 0; cleanup < 4 && Network::IsInitialized(); ++cleanup) {
        Network::Shutdown();
    }
    const NetworkDiagnostics diagnostics = Network::CaptureDiagnostics();
    EXPECT_EQ(diagnostics.initialization_reference_count, 0u);
    EXPECT_EQ(diagnostics.pending_cleanup_error, 0u);
    EXPECT_EQ(diagnostics.resource_release_failure_count, 0ull);
}
