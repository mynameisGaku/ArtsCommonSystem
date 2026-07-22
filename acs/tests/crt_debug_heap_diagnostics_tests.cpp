// SPDX-License-Identifier: Apache-2.0
#include "test/Test.h"
#include "test/Expect.h"
#include "foundation/Platform.h"
#include "memory/CrtDebugHeapDiagnostics.h"

#include <cstdlib>

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
#    include <crtdbg.h>
#endif

using namespace acs;

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
#    define ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE 1
#else
#    define ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE 0
#endif

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
namespace {

volatile LONG g_ReentrantReportHookEntered = 0;
volatile LONG g_ReentrantCheckWasRejected = 0;
volatile LONG g_ReentrantSettersPreservedConfiguration = 0;

int __cdecl ReentrantCrtReportHook(int, char*, int*)
{
    if (::InterlockedCompareExchange(&g_ReentrantReportHookEntered, 1, 0) != 0) {
        return TRUE;
    }

    const long break_allocation_before = _crtBreakAlloc;
    const int debug_flags_before = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const bool exit_check_before = (debug_flags_before & _CRTDBG_LEAK_CHECK_DF) != 0;

    if (!FCrtDebugHeapDiagnostics::CheckHeapIntegrity()) {
        (void)::InterlockedExchange(&g_ReentrantCheckWasRejected, 1);
    }
    (void)FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(17007);
    (void)FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(!exit_check_before);
    FCrtDebugHeapDiagnostics::DumpAllLiveObjects();

    const bool configuration_preserved = _crtBreakAlloc == break_allocation_before &&
                                         _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) == debug_flags_before;
    if (configuration_preserved) {
        (void)::InterlockedExchange(&g_ReentrantSettersPreservedConfiguration, 1);
    }
    return TRUE;
}

struct FConcurrentProcessConfigurationEndContext {
    FCrtDebugHeapProcessConfigurationScope* process_configuration = nullptr;
    HANDLE start_event = nullptr;
};

struct FConcurrentHeapCheckContext {
    HANDLE StartEvent = nullptr;
    volatile LONG* FailureCount = nullptr;
};

DWORD WINAPI RunConcurrentHeapChecks(void* OpaqueContext)
{
    auto* const Context = static_cast<FConcurrentHeapCheckContext*>(OpaqueContext);
    if (!Context || !Context->StartEvent || !Context->FailureCount) return 1u;
    if (::WaitForSingleObject(Context->StartEvent, 5000u) != WAIT_OBJECT_0) return 2u;

    for (usize Iteration = 0u; Iteration < 200u; ++Iteration) {
        if (!FCrtDebugHeapDiagnostics::CheckHeapIntegrity()) {
            (void)::InterlockedIncrement(Context->FailureCount);
        }
    }
    return 0u;
}

DWORD WINAPI EndProcessConfigurationOnWorker(void* opaque_context)
{
    auto* context = static_cast<FConcurrentProcessConfigurationEndContext*>(opaque_context);
    if (!context || !context->process_configuration || !context->start_event) {
        return 1u;
    }
    if (::WaitForSingleObject(context->start_event, 5000u) != WAIT_OBJECT_0) {
        return 2u;
    }

    context->process_configuration->End();
    return 0u;
}

} // namespace
#endif

ACS_TEST(CrtDebugHeapDiagnostics, ReportsBuildSupport)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_TRUE(FCrtDebugHeapDiagnostics::IsSupported());
#else
    EXPECT_FALSE(FCrtDebugHeapDiagnostics::IsSupported());
#endif
    EXPECT_TRUE(FCrtDebugHeapDiagnostics::CheckHeapIntegrity());
}

