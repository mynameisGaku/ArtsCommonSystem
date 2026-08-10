// SPDX-License-Identifier: Apache-2.0
#include "memory/CrtDebugHeapDiagnostics.h"

#include "foundation/Platform.h"

#include <cstdio>
#include <cstring>
#include <new>

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
#    include <crtdbg.h>
#endif

namespace acs {
namespace {

constexpr usize kScopeNameCapacity = 64u;

// 標準出力へ書けなかったときに標準エラーへ送る固定結果です。
constexpr char kStandardOutputWriteFailedFallback[] =
    "[acs][memory] tracker=msvc_crt_debug_heap status=write_failed target=standard_output "
    "reason=standard_output_write_failed\n";
// 標準エラーへ書けなかったときにデバッガだけへ送る固定結果です。
constexpr char kStandardErrorWriteFailedFallback[] =
    "[acs][memory] tracker=msvc_crt_debug_heap status=write_failed target=standard_error "
    "reason=standard_error_write_failed\n";
// 機械行を構築できなかったときに標準エラーへ送る固定結果です。
constexpr char kDiagnosticFormatFailedFallback[] =
    "[acs][memory] tracker=msvc_crt_debug_heap status=write_failed target=diagnostic_format "
    "reason=diagnostic_format_failed\n";

// 指定ハンドルへ全量を書き、部分書込みや無効ハンドルを失敗にします。
bool WriteRawDiagnosticText(HANDLE StandardHandle, const char* Text, usize TextLength) noexcept
{
    if (!StandardHandle || StandardHandle == INVALID_HANDLE_VALUE || !Text) {
        return false;
    }

    usize WrittenTotal = 0u;
    while (WrittenTotal < TextLength) {
        const usize Remaining = TextLength - WrittenTotal;
        const DWORD ChunkSize = Remaining > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<DWORD>(Remaining);
        DWORD Written = 0u;
        if (!::WriteFile(StandardHandle, Text + WrittenTotal, ChunkSize, &Written, nullptr) || Written != ChunkSize) {
            return false;
        }
        WrittenTotal += Written;
    }
    return true;
}

// 結果をデバッガと成功・失敗に応じた標準ストリームへ送ります。
bool WriteDiagnosticLine(const char* Text, bool bFailure) noexcept
{
    if (!Text) {
        return false;
    }

    ::OutputDebugStringA(Text);
    const HANDLE StandardHandle = ::GetStdHandle(bFailure ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    return WriteRawDiagnosticText(StandardHandle, Text, std::strlen(Text));
}

// 出力先の失敗を固定文で通知し、標準エラーの再帰書込みを避けます。
void WriteDiagnosticFailureFallback(bool bFailure) noexcept
{
    const char* Fallback = bFailure ? kStandardErrorWriteFailedFallback : kStandardOutputWriteFailedFallback;
    ::OutputDebugStringA(Fallback);
    if (!bFailure) {
        const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
        (void)WriteRawDiagnosticText(StandardError, Fallback, std::strlen(Fallback));
    }
}

// 診断行の切断を成功結果として出さず固定文に置き換えます。
void WriteDiagnosticFormatFailureFallback() noexcept
{
    ::OutputDebugStringA(kDiagnosticFormatFailedFallback);
    const HANDLE StandardError = ::GetStdHandle(STD_ERROR_HANDLE);
    (void)WriteRawDiagnosticText(StandardError, kDiagnosticFormatFailedFallback,
                                 std::strlen(kDiagnosticFormatFailedFallback));
}

// 通常出力に失敗したときだけ固定フォールバックを送ります。
void EmitDiagnosticLine(const char* Text, bool bFailure) noexcept
{
    if (!WriteDiagnosticLine(Text, bFailure)) {
        WriteDiagnosticFailureFallback(bFailure);
    }
}

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER

struct FCrtDebugHeapScopeImplementation {
    _CrtMemState Baseline{};
    FCrtDebugHeapScopeConfiguration Configuration{};
    char ScopeName[kScopeNameCapacity]{};
    int DebugFlagsAtBegin = 0;
    bool bHeapValidAtBegin = true;
    bool bAllocationTrackingEnabled = false;
};

struct FCrtDebugHeapProcessConfigurationImplementation {
    int DebugFlags = 0;
    long BreakAllocation = -1;
    int ReportModes[_CRT_ERRCNT]{};
    _HFILE ReportFiles[_CRT_ERRCNT]{};
};

SRWLOCK g_ProcessConfigurationLock = SRWLOCK_INIT;
SRWLOCK g_ReportOperationLock = SRWLOCK_INIT;
void* g_ProcessConfigurationOwner = nullptr;
void* g_DiagnosticScopeOwner = nullptr;
FCrtDebugHeapProcessConfigurationImplementation g_PendingProcessConfigurationRestoration{};
bool g_ProcessConfigurationRestorationPending = false;
u32 g_ActiveReportCapableOperationCount = 0u;
thread_local bool g_ReportCapableOperationActiveOnCurrentThread = false;

void RestoreProcessConfiguration(const FCrtDebugHeapProcessConfigurationImplementation& Implementation) noexcept
{
    (void)_CrtSetDbgFlag(Implementation.DebugFlags);
    (void)_CrtSetBreakAlloc(Implementation.BreakAllocation);
    for (int ReportType = 0; ReportType < _CRT_ERRCNT; ++ReportType) {
        (void)_CrtSetReportFile(ReportType, Implementation.ReportFiles[ReportType]);
        (void)_CrtSetReportMode(ReportType, Implementation.ReportModes[ReportType]);
    }
}

void RestorePendingProcessConfigurationIfPossible() noexcept
{
    if (g_DiagnosticScopeOwner != nullptr || g_ActiveReportCapableOperationCount != 0u ||
        !g_ProcessConfigurationRestorationPending) {
        return;
    }

    RestoreProcessConfiguration(g_PendingProcessConfigurationRestoration);
    g_ProcessConfigurationRestorationPending = false;
}

bool ProcessSettingsAreProtected() noexcept
{
    return g_ProcessConfigurationOwner != nullptr || g_DiagnosticScopeOwner != nullptr ||
           g_ProcessConfigurationRestorationPending || g_ActiveReportCapableOperationCount != 0u;
}

bool TryBeginReportCapableOperation() noexcept
{
    if (g_ReportCapableOperationActiveOnCurrentThread) {
        return false;
    }

    // CRT の走査と report hook 呼び出しをプロセス内で直列化する。
    ::AcquireSRWLockExclusive(&g_ReportOperationLock);
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    ++g_ActiveReportCapableOperationCount;
    g_ReportCapableOperationActiveOnCurrentThread = true;
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
    return true;
}

void EndReportCapableOperation() noexcept
{
    if (!g_ReportCapableOperationActiveOnCurrentThread) {
        return;
    }

    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    g_ReportCapableOperationActiveOnCurrentThread = false;
    if (g_ActiveReportCapableOperationCount != 0u) {
        --g_ActiveReportCapableOperationCount;
    }
    RestorePendingProcessConfigurationIfPossible();
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
    ::ReleaseSRWLockExclusive(&g_ReportOperationLock);
}

template<typename T>
T* AllocateImplementation() noexcept
{
    void* const Storage = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(T));
    return Storage ? ::new (Storage) T{} : nullptr;
}

template<typename T>
void FreeImplementation(T*& Implementation) noexcept
{
    if (!Implementation) {
        return;
    }

    Implementation->~T();
    (void)::HeapFree(::GetProcessHeap(), 0, Implementation);
    Implementation = nullptr;
}

void CopySanitizedScopeName(char* Destination, usize Capacity, const char* Source) noexcept
{
    if (!Destination || Capacity == 0u) {
        return;
    }

    if (!Source || Source[0] == '\0') {
        Source = "unnamed";
    }

    usize Index = 0u;
    while (Index + 1u < Capacity && Source[Index] != '\0') {
        const char Character = Source[Index];
        const bool bAccepted = (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
                               (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' ||
                               Character == '.';
        Destination[Index] = bAccepted ? Character : '_';
        ++Index;
    }
    Destination[Index] = '\0';
}

u64 PositiveDifference(usize Current, usize Baseline) noexcept
{
    if (Current <= Baseline) {
        return 0u;
    }

    return static_cast<u64>(Current - Baseline);
}

void WriteScopeReport(const FCrtDebugHeapScopeImplementation& Implementation,
                      const FCrtDebugHeapScopeReport& Report) noexcept
{
    const char* LeakResult = Report.bMeasurementConclusive ? (Report.bLeakDetected ? "true" : "false") : "inconclusive";
    const char* DifferenceResult = Report.bMeasurementConclusive ? (Report.bDifferenceDetected ? "true" : "false")
                                                                 : "inconclusive";
    // 検査確定とリーク有無から出力先を選びます。
    const bool bFailure = !Report.bMeasurementConclusive || Report.bLeakDetected;
    const char* Status = !Report.bMeasurementConclusive ? "inconclusive" : (Report.bLeakDetected ? "leak_detected" : "ok");
    const char* Reason = "none";
    if (!Report.bAllocationTrackingEnabled) {
        Reason = "allocation_tracking_disabled";
    } else if (!Report.bConfigurationStable) {
        Reason = "crt_configuration_changed";
    } else if (!Report.bHeapValid) {
        Reason = "heap_invalid";
    }

    char Line[1024]{};
    const int Result = std::snprintf(
        Line, sizeof(Line),
        "[acs][memory] tracker=msvc_crt_debug_heap supported=true "
        "scope=%s measurement_conclusive=%s configuration_stable=%s allocation_tracking_enabled=%s "
        "leak_detected=%s status=%s reason=%s outstanding_allocations=%llu "
        "outstanding_bytes=%llu normal_allocations=%llu normal_bytes=%llu "
        "client_allocations=%llu client_bytes=%llu crt_allocations=%llu "
        "crt_bytes=%llu difference_detected=%s heap_valid=%s\n",
        Implementation.ScopeName, Report.bMeasurementConclusive ? "true" : "false",
        Report.bConfigurationStable ? "true" : "false", Report.bAllocationTrackingEnabled ? "true" : "false",
        LeakResult, Status, Reason,
        static_cast<unsigned long long>(Report.OutstandingAllocationCount),
        static_cast<unsigned long long>(Report.OutstandingBytes),
        static_cast<unsigned long long>(Report.NormalAllocationCount),
        static_cast<unsigned long long>(Report.NormalBytes),
        static_cast<unsigned long long>(Report.ClientAllocationCount),
        static_cast<unsigned long long>(Report.ClientBytes), static_cast<unsigned long long>(Report.CrtAllocationCount),
        static_cast<unsigned long long>(Report.CrtBytes), DifferenceResult, Report.bHeapValid ? "true" : "false");
    if (Result > 0 && static_cast<usize>(Result) < sizeof(Line)) {
        EmitDiagnosticLine(Line, bFailure);
    } else {
        WriteDiagnosticFormatFailureFallback();
    }
}

int CheckFrequencyFlags(ECrtDebugHeapCheckFrequency Frequency) noexcept
{
    switch (Frequency) {
    case ECrtDebugHeapCheckFrequency::Every16Operations:
        return _CRTDBG_CHECK_EVERY_16_DF;
    case ECrtDebugHeapCheckFrequency::Every128Operations:
        return _CRTDBG_CHECK_EVERY_128_DF;
    case ECrtDebugHeapCheckFrequency::Every1024Operations:
        return _CRTDBG_CHECK_EVERY_1024_DF;
    case ECrtDebugHeapCheckFrequency::EveryOperation:
        return _CRTDBG_CHECK_ALWAYS_DF;
    case ECrtDebugHeapCheckFrequency::Default:
    default:
        return _CRTDBG_CHECK_DEFAULT_DF;
    }
}

int ApplyBooleanFlag(int Flags, int Flag, bool bEnabled) noexcept
{
    return bEnabled ? (Flags | Flag) : (Flags & ~Flag);
}

long NormalizeBreakAllocationSequence(i64 AllocationSequence) noexcept
{
    if (AllocationSequence < -1) {
        return -1;
    }
    if (AllocationSequence > 0x7FFFFFFFll) {
        return 0x7FFFFFFFl;
    }
    return static_cast<long>(AllocationSequence);
}

#else

struct FCrtDebugHeapScopeImplementation {
    FCrtDebugHeapScopeConfiguration Configuration{};
    char ScopeName[kScopeNameCapacity]{};
};

struct FCrtDebugHeapProcessConfigurationImplementation {
    bool bUnused = false;
};

template<typename T>
T* AllocateImplementation() noexcept
{
    void* const Storage = ::HeapAlloc(::GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(T));
    return Storage ? ::new (Storage) T{} : nullptr;
}

template<typename T>
void FreeImplementation(T*& Implementation) noexcept
{
    if (!Implementation) {
        return;
    }

    Implementation->~T();
    (void)::HeapFree(::GetProcessHeap(), 0, Implementation);
    Implementation = nullptr;
}

void CopySanitizedScopeName(char* Destination, usize Capacity, const char* Source) noexcept
{
    if (!Destination || Capacity == 0u) {
        return;
    }

    if (!Source || Source[0] == '\0') {
        Source = "unnamed";
    }

    usize Index = 0u;
    while (Index + 1u < Capacity && Source[Index] != '\0') {
        const char Character = Source[Index];
        const bool bAccepted = (Character >= 'a' && Character <= 'z') || (Character >= 'A' && Character <= 'Z') ||
                               (Character >= '0' && Character <= '9') || Character == '_' || Character == '-' ||
                               Character == '.';
        Destination[Index] = bAccepted ? Character : '_';
        ++Index;
    }
    Destination[Index] = '\0';
}

const char* UnsupportedReason() noexcept
{
#    if ACS_ADDRESS_SANITIZER
    return "address_sanitizer_intercepts_allocations";
#    else
    return "unsupported_build";
#    endif
}

void WriteUnsupportedScopeReport(const FCrtDebugHeapScopeImplementation& Implementation) noexcept
{
    char Line[1024]{};
    const int Result = std::snprintf(Line, sizeof(Line),
                                     "[acs][memory] tracker=msvc_crt_debug_heap supported=false "
                                     "scope=%s measurement_performed=false measurement_conclusive=false "
                                     "configuration_stable=false allocation_tracking_enabled=false "
                                     "leak_detected=inconclusive status=unsupported reason=%s "
                                     "outstanding_allocations=0 outstanding_bytes=0 normal_allocations=0 "
                                     "normal_bytes=0 client_allocations=0 client_bytes=0 crt_allocations=0 "
                                     "crt_bytes=0 difference_detected=inconclusive heap_valid=inconclusive\n",
                                     Implementation.ScopeName, UnsupportedReason());
    if (Result <= 0 || static_cast<usize>(Result) >= sizeof(Line)) {
        WriteDiagnosticFormatFailureFallback();
        return;
    }

    EmitDiagnosticLine(Line, true);
}

#endif

} // namespace

FCrtDebugHeapScope::~FCrtDebugHeapScope() noexcept
{
    (void)End();

    auto* Implementation = static_cast<FCrtDebugHeapScopeImplementation*>(m_Implementation);
    FreeImplementation(Implementation);
    m_Implementation = nullptr;
}

bool FCrtDebugHeapScope::Begin(const FCrtDebugHeapScopeConfiguration& Configuration) noexcept
{
    if (m_Active) {
        return false;
    }

    auto* Implementation = static_cast<FCrtDebugHeapScopeImplementation*>(m_Implementation);
    if (!Implementation) {
        Implementation = AllocateImplementation<FCrtDebugHeapScopeImplementation>();
        if (!Implementation) {
            return false;
        }
        m_Implementation = Implementation;
    }

    Implementation->Configuration = Configuration;
    CopySanitizedScopeName(Implementation->ScopeName, sizeof(Implementation->ScopeName), Configuration.ScopeName);

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    if (g_DiagnosticScopeOwner != nullptr || g_ProcessConfigurationRestorationPending ||
        g_ActiveReportCapableOperationCount != 0u) {
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return false;
    }

    const int DebugFlags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    Implementation->bAllocationTrackingEnabled = (DebugFlags & _CRTDBG_ALLOC_MEM_DF) != 0;
    if (!Implementation->bAllocationTrackingEnabled) {
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return false;
    }
    Implementation->DebugFlagsAtBegin = DebugFlags;
    g_DiagnosticScopeOwner = Implementation;
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);

    if (!TryBeginReportCapableOperation()) {
        ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
        if (g_DiagnosticScopeOwner == Implementation) {
            g_DiagnosticScopeOwner = nullptr;
            RestorePendingProcessConfigurationIfPossible();
        }
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return false;
    }
    Implementation->bHeapValidAtBegin = !Configuration.bCheckHeapIntegrity || _CrtCheckMemory() != 0;
    _CrtMemCheckpoint(&Implementation->Baseline);
    EndReportCapableOperation();
#endif

    m_Active = true;
    return true;
}

FCrtDebugHeapScopeReport FCrtDebugHeapScope::End() noexcept
{
    FCrtDebugHeapScopeReport Report{};
    Report.bSupported = CCrtDebugHeapDiagnostics::IsSupported();
    if (!m_Active) {
        return Report;
    }

    Report.bWasActive = true;
    m_Active = false;
    auto* Implementation = static_cast<FCrtDebugHeapScopeImplementation*>(m_Implementation);
    if (!Implementation) {
        Report.bHeapValid = false;
        return Report;
    }

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    if (g_DiagnosticScopeOwner != Implementation) {
        Report.bHeapValid = false;
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return Report;
    }
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);

