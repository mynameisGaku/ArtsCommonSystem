// SPDX-License-Identifier: Apache-2.0
#include "memory/CrtDebugHeapDiagnostics.h"

#include <cstdlib>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
#    include <crtdbg.h>
#endif

#if ACS_PLATFORM_WINDOWS
#    include <windows.h>
#endif

namespace {

// 子プロセス引数を比較します。
bool TextEquals(const char* Left, const char* Right) noexcept
{
    if (!Left || !Right) {
        return false;
    }
    return std::strcmp(Left, Right) == 0;
}

// CRT自身の診断をデバッガへ送り、機械行だけをパイプへ残します。
void ConfigureCrtReportsForPipe() noexcept
{
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    (void)::_CrtSetReportMode(_CRT_WARN, _CRTDBG_MODE_DEBUG);
    (void)::_CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_DEBUG);
    (void)::_CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_DEBUG);
#endif
}

// プロセス検査の成功・リーク結果を子プロセスで生成します。
int RunProcessProbe(bool bCreateLeak) noexcept
{
    ConfigureCrtReportsForPipe();
    void* IntentionalLeak = nullptr;
    if (bCreateLeak) {
        IntentionalLeak = ::malloc(64u);
        if (!IntentionalLeak) {
            return 76;
        }
    }

    const acs::FCrtDebugHeapProcessLeakReport Report =
        acs::CCrtDebugHeapDiagnostics::DumpProcessMemoryLeaks(true);
    if (IntentionalLeak) {
        ::free(IntentionalLeak);
    }

    if (!Report.bSupported) {
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
        return 77;
#else
        return 0;
#endif
    }
    if (!Report.bInspectionSucceeded) {
        return 77;
    }
    if (Report.bLeakDetected != bCreateLeak) {
        return 78;
    }
    return 0;
}

// Releaseの非対応プロセス結果を明示的に確認します。
int RunUnsupportedProcessProbe() noexcept
{
    const acs::FCrtDebugHeapProcessLeakReport Report =
        acs::CCrtDebugHeapDiagnostics::DumpProcessMemoryLeaks(true);
    return !Report.bSupported && !Report.bInspectionSucceeded && !Report.bLeakDetected ? 0 : 87;
}

// Releaseの非対応スコープ結果を明示的に確認します。
int RunUnsupportedScopeProbe() noexcept
{
    acs::FCrtDebugHeapScope Scope;
    // 非対応ビルドでも同じ開始・終了経路を通します。
    const acs::FCrtDebugHeapScopeConfiguration Configuration{};
    if (!Scope.Begin(Configuration)) {
        return 88;
    }
    const acs::FCrtDebugHeapScopeReport Report = Scope.End();
    return !Report.bSupported && !Report.bMeasurementConclusive ? 0 : 89;
}

// スコープ検査の成功・リーク・検査不能結果を子プロセスで生成します。
int RunScopeProbe(int Mode) noexcept
{
    ConfigureCrtReportsForPipe();
    acs::FCrtDebugHeapScope Scope;
    acs::FCrtDebugHeapScopeConfiguration Configuration{};
    Configuration.ScopeName = "routing_scope";
    if (!Scope.Begin(Configuration)) {
        return 81;
    }

    void* IntentionalLeak = nullptr;
    if (Mode == 1) {
        IntentionalLeak = ::malloc(64u);
        if (!IntentionalLeak) {
            (void)Scope.End();
            return 82;
        }
    }
    int OriginalFlags = -1;
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (Mode == 2) {
        OriginalFlags = ::_CrtSetDbgFlag(_CRTDBG_REPORT_FLAG);
        (void)::_CrtSetDbgFlag(OriginalFlags ^ _CRTDBG_ALLOC_MEM_DF);
    }
#endif

    const acs::FCrtDebugHeapScopeReport Report = Scope.End();
#if ACS_COMPILER_MSVC && ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
    if (OriginalFlags >= 0) {
        (void)::_CrtSetDbgFlag(OriginalFlags);
    }
#endif
    if (IntentionalLeak) {
        ::free(IntentionalLeak);
    }
    if (!Report.bSupported) {
#if ACS_BUILD_DEBUG && !ACS_ADDRESS_SANITIZER
        return 83;
#else
        return 0;
#endif
    }
    if (Mode == 0 && (!Report.bMeasurementConclusive || Report.bLeakDetected)) {
        return 84;
    }
    if (Mode == 1 && (!Report.bMeasurementConclusive || !Report.bLeakDetected)) {
        return 85;
    }
    if (Mode == 2 && Report.bMeasurementConclusive) {
        return 86;
    }
    return 0;
}

