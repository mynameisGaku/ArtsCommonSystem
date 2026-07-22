// SPDX-License-Identifier: Apache-2.0
// HelloCrashReporter - FCrashReporterDemoApp implementation.
#include "CrashReporterDemoApp.h"

#include "crashwin/WindowsCrashReporter.h"

#include <Windows.h>
#include <cstdio>

namespace hellocrash {

namespace {

bool FileExists(const char* Path) noexcept {
    if (Path == nullptr || Path[0] == 0) {
        return false;
    }
    const DWORD Attr = ::GetFileAttributesA(Path);
    return Attr != INVALID_FILE_ATTRIBUTES && (Attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

} // namespace

int FCrashReporterDemoApp::Run() noexcept {
    std::puts("=== ACS HelloCrashReporter ===");
    std::puts("backend: REAL Windows DbgHelp MiniDumpWriteDump (FWindowsCrashReporter)");

    acs::crashwin::FWindowsCrashReporterConfig Config{};
    Config.DumpDirectory = "hello_crash_dumps";

    acs::crashwin::FWindowsCrashReporter Reporter(Config);
    auto Init = Reporter.Init("acs.samples.hello_crash_reporter", "dev");
    if (Init.IsErr()) {
        std::printf("Init: FAILED (%s)\n", Init.Error().message);
        return 2;
    }

    Reporter.SetUserId("anonymous-local-test-user");
    auto BreadcrumbA = Reporter.AddBreadcrumb("boot", "sample started");
    auto BreadcrumbB = Reporter.AddBreadcrumb("scene", "synthetic crash context prepared");
    if (BreadcrumbA.IsErr() || BreadcrumbB.IsErr()) {
        std::puts("AddBreadcrumb: FAILED");
        return 3;
    }

    acs::game::FCrashContext Context{};
    Context.exception_type = "SYNTHETIC_TEST_CRASH";
    Context.message = "hello crash reporter smoke test";
    Context.scene_name = "HelloCrashReporter";
    Context.frame_count = 42;
    Context.timestamp = 20260528;
    Context.build_id = "local-dev";
    Context.stack_trace = "SyntheticFrameA\nSyntheticFrameB\n";

    auto Crash = Reporter.ReportCrash(Context);
    if (Crash.IsErr()) {
        std::printf("ReportCrash: FAILED (%s)\n", Crash.Error().message);
        return 4;
    }

    const bool bReportExists = FileExists(Reporter.LastReportPath());
    const bool bDumpExists = FileExists(Reporter.LastDumpPath());
    std::printf("report: %s\n", Reporter.LastReportPath());
    std::printf("dump  : %s\n", Reporter.LastDumpPath());
    std::printf("files : report=%s dump=%s\n",
                bReportExists ? "ok" : "missing",
                bDumpExists ? "ok" : "missing");

    Reporter.Shutdown();

    const bool bOk = bReportExists && bDumpExists;
    std::puts(bOk ? "result: ALL PASS" : "result: FAILED");
    return bOk ? 0 : 5;
}

} // namespace hellocrash