    if (TryBeginReportCapableOperation()) {
        const int DebugFlagsBeforeCheckpoint = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        _CrtMemState Current{};
        _CrtMemState Difference{};
        _CrtMemCheckpoint(&Current);
        Report.bDifferenceDetected = _CrtMemDifference(&Difference, &Implementation->Baseline, &Current) != 0;
        const bool bHeapValidAtEnd = !Implementation->Configuration.bCheckHeapIntegrity || _CrtCheckMemory() != 0;
        const int DebugFlagsAfterCheckpoint = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);

        Report.bConfigurationStable = DebugFlagsBeforeCheckpoint == Implementation->DebugFlagsAtBegin &&
                                      DebugFlagsAfterCheckpoint == Implementation->DebugFlagsAtBegin;
        Report.bAllocationTrackingEnabled = Implementation->bAllocationTrackingEnabled &&
                                            (DebugFlagsBeforeCheckpoint & _CRTDBG_ALLOC_MEM_DF) != 0 &&
                                            (DebugFlagsAfterCheckpoint & _CRTDBG_ALLOC_MEM_DF) != 0;
        Report.bHeapValid = Implementation->bHeapValidAtBegin && bHeapValidAtEnd;

        Report.NormalAllocationCount = PositiveDifference(Current.lCounts[_NORMAL_BLOCK],
                                                          Implementation->Baseline.lCounts[_NORMAL_BLOCK]);
        Report.NormalBytes = PositiveDifference(Current.lSizes[_NORMAL_BLOCK],
                                                Implementation->Baseline.lSizes[_NORMAL_BLOCK]);
        Report.ClientAllocationCount = PositiveDifference(Current.lCounts[_CLIENT_BLOCK],
                                                          Implementation->Baseline.lCounts[_CLIENT_BLOCK]);
        Report.ClientBytes = PositiveDifference(Current.lSizes[_CLIENT_BLOCK],
                                                Implementation->Baseline.lSizes[_CLIENT_BLOCK]);
        Report.CrtAllocationCount = PositiveDifference(Current.lCounts[_CRT_BLOCK],
                                                       Implementation->Baseline.lCounts[_CRT_BLOCK]);
        Report.CrtBytes = PositiveDifference(Current.lSizes[_CRT_BLOCK], Implementation->Baseline.lSizes[_CRT_BLOCK]);

