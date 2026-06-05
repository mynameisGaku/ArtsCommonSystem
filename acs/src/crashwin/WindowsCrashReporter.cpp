// SPDX-License-Identifier: Apache-2.0
// Windows DbgHelp minidump backend for acs::game::ICrashReporterBackend.
#include "crashwin/WindowsCrashReporter.h"

#include "foundation/Error.h"

#include <Windows.h>
#include <DbgHelp.h>

#include <cstdio>
#include <cstring>

namespace acs::crashwin {

/** 設定からダンプディレクトリを取り込んで構築する。 */
FWindowsCrashReporter::FWindowsCrashReporter(const FWindowsCrashReporterConfig& config) noexcept {
    CopyText(m_DumpDirectory, sizeof(m_DumpDirectory), config.DumpDirectory);
}

/** src を dst へ安全に NUL 終端コピーする (バッファ境界を超えない)。 */
void FWindowsCrashReporter::CopyText(char* dst, acs::usize dst_size, const char* src) noexcept {
    if (dst == nullptr || dst_size == 0) {
        return;
    }
    dst[0] = 0;
    if (src == nullptr) {
        return;
    }
    std::strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = 0;
}

/** backend を初期化し、ダンプディレクトリを作成する。 */
acs::TResult<void> FWindowsCrashReporter::Init(const char* product_id, const char* version) noexcept {
    if (product_id == nullptr || product_id[0] == 0 || version == nullptr || version[0] == 0) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::Init requires product id and version");
    }

    CopyText(m_ProductId, sizeof(m_ProductId), product_id);
    CopyText(m_Version, sizeof(m_Version), version);
    if (m_DumpDirectory[0] == 0) {
        CopyText(m_DumpDirectory, sizeof(m_DumpDirectory), "crash_dumps");
    }

    if (!::CreateDirectoryA(m_DumpDirectory, nullptr)) {
        const DWORD error = ::GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return ACS_ERR(Generic, kSubCrashWinIoFailed,
                           "FWindowsCrashReporter failed to create dump directory");
        }
    }

    m_bInitialized = true;
    return acs::Ok();
}

/** backend を終了状態にする (m_bInitialized を下ろすだけ)。 */
void FWindowsCrashReporter::Shutdown() noexcept {
    m_bInitialized = false;
}

/** backend が利用可能 (Init 済み) かを返す。 */
bool FWindowsCrashReporter::IsAvailable() const noexcept {
    return m_bInitialized;
}

/** レポート用の一意なベースパス (拡張子なし) を組み立てる。 */
void FWindowsCrashReporter::BuildBasePath(char* out, acs::usize out_size, const char* prefix) noexcept {
    SYSTEMTIME time{};
    ::GetLocalTime(&time);
    const DWORD process_id = ::GetCurrentProcessId();
    const acs::u32 serial = ++m_ReportSerial;
    std::snprintf(out, out_size,
                  "%s\\%s_%04u%02u%02u_%02u%02u%02u_pid%lu_%u",
                  m_DumpDirectory,
                  prefix,
                  static_cast<unsigned>(time.wYear),
                  static_cast<unsigned>(time.wMonth),
                  static_cast<unsigned>(time.wDay),
                  static_cast<unsigned>(time.wHour),
                  static_cast<unsigned>(time.wMinute),
                  static_cast<unsigned>(time.wSecond),
                  static_cast<unsigned long>(process_id),
                  static_cast<unsigned>(serial));
    out[out_size - 1] = 0;
}

/** テキスト形式のクラッシュ / エラーレポートをファイルへ書き出す。 */
bool FWindowsCrashReporter::WriteTextReport(const char* path,
                                            const acs::game::CrashContext* context,
                                            const char* category,
                                            const char* message) noexcept {
    FILE* file = nullptr;
    if (fopen_s(&file, path, "wb") != 0 || file == nullptr) {
        return false;
    }

    std::fprintf(file, "product_id=%s\n", m_ProductId);
    std::fprintf(file, "version=%s\n", m_Version);
    std::fprintf(file, "user_id=%s\n", m_UserId[0] ? m_UserId : "<unset>");
    if (category != nullptr) {
        std::fprintf(file, "category=%s\n", category);
    }
    if (message != nullptr) {
        std::fprintf(file, "message=%s\n", message);
    }
    if (context != nullptr) {
        std::fprintf(file, "exception_type=%s\n",
                     context->exception_type ? context->exception_type : "<null>");
        std::fprintf(file, "crash_message=%s\n",
                     context->message ? context->message : "<null>");
        std::fprintf(file, "scene_name=%s\n",
                     context->scene_name ? context->scene_name : "<null>");
        std::fprintf(file, "frame_count=%llu\n",
                     static_cast<unsigned long long>(context->frame_count));
        std::fprintf(file, "timestamp=%llu\n",
                     static_cast<unsigned long long>(context->timestamp));
        std::fprintf(file, "build_id=%s\n",
                     context->build_id ? context->build_id : "<null>");
        if (context->stack_trace != nullptr) {
            std::fprintf(file, "stack_trace=\n%s\n", context->stack_trace);
        }
    }

    std::fprintf(file, "breadcrumbs=%u\n", static_cast<unsigned>(m_BreadcrumbCount));
    for (acs::u32 i = 0; i < m_BreadcrumbCount; ++i) {
        const acs::u32 index = (m_BreadcrumbStart + i) % kMaxBreadcrumbs;
        const FBreadcrumb& breadcrumb = m_Breadcrumbs[index];
        std::fprintf(file, "  [%u] %s: %s\n",
                     static_cast<unsigned>(i),
                     breadcrumb.Category,
                     breadcrumb.Message);
    }

    fclose(file);
    return true;
}

