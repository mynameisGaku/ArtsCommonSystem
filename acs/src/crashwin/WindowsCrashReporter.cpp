// SPDX-License-Identifier: Apache-2.0
// Windows DbgHelp minidump backend for acs::game::ICrashReporterBackend.
#include "crashwin/WindowsCrashReporter.h"

#include "foundation/Error.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdio>
#include <cstring>

namespace acs::crashwin {

FWindowsCrashReporter::FWindowsCrashReporter(const FWindowsCrashReporterConfig& Config) noexcept {
    CopyText(m_DumpDirectory, sizeof(m_DumpDirectory), Config.DumpDirectory);
}

void FWindowsCrashReporter::CopyText(char* Dst, acs::usize DstSize, const char* Src) noexcept {
    if (Dst == nullptr || DstSize == 0) {
        return;
    }
    Dst[0] = 0;
    if (Src == nullptr) {
        return;
    }
    std::strncpy(Dst, Src, DstSize - 1);
    Dst[DstSize - 1] = 0;
}

acs::TResult<void> FWindowsCrashReporter::Init(const char* ProductId, const char* Version) noexcept {
    if (ProductId == nullptr || ProductId[0] == 0 || Version == nullptr || Version[0] == 0) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::Init requires product id and version");
    }

    CopyText(m_ProductId, sizeof(m_ProductId), ProductId);
    CopyText(m_Version, sizeof(m_Version), Version);
    if (m_DumpDirectory[0] == 0) {
        CopyText(m_DumpDirectory, sizeof(m_DumpDirectory), "crash_dumps");
    }

    if (!::CreateDirectoryA(m_DumpDirectory, nullptr)) {
        const DWORD Error = ::GetLastError();
        if (Error != ERROR_ALREADY_EXISTS) {
            return ACS_ERR(Generic, kSubCrashWinIoFailed,
                           "FWindowsCrashReporter failed to create dump directory");
        }
    }

    m_bInitialized = true;
    return acs::Ok();
}

void FWindowsCrashReporter::Shutdown() noexcept {
    m_bInitialized = false;
}

bool FWindowsCrashReporter::IsAvailable() const noexcept {
    return m_bInitialized;
}

void FWindowsCrashReporter::BuildBasePath(char* Out, acs::usize OutSize, const char* Prefix) noexcept {
    SYSTEMTIME Time{};
    ::GetLocalTime(&Time);
    const DWORD ProcessId = ::GetCurrentProcessId();
    const acs::u32 Serial = ++m_ReportSerial;
    std::snprintf(Out, OutSize,
                  "%s\\%s_%04u%02u%02u_%02u%02u%02u_pid%lu_%u",
                  m_DumpDirectory,
                  Prefix,
                  static_cast<unsigned>(Time.wYear),
                  static_cast<unsigned>(Time.wMonth),
                  static_cast<unsigned>(Time.wDay),
                  static_cast<unsigned>(Time.wHour),
                  static_cast<unsigned>(Time.wMinute),
                  static_cast<unsigned>(Time.wSecond),
                  static_cast<unsigned long>(ProcessId),
                  static_cast<unsigned>(Serial));
    Out[OutSize - 1] = 0;
}

bool FWindowsCrashReporter::WriteTextReport(const char* Path,
                                            const acs::game::CrashContext* Context,
                                            const char* Category,
                                            const char* Message) noexcept {
    FILE* File = nullptr;
    if (fopen_s(&File, Path, "wb") != 0 || File == nullptr) {
        return false;
    }

    std::fprintf(File, "product_id=%s\n", m_ProductId);
    std::fprintf(File, "version=%s\n", m_Version);
    std::fprintf(File, "user_id=%s\n", m_UserId[0] ? m_UserId : "<unset>");
    if (Category != nullptr) {
        std::fprintf(File, "category=%s\n", Category);
    }
    if (Message != nullptr) {
        std::fprintf(File, "message=%s\n", Message);
    }
    if (Context != nullptr) {
        std::fprintf(File, "exception_type=%s\n",
                     Context->exception_type ? Context->exception_type : "<null>");
        std::fprintf(File, "crash_message=%s\n",
                     Context->message ? Context->message : "<null>");
        std::fprintf(File, "scene_name=%s\n",
                     Context->scene_name ? Context->scene_name : "<null>");
        std::fprintf(File, "frame_count=%llu\n",
                     static_cast<unsigned long long>(Context->frame_count));
        std::fprintf(File, "timestamp=%llu\n",
                     static_cast<unsigned long long>(Context->timestamp));
        std::fprintf(File, "build_id=%s\n",
                     Context->build_id ? Context->build_id : "<null>");
        if (Context->stack_trace != nullptr) {
            std::fprintf(File, "stack_trace=\n%s\n", Context->stack_trace);
        }
    }

    std::fprintf(File, "breadcrumbs=%u\n", static_cast<unsigned>(m_BreadcrumbCount));
    for (acs::u32 i = 0; i < m_BreadcrumbCount; ++i) {
        const acs::u32 Index = (m_BreadcrumbStart + i) % kMaxBreadcrumbs;
        const FBreadcrumb& Breadcrumb = m_Breadcrumbs[Index];
        std::fprintf(File, "  [%u] %s: %s\n",
                     static_cast<unsigned>(i),
                     Breadcrumb.Category,
                     Breadcrumb.Message);
    }

    fclose(File);
    return true;
}

