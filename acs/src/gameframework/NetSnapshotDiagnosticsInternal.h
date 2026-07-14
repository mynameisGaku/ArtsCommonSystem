// SPDX-License-Identifier: Apache-2.0
// FUdpTransport の失敗経路を決定論的に検証するための内部 API。
#pragma once

#include "foundation/Types.h"

namespace acs::game::internal {

/** Winsock 操作へ注入する一時的な失敗。製品 API からは使用しない。 */
struct UdpTransportFailureInjection {
    /** 次の closesocket 呼び出しを失敗させる回数。 */
    u32 close_socket_failure_count = 0;

    /** 注入する closesocket の Winsock エラー。 */
    u32 close_socket_error = 10035u; // WSAEWOULDBLOCK

    /** 次の WSACleanup 呼び出しを失敗させる回数。 */
    u32 cleanup_failure_count = 0;

    /** 注入する WSACleanup の Winsock エラー。 */
    u32 cleanup_error = 10036u; // WSAEINPROGRESS

    /** 0 は製品既定値。非0なら固定回収表を縮小し、overflow 経路を検証する。 */
    u32 primary_orphaned_socket_capacity = 0;
};

/** 所有資源が無いときだけ、次の Winsock 操作へ retryable な失敗を設定する。 */
bool ConfigureUdpTransportFailureInjectionForTesting(const UdpTransportFailureInjection& injection) noexcept;

/** 所有資源が無いことを確認し、失敗注入と累積診断値を初期化する。 */
bool ResetUdpTransportDiagnosticsForTesting() noexcept;

} // namespace acs::game::internal