/** 現在のプロセスの minidump を MiniDumpWriteDump で書き出す。 */
bool FWindowsCrashReporter::WriteCurrentProcessDump(const char* path) noexcept {
    const HANDLE file = ::CreateFileA(path,
                                GENERIC_WRITE,
                                0,
                                nullptr,
                                CREATE_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    const BOOL b_ok = ::MiniDumpWriteDump(::GetCurrentProcess(),
                                         ::GetCurrentProcessId(),
                                         file,
                                         MiniDumpNormal,
                                         nullptr,
                                         nullptr,
                                         nullptr);
    ::CloseHandle(file);
    return b_ok == TRUE;
}

/** クラッシュ 1 件のテキストレポートと minidump を書き出す。 */
acs::TResult<void> FWindowsCrashReporter::ReportCrash(
    const acs::game::CrashContext& context) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::ReportCrash called before Init");
    }

    char base[260] = {};
    BuildBasePath(base, sizeof(base), "crash");
    std::snprintf(m_LastReportPath, sizeof(m_LastReportPath), "%s.txt", base);
    std::snprintf(m_LastDumpPath, sizeof(m_LastDumpPath), "%s.dmp", base);

    if (!WriteTextReport(m_LastReportPath, &context, nullptr, nullptr)) {
        return ACS_ERR(Generic, kSubCrashWinIoFailed,
                       "FWindowsCrashReporter failed to write crash text report");
    }
    if (!WriteCurrentProcessDump(m_LastDumpPath)) {
        return ACS_ERR(Generic, kSubCrashWinDumpFailed,
                       "FWindowsCrashReporter failed to write minidump");
    }

    return acs::Ok();
}

/** 非致命的エラー 1 件のテキストレポートを書き出す (minidump は作らない)。 */
acs::TResult<void> FWindowsCrashReporter::ReportError(const char* category,
                                                       const char* message) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::ReportError called before Init");
    }
    if (category == nullptr || message == nullptr) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::ReportError requires category and message");
    }

    char base[260] = {};
    BuildBasePath(base, sizeof(base), "error");
    std::snprintf(m_LastReportPath, sizeof(m_LastReportPath), "%s.txt", base);
    m_LastDumpPath[0] = 0;

    if (!WriteTextReport(m_LastReportPath, nullptr, category, message)) {
        return ACS_ERR(Generic, kSubCrashWinIoFailed,
                       "FWindowsCrashReporter failed to write error report");
    }
    return acs::Ok();
}

/** breadcrumb を 1 件リングバッファへ追加する (満杯時は最古を上書き)。 */
acs::TResult<void> FWindowsCrashReporter::AddBreadcrumb(const char* category,
                                                         const char* message) noexcept {
    if (!m_bInitialized) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_NotInitialized,
                       "FWindowsCrashReporter::AddBreadcrumb called before Init");
    }
    if (category == nullptr || message == nullptr) {
        return ACS_ERR(Generic, acs::game::CrashReporterError::kSub_BadArgument,
                       "FWindowsCrashReporter::AddBreadcrumb requires category and message");
    }

    acs::u32 index = 0;
    if (m_BreadcrumbCount < kMaxBreadcrumbs) {
        index = (m_BreadcrumbStart + m_BreadcrumbCount) % kMaxBreadcrumbs;
        ++m_BreadcrumbCount;
    } else {
        index = m_BreadcrumbStart;
        m_BreadcrumbStart = (m_BreadcrumbStart + 1) % kMaxBreadcrumbs;
    }

    CopyText(m_Breadcrumbs[index].Category, sizeof(m_Breadcrumbs[index].Category), category);
    CopyText(m_Breadcrumbs[index].Message, sizeof(m_Breadcrumbs[index].Message), message);
    return acs::Ok();
}

/** レポートに付与する匿名ユーザー ID を設定する。 */
void FWindowsCrashReporter::SetUserId(const char* anonymous_id) noexcept {
    CopyText(m_UserId, sizeof(m_UserId), anonymous_id);
}

/** 毎フレーム pump フック (本 backend は同期書き込みのため何もしない)。 */
void FWindowsCrashReporter::Tick(acs::f32 dt) noexcept {
    (void)dt;
}

} // namespace acs::crashwin