ACS_TEST(CrtDebugHeapDiagnostics, ConcurrentHeapChecksAreSerializedSafely)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    constexpr usize kThreadCount = 8u;
    HANDLE StartEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE Workers[kThreadCount] = {};
    FConcurrentHeapCheckContext Contexts[kThreadCount] = {};
    volatile LONG FailureCount = 0;

    EXPECT_TRUE(StartEvent != nullptr);
    for (usize Index = 0u; Index < kThreadCount; ++Index) {
        Contexts[Index].StartEvent = StartEvent;
        Contexts[Index].FailureCount = &FailureCount;
        Workers[Index] = StartEvent
            ? ::CreateThread(nullptr, 0u, RunConcurrentHeapChecks, &Contexts[Index], 0u, nullptr)
            : nullptr;
        EXPECT_TRUE(Workers[Index] != nullptr);
    }

    if (StartEvent) (void)::SetEvent(StartEvent);
    for (HANDLE Worker : Workers) {
        if (!Worker) continue;
        const DWORD WaitResult = ::WaitForSingleObject(Worker, 30000u);
        DWORD ExitCode = 3u;
        if (WaitResult == WAIT_OBJECT_0) (void)::GetExitCodeThread(Worker, &ExitCode);
        EXPECT_EQ(WaitResult, WAIT_OBJECT_0);
        EXPECT_EQ(ExitCode, 0u);
        (void)::CloseHandle(Worker);
    }
    if (StartEvent) (void)::CloseHandle(StartEvent);
    EXPECT_EQ(::InterlockedCompareExchange(&FailureCount, 0, 0), 0l);
#else
    EXPECT_TRUE(true);
#endif
}

ACS_TEST(CrtDebugHeapDiagnostics, ScopeRejectsMultipleBeginsAndCanBeReused)
{
    FCrtDebugHeapScopeConfiguration configuration{};
    configuration.ScopeName = "reuse contract";
    configuration.bWriteMachineReadableLog = false;

    FCrtDebugHeapScope scope;
    EXPECT_TRUE(scope.Begin(configuration));
    EXPECT_FALSE(scope.Begin(configuration));
    EXPECT_TRUE(scope.IsActive());

    const FCrtDebugHeapScopeReport first = scope.End();
    EXPECT_TRUE(first.bWasActive);
    EXPECT_FALSE(first.bLeakDetected);
    EXPECT_FALSE(scope.IsActive());

    const FCrtDebugHeapScopeReport inactive = scope.End();
    EXPECT_FALSE(inactive.bWasActive);

    EXPECT_TRUE(scope.Begin(configuration));
    const FCrtDebugHeapScopeReport second = scope.End();
    EXPECT_TRUE(second.bWasActive);
}

ACS_TEST(CrtDebugHeapDiagnostics, DetectsTransientLeakWithoutLeavingItAlive)
{
    FCrtDebugHeapScopeConfiguration configuration{};
    configuration.ScopeName = "transient_leak";
    configuration.bCheckHeapIntegrity = true;
    configuration.bWriteMachineReadableLog = false;

    FCrtDebugHeapScope scope;
    EXPECT_TRUE(scope.Begin(configuration));
    void* const allocation = std::malloc(73u);
    const FCrtDebugHeapScopeReport report = scope.End();

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_TRUE(allocation != nullptr);
    EXPECT_TRUE(report.bSupported);
    EXPECT_TRUE(report.bMeasurementConclusive);
    EXPECT_TRUE(report.bConfigurationStable);
    EXPECT_TRUE(report.bAllocationTrackingEnabled);
    EXPECT_TRUE(report.bHeapValid);
    EXPECT_TRUE(report.bDifferenceDetected);
    EXPECT_TRUE(report.bLeakDetected);
    EXPECT_TRUE(report.OutstandingAllocationCount >= 1u);
    EXPECT_TRUE(report.OutstandingBytes >= 73u);
#else
    EXPECT_FALSE(report.bSupported);
    EXPECT_FALSE(report.bMeasurementConclusive);
    EXPECT_FALSE(report.bDifferenceDetected);
    EXPECT_FALSE(report.bLeakDetected);
#endif

    // 親テストプロセスに実リークを残さず、検出した割り当てだけを必ず回収する。
    std::free(allocation);
}

ACS_TEST(CrtDebugHeapDiagnostics, ProcessConfigurationRestoresEveryCrtSetting)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int original_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const long original_break_allocation = _crtBreakAlloc;
    int original_report_modes[_CRT_ERRCNT]{};
    _HFILE original_report_files[_CRT_ERRCNT]{};
    for (int report_type = 0; report_type < _CRT_ERRCNT; ++report_type) {
        original_report_modes[report_type] = _CrtSetReportMode(report_type, _CRTDBG_REPORT_MODE);
        original_report_files[report_type] = _CrtSetReportFile(report_type, _CRTDBG_REPORT_FILE);
    }