        Report.OutstandingAllocationCount = Report.NormalAllocationCount + Report.ClientAllocationCount;
        Report.OutstandingBytes = Report.NormalBytes + Report.ClientBytes;
        if (Implementation->Configuration.bIncludeCrtBlocksInLeakResult) {
            Report.OutstandingAllocationCount += Report.CrtAllocationCount;
            Report.OutstandingBytes += Report.CrtBytes;
        }
        Report.bMeasurementConclusive = Report.bConfigurationStable && Report.bAllocationTrackingEnabled &&
                                        Report.bHeapValid;
        Report.bLeakDetected = Report.bMeasurementConclusive &&
                               (Report.OutstandingAllocationCount != 0u || Report.OutstandingBytes != 0u);

        if (Report.bLeakDetected && Implementation->Configuration.bDumpObjectsOnLeak) {
            _CrtMemDumpAllObjectsSince(&Implementation->Baseline);
        }
        EndReportCapableOperation();
    } else {
        Report.bHeapValid = false;
    }

    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    g_DiagnosticScopeOwner = nullptr;
    RestorePendingProcessConfigurationIfPossible();
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);

    if (Implementation->Configuration.bWriteMachineReadableLog) {
        WriteScopeReport(*Implementation, Report);
    }
#else
    if (Implementation->Configuration.bWriteMachineReadableLog) {
        WriteUnsupportedScopeReport(*Implementation);
    }
