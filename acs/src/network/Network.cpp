// SPDX-License-Identifier: Apache-2.0
// ネットワーク共通（WSAStartup, IpAddress 文字列パース）
#include "network/Network.h"
#include "network/IpAddress.h"
#include "foundation/Platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cstdio>

namespace acs {

namespace {

/** WSAStartup / WSACleanup と参照数の遷移を一体で直列化する。 */
SRWLOCK g_network_lifecycle_lock = SRWLOCK_INIT;

/** Init/Shutdown の対称な参照数 (0 で未初期化、1 以上で初期化済み)。 */
u32 g_initialization_reference_count = 0;

/** 失敗した最終 WSACleanup のエラー。非0の間は新規 Init を拒否する。 */
u32 g_cleanup_error = 0;

/** プロセス寿命中に観測した Network 資源解放失敗の累積数。 */
volatile LONG64 g_resource_failure_count = 0;

/** Network の資源解放失敗を debugger と標準エラーへ機械可読形式で出す。 */
void ReportNetworkResourceFailure(const char* record, u32 error, const char* leak_verdict) noexcept
{
    const LONG64 failure_count = ::_InterlockedIncrement64(&g_resource_failure_count);
    char message[320] = {};
    const int length = ::snprintf(message, sizeof(message),
                                  "memory_diagnostic_method=win32_resource tracker=network "
                                  "record=%s os_error=%u failure_count=%lld "
                                  "resource_release_failed=true leak_detected=%s\n",
                                  record, error, static_cast<long long>(failure_count), leak_verdict);
    if (length <= 0) return;

    ::OutputDebugStringA(message);
    const HANDLE error_output = ::GetStdHandle(STD_ERROR_HANDLE);
    if (error_output != nullptr && error_output != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        const DWORD byte_count = static_cast<DWORD>(static_cast<usize>(length) < sizeof(message) ? length
                                                                                                 : sizeof(message) - 1);
        (void)::WriteFile(error_output, message, byte_count, &written, nullptr);
    }
}

} // namespace

/**
 * WinSock を初期化する (多重呼び出し可)。
 *
 * @return 成功なら空の TResult、WSAStartup 失敗ならエラー。
 */
TResult<void> Network::Init() noexcept
{
    // 参照数だけを先に公開すると、別スレッドが WSAStartup 完了前に利用できてしまう。
    // 最終 WSACleanup と次の WSAStartup が交差する窓も同じロックで閉じる。
    ::AcquireSRWLockExclusive(&g_network_lifecycle_lock);

    if (g_cleanup_error != 0) {
        // Shutdown 呼出元が消えた後も、新規 Init が orphaned cleanup debt を回収できるよう再試行する。
        if (g_initialization_reference_count != 1) {
            const u32 cleanup_error = g_cleanup_error;
            ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
            ReportNetworkResourceFailure("cleanup_retry_invalid_reference_count", cleanup_error, "inconclusive");
            return ACS_ERR_OS(IO, 200, "Network::Init blocked by invalid cleanup state", cleanup_error);
        }
        if (::WSACleanup() == 0) {
            g_initialization_reference_count = 0;
            g_cleanup_error = 0;
        } else {
            const u32 cleanup_error = static_cast<u32>(::WSAGetLastError());
            if (cleanup_error == static_cast<u32>(WSANOTINITIALISED)) {
                g_initialization_reference_count = 0;
                g_cleanup_error = 0;
                ReportNetworkResourceFailure("cleanup_retry_reference_mismatch", cleanup_error, "inconclusive");
            } else {
                g_cleanup_error = cleanup_error;
                ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
                ReportNetworkResourceFailure("cleanup_retry_failed", cleanup_error, "inconclusive");
                return ACS_ERR_OS(IO, 200, "Network::Init blocked by pending cleanup", cleanup_error);
            }
        }
    }

    if (g_initialization_reference_count == ~u32{0}) {
        ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
        return ACS_ERR_OS(IO, 200, "Network::Init reference count overflow", static_cast<u32>(WSAEPROCLIM));
    }

    if (g_initialization_reference_count == 0) {
        WSADATA winsock_data{};
        const int startup_result = ::WSAStartup(MAKEWORD(2, 2), &winsock_data);
        if (startup_result != 0) {
            ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
            return ACS_ERR_OS(IO, 200, "WSAStartup failed", static_cast<u32>(startup_result));
        }
    }

    ++g_initialization_reference_count;
    ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
    return Ok();
}

/** 参照カウントを 1 減らし、最後の参照が外れたら WSACleanup を呼ぶ。 */
void Network::Shutdown() noexcept
{
    ::AcquireSRWLockExclusive(&g_network_lifecycle_lock);

    // 未初期化状態の Shutdown は参照数をアンダーフローさせず no-op とする。
    if (g_initialization_reference_count == 0) {
        ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
        return;
    }

    if (g_initialization_reference_count > 1) {
        --g_initialization_reference_count;
        ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
        return;
    }

    // 最後の参照は WSACleanup 成功後にだけ 0 として公開する。
    if (::WSACleanup() == 0) {
        g_initialization_reference_count = 0;
        g_cleanup_error = 0;
        ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
        return;
    }

    const u32 cleanup_error = static_cast<u32>(::WSAGetLastError());
    if (cleanup_error == static_cast<u32>(WSANOTINITIALISED)) {
        // OS 側に参照が無いことが確定したため内部状態を修復し、不整合は診断へ残す。
        g_initialization_reference_count = 0;
        g_cleanup_error = 0;
        ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
        ReportNetworkResourceFailure("cleanup_reference_mismatch", cleanup_error, "inconclusive");
        return;
    }

    g_cleanup_error = cleanup_error;
    ::ReleaseSRWLockExclusive(&g_network_lifecycle_lock);
    ReportNetworkResourceFailure("cleanup_failed", cleanup_error, "inconclusive");
}

/** 初期化済みかどうかを返す (参照カウントが 1 以上なら true)。 */
bool Network::IsInitialized() noexcept
{
    ::AcquireSRWLockShared(&g_network_lifecycle_lock);
    const bool initialized = g_initialization_reference_count > 0;
    ::ReleaseSRWLockShared(&g_network_lifecycle_lock);
    return initialized;
}

/** Network が所有する Winsock 資源状態を一貫した時点で取得する。 */
NetworkDiagnostics Network::CaptureDiagnostics() noexcept
{
    NetworkDiagnostics diagnostics{};
    ::AcquireSRWLockShared(&g_network_lifecycle_lock);
    diagnostics.initialization_reference_count = g_initialization_reference_count;
    diagnostics.pending_cleanup_error = g_cleanup_error;
    diagnostics.resource_release_failure_count = static_cast<u64>(
        ::_InterlockedCompareExchange64(&g_resource_failure_count, 0, 0));
    ::ReleaseSRWLockShared(&g_network_lifecycle_lock);
    return diagnostics;
}

/** "192.168.0.1" 等のドット区切り文字列を IpAddress に変換する (不正なら 0.0.0.0)。 */
IpAddress IpAddress::FromString(const char* dotted) noexcept
{
    IpAddress a{};
    if (!dotted) return a;

    u32 cur = 0;
    int oct = 0;
    bool digit = false;
    while (*dotted && oct < 4) {
        if (*dotted >= '0' && *dotted <= '9') {
            cur = cur * 10 + static_cast<u32>(*dotted - '0');
            digit = true;
            if (cur > 255) return IpAddress{}; // 不正値
        } else if (*dotted == '.') {
            if (!digit) return IpAddress{};
            a.octets[oct++] = static_cast<u8>(cur);
            cur = 0;
            digit = false;
        } else {
            return IpAddress{}; // 不正な文字
        }
        ++dotted;
    }
    if (oct < 4 && digit) a.octets[oct++] = static_cast<u8>(cur);
    if (oct != 4) return IpAddress{}; // 4 octet 揃っていない
    return a;
}

} // namespace acs