#endif

    FCrtDebugHeapProcessConfiguration configuration{};
    configuration.bEnableProcessExitLeakCheck = true;
    configuration.bRetainFreedBlocks = false;
    configuration.bIncludeCrtBlocks = true;
    configuration.CheckFrequency = ECrtDebugHeapCheckFrequency::Every128Operations;
    configuration.bReportToDebugger = true;
    configuration.bReportToStandardError = true;
    configuration.bConfigureBreakOnAllocationSequence = true;
    configuration.BreakOnAllocationSequence = 17001;

    FCrtDebugHeapProcessConfigurationScope process_configuration;
    EXPECT_TRUE(process_configuration.Begin(configuration));
    EXPECT_FALSE(process_configuration.Begin(configuration));

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int applied_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    EXPECT_TRUE((applied_flags & _CRTDBG_ALLOC_MEM_DF) != 0);
    EXPECT_TRUE((applied_flags & _CRTDBG_LEAK_CHECK_DF) != 0);
    EXPECT_TRUE((applied_flags & _CRTDBG_CHECK_CRT_DF) != 0);
    EXPECT_TRUE((applied_flags & _CRTDBG_CHECK_EVERY_128_DF) != 0);
    EXPECT_EQ(_crtBreakAlloc, 17001l);
#endif

    process_configuration.End();
    EXPECT_FALSE(process_configuration.IsActive());

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_EQ(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG), original_flags);
    EXPECT_EQ(_crtBreakAlloc, original_break_allocation);
    for (int report_type = 0; report_type < _CRT_ERRCNT; ++report_type) {
        EXPECT_EQ(_CrtSetReportMode(report_type, _CRTDBG_REPORT_MODE), original_report_modes[report_type]);
        EXPECT_EQ(_CrtSetReportFile(report_type, _CRTDBG_REPORT_FILE), original_report_files[report_type]);
    }
#endif

    // 復元後は同じインスタンスへ再適用できる。
    EXPECT_TRUE(process_configuration.Begin(configuration));
    process_configuration.End();
}

ACS_TEST(CrtDebugHeapDiagnostics, ProcessConfigurationRejectsConcurrentOwner)
{
    FCrtDebugHeapProcessConfiguration configuration{};
    FCrtDebugHeapProcessConfigurationScope first;
    FCrtDebugHeapProcessConfigurationScope second;

    EXPECT_TRUE(first.Begin(configuration));
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_FALSE(second.Begin(configuration));
#else
    // no-op ビルドではプロセス共有状態を変更しないため独立に開始できる。
    EXPECT_TRUE(second.Begin(configuration));
    second.End();
#endif
    first.End();

    EXPECT_TRUE(second.Begin(configuration));
    second.End();
}

ACS_TEST(CrtDebugHeapDiagnostics, RejectsExitLeakCheckWithoutAllocationTracking)
{
    FCrtDebugHeapProcessConfiguration configuration{};
    configuration.bEnableAllocationTracking = false;
    configuration.bEnableProcessExitLeakCheck = true;
    FCrtDebugHeapProcessConfigurationScope process_configuration;
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_FALSE(process_configuration.Begin(configuration));
#else
    EXPECT_TRUE(process_configuration.Begin(configuration));
    process_configuration.End();
#endif
}

ACS_TEST(CrtDebugHeapDiagnostics, DirectSettersDoNotOverrideActiveConfigurationScope)
{
    FCrtDebugHeapProcessConfiguration configuration{};
    configuration.bConfigureBreakOnAllocationSequence = true;
    configuration.BreakOnAllocationSequence = 17003;
    configuration.bEnableProcessExitLeakCheck = false;
    FCrtDebugHeapProcessConfigurationScope process_configuration;
    EXPECT_TRUE(process_configuration.Begin(configuration));

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_EQ(FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(17004), 17003);
    EXPECT_EQ(_crtBreakAlloc, 17003l);
    EXPECT_FALSE(FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(true));
    EXPECT_TRUE((_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) & _CRTDBG_LEAK_CHECK_DF) == 0);
#endif
    process_configuration.End();
}

ACS_TEST(CrtDebugHeapDiagnostics, DirectProcessSettingsCanBeRestored)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int original_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const bool original_exit_check = (original_flags & _CRTDBG_LEAK_CHECK_DF) != 0;
    const long original_break_allocation = _crtBreakAlloc;

    const bool previous_exit_check = FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(!original_exit_check);
    EXPECT_EQ(previous_exit_check, original_exit_check);
    const int changed_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    EXPECT_EQ((changed_flags & _CRTDBG_LEAK_CHECK_DF) != 0, !original_exit_check);
    (void)FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(original_exit_check);

    const i64 previous_break_allocation = FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(17002);
    EXPECT_EQ(previous_break_allocation, static_cast<i64>(original_break_allocation));
    EXPECT_EQ(_crtBreakAlloc, 17002l);
    (void)FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(original_break_allocation);

    EXPECT_EQ(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG), original_flags);
    EXPECT_EQ(_crtBreakAlloc, original_break_allocation);