#endif

    return Report;
}

FCrtDebugHeapProcessConfigurationScope::~FCrtDebugHeapProcessConfigurationScope() noexcept
{
    End();

    auto* Implementation = static_cast<FCrtDebugHeapProcessConfigurationImplementation*>(m_Implementation);
    FreeImplementation(Implementation);
    m_Implementation = nullptr;
}

bool FCrtDebugHeapProcessConfigurationScope::Begin(const FCrtDebugHeapProcessConfiguration& Configuration) noexcept
{
    if (m_Active) {
        return false;
    }

    auto* Implementation = static_cast<FCrtDebugHeapProcessConfigurationImplementation*>(m_Implementation);
    if (!Implementation) {
        Implementation = AllocateImplementation<FCrtDebugHeapProcessConfigurationImplementation>();
        if (!Implementation) {
            return false;
        }
        m_Implementation = Implementation;
    }

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (!Configuration.bEnableAllocationTracking && Configuration.bEnableProcessExitLeakCheck) {
        return false;
    }
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    if (ProcessSettingsAreProtected()) {
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return false;
    }
    g_ProcessConfigurationOwner = Implementation;

    Implementation->DebugFlags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    Implementation->BreakAllocation = _crtBreakAlloc;
    for (int ReportType = 0; ReportType < _CRT_ERRCNT; ++ReportType) {
        Implementation->ReportModes[ReportType] = _CrtSetReportMode(ReportType, _CRTDBG_REPORT_MODE);
        Implementation->ReportFiles[ReportType] = _CrtSetReportFile(ReportType, _CRTDBG_REPORT_FILE);
    }

    int NewFlags = Implementation->DebugFlags;
    NewFlags = ApplyBooleanFlag(NewFlags, _CRTDBG_ALLOC_MEM_DF, Configuration.bEnableAllocationTracking);
    NewFlags = ApplyBooleanFlag(NewFlags, _CRTDBG_LEAK_CHECK_DF, Configuration.bEnableProcessExitLeakCheck);
    NewFlags = ApplyBooleanFlag(NewFlags, _CRTDBG_DELAY_FREE_MEM_DF, Configuration.bRetainFreedBlocks);
    NewFlags = ApplyBooleanFlag(NewFlags, _CRTDBG_CHECK_CRT_DF, Configuration.bIncludeCrtBlocks);
    NewFlags &= ~(_CRTDBG_CHECK_ALWAYS_DF | _CRTDBG_CHECK_EVERY_16_DF | _CRTDBG_CHECK_EVERY_128_DF |
                  _CRTDBG_CHECK_EVERY_1024_DF);
    NewFlags |= CheckFrequencyFlags(Configuration.CheckFrequency);
    (void)_CrtSetDbgFlag(NewFlags);

    int ReportMode = 0;
    if (Configuration.bReportToDebugger) {
        ReportMode |= _CRTDBG_MODE_DEBUG;
    }
    if (Configuration.bReportToStandardError) {
        ReportMode |= _CRTDBG_MODE_FILE;
    }
    for (int ReportType = 0; ReportType < _CRT_ERRCNT; ++ReportType) {
        if (Configuration.bReportToStandardError) {
            (void)_CrtSetReportFile(ReportType, _CRTDBG_FILE_STDERR);
        }
        (void)_CrtSetReportMode(ReportType, ReportMode);
    }

    if (Configuration.bConfigureBreakOnAllocationSequence) {
        (void)_CrtSetBreakAlloc(NormalizeBreakAllocationSequence(Configuration.BreakOnAllocationSequence));
    }
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
#else
    (void)Configuration;
#endif

    m_Active = true;
    return true;
}