#if ACS_PLATFORM_WINDOWS
struct FCapturedProcess {
    // 子プロセスの終了コードです。
    DWORD ExitCode = 0xFFFFFFFFu;
    // タイムアウトで子プロセスを終了したかを示します。
    bool bTimedOut = false;
    // 標準出力の捕捉内容です。
    std::string Stdout;
    // 標準エラーの捕捉内容です。
    std::string Stderr;
};

// 継承を切った読出しハンドルから全出力を取得します。
bool ReadPipe(HANDLE Handle, std::string& Output) noexcept
{
    char Buffer[4096];
    for (;;) {
        DWORD ReadCount = 0u;
        if (!::ReadFile(Handle, Buffer, static_cast<DWORD>(sizeof(Buffer)), &ReadCount, nullptr)) {
            return ::GetLastError() == ERROR_BROKEN_PIPE;
        }
        if (ReadCount == 0u) {
            return true;
        }
        Output.append(Buffer, ReadCount);
    }
}

// 同じprobe実行ファイルを別プロセスで起動し、両ストリームを分離します。
bool CaptureChild(const char* Mode, FCapturedProcess& Capture) noexcept
{
    // 親だけが読むための標準出力・標準エラーのパイプです。
    SECURITY_ATTRIBUTES Attributes{};
    Attributes.nLength = sizeof(Attributes);
    Attributes.bInheritHandle = TRUE;
    HANDLE StdoutRead = nullptr;
    HANDLE StdoutWrite = nullptr;
    HANDLE StderrRead = nullptr;
    HANDLE StderrWrite = nullptr;
    if (!::CreatePipe(&StdoutRead, &StdoutWrite, &Attributes, 0u)) {
        return false;
    }
    if (!::CreatePipe(&StderrRead, &StderrWrite, &Attributes, 0u)) {
        ::CloseHandle(StdoutRead);
        ::CloseHandle(StdoutWrite);
        return false;
    }
    (void)::SetHandleInformation(StdoutRead, HANDLE_FLAG_INHERIT, 0u);
    (void)::SetHandleInformation(StderrRead, HANDLE_FLAG_INHERIT, 0u);

    char ExecutablePath[2048]{};
    const DWORD PathLength = ::GetModuleFileNameA(nullptr, ExecutablePath, static_cast<DWORD>(sizeof(ExecutablePath)));
    char CommandLine[4096]{};
    std::snprintf(CommandLine, sizeof(CommandLine), "\"%s\" %s", ExecutablePath, Mode);

    STARTUPINFOA StartupInfo{};
    StartupInfo.cb = sizeof(StartupInfo);
    StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    StartupInfo.hStdInput = ::GetStdHandle(STD_INPUT_HANDLE);
    StartupInfo.hStdOutput = StdoutWrite;
    StartupInfo.hStdError = StderrWrite;
    PROCESS_INFORMATION ProcessInfo{};
    const BOOL Created = PathLength != 0u && ::CreateProcessA(nullptr, CommandLine, nullptr, nullptr, TRUE,
                                                              CREATE_NO_WINDOW, nullptr, nullptr, &StartupInfo,
                                                              &ProcessInfo);
    ::CloseHandle(StdoutWrite);
    ::CloseHandle(StderrWrite);
    if (!Created) {
        ::CloseHandle(StdoutRead);
        ::CloseHandle(StderrRead);
        return false;
    }
    // 子プロセスが停止しない場合は10秒で子プロセスだけを終了します。
    const DWORD WaitResult = ::WaitForSingleObject(ProcessInfo.hProcess, 10000u);
    if (WaitResult == WAIT_TIMEOUT) {
        Capture.bTimedOut = true;
        (void)::TerminateProcess(ProcessInfo.hProcess, 124u);
        (void)::WaitForSingleObject(ProcessInfo.hProcess, 2000u);
    }
    (void)::GetExitCodeProcess(ProcessInfo.hProcess, &Capture.ExitCode);
    (void)ReadPipe(StdoutRead, Capture.Stdout);
    (void)ReadPipe(StderrRead, Capture.Stderr);
    ::CloseHandle(ProcessInfo.hThread);
    ::CloseHandle(ProcessInfo.hProcess);
    ::CloseHandle(StdoutRead);
    ::CloseHandle(StderrRead);
    return true;
}