bool FWindowsCrashReporter::WriteCurrentProcessDump(const char* Path) noexcept {
    HANDLE File = ::CreateFileA(Path,
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (File == INVALID_HANDLE_VALUE) {
        return false;
    }

    const BOOL bOk = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                         ::GetCurrentProcessId(),
                                         File,
                                         MiniDumpNormal,
                                         nullptr,
                                         nullptr,
                                         nullptr);
    ::CloseHandle(File);
    return bOk == TRUE;
}

acs::TResult<void> FWindowsCrashReporter::ReportCrash(
    const acs::game::CrashContext& Context) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::ReportCrash called before Init");
    }

    char Base[260] = {};
    BuildBasePath(Base, sizeof(Base), "crash");
    std::snprintf(m_LastReportPath, sizeof(m_LastReportPath), "%s.txt", Base);
    std::snprintf(m_LastDumpPath, sizeof(m_LastDumpPath), "%s.dmp", Base);

    if (!WriteTextReport(m_LastReportPath, &Context, nullptr, nullptr)) {
        return ACS_ERR(Generic, kSubCrashWinIoFailed,
                       "FWindowsCrashReporter failed to write crash text report");
    }
    if (!WriteCurrentProcessDump(m_LastDumpPath)) {
        return ACS_ERR(Generic, kSubCrashWinDumpFailed,
                       "FWindowsCrashReporter failed to write minidump");
    }

    return acs::Ok();
}

acs::TResult<void> FWindowsCrashReporter::ReportError(const char* Category,
                                                       const char* Message) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::ReportError called before Init");
    }
    if (Category == nullptr || Message == nullptr) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::ReportError requires category and message");
    }

    char Base[260] = {};
    BuildBasePath(Base, sizeof(Base), "error");
    std::snprintf(m_LastReportPath, sizeof(m_LastReportPath), "%s.txt", Base);
    m_LastDumpPath[0] = 0;

    if (!WriteTextReport(m_LastReportPath, nullptr, Category, Message)) {
        return ACS_ERR(Generic, kSubCrashWinIoFailed,
                       "FWindowsCrashReporter failed to write error report");
    }
    return acs::Ok();
}

acs::TResult<void> FWindowsCrashReporter::AddBreadcrumb(const char* Category,
                                                         const char* Message) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::AddBreadcrumb called before Init");
    }
    if (Category == nullptr || Message == nullptr) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::AddBreadcrumb requires category and message");
    }

    acs::u32 Index = 0;
    if (m_BreadcrumbCount < kMaxBreadcrumbs) {
        Index = (m_BreadcrumbStart + m_BreadcrumbCount) % kMaxBreadcrumbs;
        ++m_BreadcrumbCount;
    } else {
        Index = m_BreadcrumbStart;
        m_BreadcrumbStart = (m_BreadcrumbStart + 1) % kMaxBreadcrumbs;
    }

    CopyText(m_Breadcrumbs[Index].Category, sizeof(m_Breadcrumbs[Index].Category), Category);
    CopyText(m_Breadcrumbs[Index].Message, sizeof(m_Breadcrumbs[Index].Message), Message);
    return acs::Ok();
}

void FWindowsCrashReporter::SetUserId(const char* AnonymousId) noexcept {
    CopyText(m_UserId, sizeof(m_UserId), AnonymousId);
}

void FWindowsCrashReporter::Tick(acs::f32 Dt) noexcept {
    (void)Dt;
}

} // namespace acs::crashwin