#else
    EXPECT_FALSE(FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(true));
    EXPECT_EQ(FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(1), -1);
#endif
}

ACS_TEST(CrtDebugHeapDiagnostics, DiagnosticScopeBlocksIndependentProcessSettingChanges)
{
    FCrtDebugHeapScopeConfiguration scope_configuration{};
    scope_configuration.ScopeName = "process_setting_exclusion";
    scope_configuration.bWriteMachineReadableLog = false;

    FCrtDebugHeapScope diagnostic_scope;
    EXPECT_TRUE(diagnostic_scope.Begin(scope_configuration));

    FCrtDebugHeapProcessConfiguration process_configuration_values{};
    FCrtDebugHeapProcessConfigurationScope process_configuration;
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int flags_before = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const bool exit_check_before = (flags_before & _CRTDBG_LEAK_CHECK_DF) != 0;
    const long break_allocation_before = _crtBreakAlloc;

    EXPECT_FALSE(process_configuration.Begin(process_configuration_values));
    EXPECT_EQ(FCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(17005), static_cast<i64>(break_allocation_before));
    EXPECT_EQ(_crtBreakAlloc, break_allocation_before);
    EXPECT_EQ(FCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(!exit_check_before), exit_check_before);
    EXPECT_EQ((_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) & _CRTDBG_LEAK_CHECK_DF) != 0, exit_check_before);
#else
    EXPECT_TRUE(process_configuration.Begin(process_configuration_values));
    process_configuration.End();
#endif

    (void)diagnostic_scope.End();
    EXPECT_TRUE(process_configuration.Begin(process_configuration_values));
    process_configuration.End();
}

ACS_TEST(CrtDebugHeapDiagnostics, ProcessConfigurationRestorationWaitsForDiagnosticScope)
{
    FCrtDebugHeapProcessConfiguration configuration{};
    configuration.bConfigureBreakOnAllocationSequence = true;
    configuration.BreakOnAllocationSequence = 17006;
    configuration.bReportToDebugger = true;
    configuration.bReportToStandardError = false;

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int original_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const long original_break_allocation = _crtBreakAlloc;
    configuration.bEnableProcessExitLeakCheck = (original_flags & _CRTDBG_LEAK_CHECK_DF) == 0;
#endif

    FCrtDebugHeapProcessConfigurationScope process_configuration;
    EXPECT_TRUE(process_configuration.Begin(configuration));

    FCrtDebugHeapScopeConfiguration scope_configuration{};
    scope_configuration.ScopeName = "deferred_process_restore";
    scope_configuration.bWriteMachineReadableLog = false;
    FCrtDebugHeapScope diagnostic_scope;
    EXPECT_TRUE(diagnostic_scope.Begin(scope_configuration));

    process_configuration.End();
    EXPECT_FALSE(process_configuration.IsActive());

    FCrtDebugHeapProcessConfigurationScope next_process_configuration;
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_EQ(_crtBreakAlloc, 17006l);
    EXPECT_EQ((_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG) & _CRTDBG_LEAK_CHECK_DF) != 0,
              configuration.bEnableProcessExitLeakCheck);
    EXPECT_FALSE(next_process_configuration.Begin(configuration));
#endif

    (void)diagnostic_scope.End();

#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    EXPECT_EQ(_crtBreakAlloc, original_break_allocation);
    EXPECT_EQ(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG), original_flags);
#endif
    EXPECT_TRUE(next_process_configuration.Begin(configuration));
    next_process_configuration.End();
}

ACS_TEST(CrtDebugHeapDiagnostics, DirectTrackingFlagChangeMakesMeasurementInconclusive)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int original_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    (void)_CrtSetDbgFlag(original_flags | _CRTDBG_ALLOC_MEM_DF);

    FCrtDebugHeapScopeConfiguration configuration{};
    configuration.ScopeName = "tracking_flag_changed";
    configuration.bWriteMachineReadableLog = true;
    FCrtDebugHeapScope diagnostic_scope;
    EXPECT_TRUE(diagnostic_scope.Begin(configuration));

    (void)_CrtSetDbgFlag((original_flags | _CRTDBG_ALLOC_MEM_DF) & ~_CRTDBG_ALLOC_MEM_DF);
    void* const untracked_allocation = std::malloc(41u);
    const FCrtDebugHeapScopeReport report = diagnostic_scope.End();
    (void)_CrtSetDbgFlag(original_flags);

    EXPECT_TRUE(untracked_allocation != nullptr);
    EXPECT_TRUE(report.bWasActive);
    EXPECT_FALSE(report.bConfigurationStable);
    EXPECT_FALSE(report.bAllocationTrackingEnabled);
    EXPECT_FALSE(report.bMeasurementConclusive);
    EXPECT_FALSE(report.bLeakDetected);
    std::free(untracked_allocation);