std::size_t CountToken(const std::string& Text, const char* Token) noexcept
{
    std::size_t Count = 0u;
    std::size_t Offset = 0u;
    const std::size_t TokenLength = std::strlen(Token);
    while (TokenLength != 0u && Offset < Text.size()) {
        const std::size_t Found = Text.find(Token, Offset);
        if (Found == std::string::npos) {
            break;
        }
        ++Count;
        Offset = Found + TokenLength;
    }
    return Count;
}

// 機械行が改行1件だけで終わることを検証します。
bool IsSingleDiagnosticLine(const std::string& Text) noexcept
{
    return !Text.empty() && Text.back() == '\n' && Text.find('\n') == Text.size() - 1u && Text.find('\r') == std::string::npos;
}

// 固定プレフィックスと期待status以外の診断行を拒否します。
bool HasExactDiagnosticShape(const std::string& Text, const char* ExpectedStatus) noexcept
{
    constexpr char Prefix[] = "[acs][memory] tracker=msvc_crt_debug_heap ";
    return IsSingleDiagnosticLine(Text) && Text.rfind(Prefix, 0u) == 0u &&
           CountToken(Text, ExpectedStatus) == 1u && CountToken(Text, "status=") == 1u &&
           CountToken(Text, "tracker=msvc_crt_debug_heap") == 1u;
}

// 成功行が標準出力だけに1件あることを検証します。
bool ExpectRoute(const char* Mode, const char* ExpectedStdout, const char* ExpectedStderr) noexcept
{
    // 成功モードの期待引数と両チャネルを比較します。
    FCapturedProcess Capture{};
    if (!CaptureChild(Mode, Capture) || Capture.bTimedOut || Capture.ExitCode != 0u) {
        return false;
    }
    if (!HasExactDiagnosticShape(Capture.Stdout, ExpectedStdout) || !Capture.Stderr.empty() ||
        CountToken(Capture.Stderr, ExpectedStderr) != 0u) {
        return false;
    }
    return true;
}

// 失敗行が標準エラーだけに1件あることを検証します。
bool ExpectFailureRoute(const char* Mode, const char* ExpectedStdout, const char* ExpectedStderr) noexcept
{
    // 失敗モードの期待引数と両チャネルを比較します。
    FCapturedProcess Capture{};
    if (!CaptureChild(Mode, Capture) || Capture.bTimedOut || Capture.ExitCode != 0u) {
        return false;
    }
    return Capture.Stdout.empty() && HasExactDiagnosticShape(Capture.Stderr, ExpectedStderr) &&
           CountToken(Capture.Stdout, ExpectedStdout) == 0u;
}

// 出力先を無効にした子プロセスが静かに終了することを検証します。
bool ExpectSilentSuccess(const char* Mode) noexcept
{
    // デバッガ専用分岐が標準ストリームを汚さないことを比較します。
    FCapturedProcess Capture{};
    return CaptureChild(Mode, Capture) && !Capture.bTimedOut && Capture.ExitCode == 0u && Capture.Stdout.empty() &&
           Capture.Stderr.empty();
}

// 失敗行全体を比較し、改行を含めた機械契約を固定します。
bool ExpectExactFailureRoute(const char* Mode, const char* ExpectedLine) noexcept
{
    FCapturedProcess Capture{};
    if (!CaptureChild(Mode, Capture) || Capture.bTimedOut || Capture.ExitCode != 0u) {
        return false;
    }
    return Capture.Stdout.empty() && Capture.Stderr == ExpectedLine && IsSingleDiagnosticLine(Capture.Stderr);
}

