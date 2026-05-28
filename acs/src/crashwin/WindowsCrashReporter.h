// SPDX-License-Identifier: Apache-2.0
// Windows DbgHelp minidump backend for acs::game::ICrashReporterBackend.
#pragma once

#include "gameframework/CrashReporter.h"

namespace acs::crashwin {

struct FWindowsCrashReporterConfig {
    const char* DumpDirectory = "crash_dumps";
};

class FWindowsCrashReporter final : public acs::game::ICrashReporterBackend {
public:
    FWindowsCrashReporter() noexcept = default;
    explicit FWindowsCrashReporter(const FWindowsCrashReporterConfig& Config) noexcept;
    ~FWindowsCrashReporter() noexcept override = default;

    FWindowsCrashReporter(const FWindowsCrashReporter&)            = delete;
    FWindowsCrashReporter& operator=(const FWindowsCrashReporter&) = delete;
    FWindowsCrashReporter(FWindowsCrashReporter&&)                 = delete;
    FWindowsCrashReporter& operator=(FWindowsCrashReporter&&)      = delete;

    acs::TResult<void> Init(const char* ProductId, const char* Version) noexcept override;
    void               Shutdown() noexcept override;
    bool               IsAvailable() const noexcept override;
    acs::TResult<void> ReportCrash(const acs::game::CrashContext& Context) noexcept override;
    acs::TResult<void> ReportError(const char* Category, const char* Message) noexcept override;
    acs::TResult<void> AddBreadcrumb(const char* Category, const char* Message) noexcept override;
    void               SetUserId(const char* AnonymousId) noexcept override;
    void               Tick(acs::f32 Dt) noexcept override;

    const char* LastDumpPath() const noexcept { return m_LastDumpPath; }
    const char* LastReportPath() const noexcept { return m_LastReportPath; }

private:
    struct FBreadcrumb {
        char Category[32] = {};
        char Message[160] = {};
    };

    static constexpr acs::u32 kMaxBreadcrumbs = 32;
    static constexpr acs::u16 kSubCrashWinIoFailed = 2001;
    static constexpr acs::u16 kSubCrashWinDumpFailed = 2002;

    void CopyText(char* Dst, acs::usize DstSize, const char* Src) noexcept;
    void BuildBasePath(char* Out, acs::usize OutSize, const char* Prefix) noexcept;
    bool WriteTextReport(const char* Path,
                         const acs::game::CrashContext* Context,
                         const char* Category,
                         const char* Message) noexcept;
    bool WriteCurrentProcessDump(const char* Path) noexcept;

    char m_DumpDirectory[260] = "crash_dumps";
    char m_ProductId[96] = {};
    char m_Version[48] = {};
    char m_UserId[96] = {};
    char m_LastDumpPath[260] = {};
    char m_LastReportPath[260] = {};

    FBreadcrumb m_Breadcrumbs[kMaxBreadcrumbs] = {};
    acs::u32    m_BreadcrumbStart = 0;
    acs::u32    m_BreadcrumbCount = 0;
    acs::u32    m_ReportSerial = 0;
    bool        m_bInitialized = false;
};

} // namespace acs::crashwin