void FCrtDebugHeapProcessConfigurationScope::End() noexcept
{
    if (!m_Active) {
        return;
    }
    m_Active = false;

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    auto* Implementation = static_cast<FCrtDebugHeapProcessConfigurationImplementation*>(m_Implementation);
    if (!Implementation) {
        return;
    }

    // 診断スコープのチェックポイント中は CRT 設定を固定し、復元だけを安全に遅延させる。
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    if (g_ProcessConfigurationOwner == Implementation) {
        if (g_DiagnosticScopeOwner != nullptr || g_ActiveReportCapableOperationCount != 0u) {
            g_PendingProcessConfigurationRestoration = *Implementation;
            g_ProcessConfigurationRestorationPending = true;
        } else {
            RestoreProcessConfiguration(*Implementation);
        }
        g_ProcessConfigurationOwner = nullptr;
    }
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
#endif
}

bool CCrtDebugHeapDiagnostics::IsSupported() noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    return true;
#else
    return false;
#endif
}

bool CCrtDebugHeapDiagnostics::CheckHeapIntegrity() noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (!TryBeginReportCapableOperation()) {
        return false;
    }
    const bool bHeapValid = _CrtCheckMemory() != 0;
    EndReportCapableOperation();
    return bHeapValid;
#else
    return true;