// Debugの全routing行列を順番に検証します。
int RunRoutingVerification() noexcept
{
    if (!ExpectRoute("--child-process-clean", "status=ok", "status=ok")) {
        return 91;
    }
    if (!ExpectFailureRoute("--child-process-positive", "status=ok", "status=leak_detected")) {
        return 92;
    }
    if (!ExpectRoute("--child-scope-clean", "status=ok", "status=leak_detected")) {
        return 93;
    }
    if (!ExpectFailureRoute("--child-scope-positive", "status=ok", "status=leak_detected")) {
        return 94;
    }
    if (!ExpectFailureRoute("--child-scope-inconclusive", "status=ok", "status=inconclusive")) {
        return 95;
    }
    if (!ExpectFailureRoute("--child-write-failed", "status=ok", "status=write_failed")) {
        return 96;
    }
    if (!ExpectExactFailureRoute(
            "--child-write-failed",
            "[acs][memory] tracker=msvc_crt_debug_heap status=write_failed target=standard_output "
            "reason=standard_output_write_failed\n")) {
        return 97;
    }
    if (!ExpectSilentSuccess("--child-invalid-stderr")) {
        return 97;
    }
    return 0;
}

// Releaseで非対応結果が標準エラーへ出ることを検証します。
int RunReleaseVerification() noexcept
{
    if (!ExpectExactFailureRoute(
            "--child-release-process",
            "[acs][memory] tracker=msvc_crt_debug_heap scope=process operation=dump_memory_leaks "
            "supported=false inspection_succeeded=false leak_detected=inconclusive status=unsupported "
            "reason=unsupported_build\n")) {
        return 98;
    }
    return ExpectExactFailureRoute(
               "--child-release-scope",
               "[acs][memory] tracker=msvc_crt_debug_heap supported=false scope=unnamed "
               "measurement_performed=false measurement_conclusive=false configuration_stable=false "
               "allocation_tracking_enabled=false leak_detected=inconclusive status=unsupported "
               "reason=unsupported_build outstanding_allocations=0 outstanding_bytes=0 normal_allocations=0 "
               "normal_bytes=0 client_allocations=0 client_bytes=0 crt_allocations=0 crt_bytes=0 "
               "difference_detected=inconclusive heap_valid=inconclusive\n")
               ? 0
               : 99;
}
#endif

} // namespace

int main(int ArgumentCount, char** Arguments)
{
    if (ArgumentCount != 2 || !Arguments[1]) {
        return 79;
    }
    if (TextEquals(Arguments[1], "--child-process-clean") || TextEquals(Arguments[1], "--clean")) {
        return RunProcessProbe(false);
    }
    if (TextEquals(Arguments[1], "--child-process-positive") || TextEquals(Arguments[1], "--positive")) {
        return RunProcessProbe(true);
    }
    if (TextEquals(Arguments[1], "--child-scope-clean")) {
        return RunScopeProbe(0);
    }
    if (TextEquals(Arguments[1], "--child-scope-positive")) {
        return RunScopeProbe(1);
    }
    if (TextEquals(Arguments[1], "--child-scope-inconclusive")) {
        return RunScopeProbe(2);
    }
    if (TextEquals(Arguments[1], "--child-write-failed")) {
#if ACS_PLATFORM_WINDOWS
        const HANDLE PreviousOutput = ::GetStdHandle(STD_OUTPUT_HANDLE);
        ::SetStdHandle(STD_OUTPUT_HANDLE, INVALID_HANDLE_VALUE);
        const int Result = RunProcessProbe(false);
        ::SetStdHandle(STD_OUTPUT_HANDLE, PreviousOutput);
        return Result == 0 ? 0 : Result;
#else
        return 98;
#endif
    }
    if (TextEquals(Arguments[1], "--child-invalid-stderr")) {
#if ACS_PLATFORM_WINDOWS
        const HANDLE PreviousError = ::GetStdHandle(STD_ERROR_HANDLE);
        ::SetStdHandle(STD_ERROR_HANDLE, INVALID_HANDLE_VALUE);
        const int Result = RunProcessProbe(true);
        ::SetStdHandle(STD_ERROR_HANDLE, PreviousError);
        return Result == 0 ? 0 : Result;
#else
        return 99;
#endif
    }
    if (TextEquals(Arguments[1], "--child-release-process")) {
        return RunUnsupportedProcessProbe();
    }
    if (TextEquals(Arguments[1], "--child-release-scope")) {
        return RunUnsupportedScopeProbe();
    }
#if ACS_PLATFORM_WINDOWS
    if (TextEquals(Arguments[1], "--verify-routing")) {
        return RunRoutingVerification();
    }
    if (TextEquals(Arguments[1], "--verify-release")) {
        return RunReleaseVerification();
    }
#endif
    return 80;
}