#else
    EXPECT_TRUE(true);
#endif
}

ACS_TEST(CrtDebugHeapDiagnostics, ReportHookReentryIsBoundedAndDoesNotMutateSettings)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    (void)::InterlockedExchange(&g_ReentrantReportHookEntered, 0);
    (void)::InterlockedExchange(&g_ReentrantCheckWasRejected, 0);
    (void)::InterlockedExchange(&g_ReentrantSettersPreservedConfiguration, 0);

    void* const live_allocation = std::malloc(53u);
    _CRT_REPORT_HOOK const previous_hook = _CrtSetReportHook(ReentrantCrtReportHook);
    FCrtDebugHeapDiagnostics::DumpAllLiveObjects();
    (void)_CrtSetReportHook(previous_hook);
    std::free(live_allocation);

    EXPECT_TRUE(live_allocation != nullptr);
    EXPECT_EQ(::InterlockedCompareExchange(&g_ReentrantReportHookEntered, 0, 0), 1l);
    EXPECT_EQ(::InterlockedCompareExchange(&g_ReentrantCheckWasRejected, 0, 0), 1l);
    EXPECT_EQ(::InterlockedCompareExchange(&g_ReentrantSettersPreservedConfiguration, 0, 0), 1l);
#else
    EXPECT_TRUE(true);
#endif
}

ACS_TEST(CrtDebugHeapDiagnostics, ConcurrentProcessEndAndDiagnosticEndRestoreSettings)
{
#if ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
    const int original_flags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const long original_break_allocation = _crtBreakAlloc;

    FCrtDebugHeapProcessConfiguration configuration{};
    configuration.bConfigureBreakOnAllocationSequence = true;
    configuration.BreakOnAllocationSequence = 17008;
    configuration.bReportToDebugger = false;
    configuration.bReportToStandardError = false;
    FCrtDebugHeapProcessConfigurationScope process_configuration;
    EXPECT_TRUE(process_configuration.Begin(configuration));

    HANDLE start_event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
    FConcurrentProcessConfigurationEndContext context{&process_configuration, start_event};
    HANDLE worker = start_event ? ::CreateThread(nullptr, 0u, EndProcessConfigurationOnWorker, &context, 0u, nullptr)
                                : nullptr;
    EXPECT_TRUE(start_event != nullptr);
    EXPECT_TRUE(worker != nullptr);

    FCrtDebugHeapScopeConfiguration scope_configuration{};
    scope_configuration.ScopeName = "concurrent_process_end";
    scope_configuration.bWriteMachineReadableLog = false;
    FCrtDebugHeapScope diagnostic_scope;
    EXPECT_TRUE(diagnostic_scope.Begin(scope_configuration));

    if (worker) {
        (void)::SetEvent(start_event);
    }
    const FCrtDebugHeapScopeReport report = diagnostic_scope.End();
    const DWORD wait_result = worker ? ::WaitForSingleObject(worker, 10000u) : WAIT_FAILED;
    DWORD worker_exit_code = 3u;
    if (worker && wait_result == WAIT_OBJECT_0) {
        (void)::GetExitCodeThread(worker, &worker_exit_code);
    }
    if (worker) {
        ::CloseHandle(worker);
    }
    if (start_event) {
        ::CloseHandle(start_event);
    }

    EXPECT_EQ(wait_result, WAIT_OBJECT_0);
    EXPECT_EQ(worker_exit_code, 0u);
    EXPECT_TRUE(report.bMeasurementConclusive);
    EXPECT_FALSE(process_configuration.IsActive());
    EXPECT_EQ(_crtBreakAlloc, original_break_allocation);
    EXPECT_EQ(_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG), original_flags);
#else
    EXPECT_TRUE(true);
#endif
}

#undef ACS_TEST_CRT_DEBUG_HEAP_AVAILABLE