#endif
}

void CCrtDebugHeapDiagnostics::DumpAllLiveObjects() noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (!TryBeginReportCapableOperation()) {
        return;
    }
    _CrtMemDumpAllObjectsSince(nullptr);
    EndReportCapableOperation();
#endif
}

FCrtDebugHeapProcessLeakReport CCrtDebugHeapDiagnostics::DumpProcessMemoryLeaks(
    bool bWriteMachineReadableLog) noexcept
{
    FCrtDebugHeapProcessLeakReport Report{};
    Report.bSupported = IsSupported();

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (TryBeginReportCapableOperation()) {
        Report.bLeakDetected = _CrtDumpMemoryLeaks() != 0;
        Report.bInspectionSucceeded = true;
        EndReportCapableOperation();
    }

    if (bWriteMachineReadableLog) {
        char Line[384]{};
        const int Length = std::snprintf(
            Line, sizeof(Line),
            "[acs][memory] tracker=msvc_crt_debug_heap scope=process operation=dump_memory_leaks "
            "supported=true inspection_succeeded=%s leak_detected=%s status=%s reason=%s\n",
            Report.bInspectionSucceeded ? "true" : "false",
            Report.bInspectionSucceeded ? (Report.bLeakDetected ? "true" : "false") : "inconclusive",
            !Report.bInspectionSucceeded ? "inconclusive" : (Report.bLeakDetected ? "leak_detected" : "ok"),
            !Report.bInspectionSucceeded ? "report_hook_reentry" : "none");
        if (Length > 0 && static_cast<usize>(Length) < sizeof(Line)) {
            EmitDiagnosticLine(Line, !Report.bInspectionSucceeded || Report.bLeakDetected);
        } else {
            WriteDiagnosticFormatFailureFallback();
        }
    }
#else
    if (bWriteMachineReadableLog) {
        char Line[384]{};
        const int Length = std::snprintf(
            Line, sizeof(Line),
            "[acs][memory] tracker=msvc_crt_debug_heap scope=process operation=dump_memory_leaks "
            "supported=false inspection_succeeded=false leak_detected=inconclusive "
            "status=unsupported reason=%s\n",
            UnsupportedReason());
        if (Length > 0 && static_cast<usize>(Length) < sizeof(Line)) {
            EmitDiagnosticLine(Line, true);
        } else {
            WriteDiagnosticFormatFailureFallback();
        }
    }
#endif
    return Report;
}

i64 CCrtDebugHeapDiagnostics::SetBreakOnAllocationSequence(i64 AllocationSequence) noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    if (ProcessSettingsAreProtected()) {
        const i64 Current = static_cast<i64>(_crtBreakAlloc);
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return Current;
    }
    const i64 Previous = static_cast<i64>(_CrtSetBreakAlloc(NormalizeBreakAllocationSequence(AllocationSequence)));
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
    return Previous;
#else
    (void)AllocationSequence;
    return -1;
#endif
}

bool CCrtDebugHeapDiagnostics::SetProcessExitLeakCheckEnabled(bool bEnabled) noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    ::AcquireSRWLockExclusive(&g_ProcessConfigurationLock);
    const int OldFlags = _CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
    const bool bWasEnabled = (OldFlags & _CRTDBG_LEAK_CHECK_DF) != 0;
    if (ProcessSettingsAreProtected()) {
        ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
        return bWasEnabled;
    }
    const int NewFlags = ApplyBooleanFlag(OldFlags, _CRTDBG_LEAK_CHECK_DF, bEnabled);
    (void)_CrtSetDbgFlag(NewFlags);
    ::ReleaseSRWLockExclusive(&g_ProcessConfigurationLock);
    return bWasEnabled;
#else
    (void)bEnabled;
    return false;
#endif
}

} // namespace acs
