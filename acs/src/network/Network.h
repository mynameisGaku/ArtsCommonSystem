// SPDX-License-Identifier: Apache-2.0
// ネットワークサブシステム初期化（WSAStartup ラッパ）
//
// 使い方: アプリ起動時に一度 FNetwork::Init() を呼ぶ。FApplication が
//         Audio/Network 系を自動初期化することは無いので、明示的に呼ぶこと。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

/** FNetwork が所有する Winsock 資源の診断スナップショット。 */
struct FNetworkDiagnostics {
    /** 現在の Init 参照数。 */
    u32 initialization_reference_count = 0;

    /** 再試行を待っている WSACleanup エラー。0 は保留なし。 */
    u32 pending_cleanup_error = 0;

    /** プロセス寿命中に観測した資源解放失敗の累積数。 */
    u64 resource_release_failure_count = 0;
};

/**
 * ネットワークサブシステムの初期化・終了を司る WSAStartup ラッパ。
 *
 * @details
 * 全メンバが static。多重 Init は内部の参照カウントで安全に扱い、最後の Shutdown で
 * 実際に WSACleanup を呼ぶ。FApplication は自動初期化しないので明示的に呼ぶこと。
 */
class FNetwork {
public:
    /**
     * WinSock を初期化する (多重呼び出し可)。
     *
     * @details
     * 内部の参照カウントと WSAStartup / WSACleanup を排他制御し、カウントが 0 の
     * ときだけ WSAStartup を呼ぶ。既に初期化済みなら参照数のみ +1 する。
     * @return 成功なら空の TResult、WSAStartup 失敗ならエラー。
     */
    static TResult<void> Init() noexcept;

    /**
     * 参照カウントを 1 減らし、最後の参照が外れたら WSACleanup を呼ぶ。
     *
     * @details カウントが 0 のときの呼び出しはアンダーフローを避けるため no-op。
     */
    static void Shutdown() noexcept;

    /**
     * 初期化済みかどうかを返す。
     *
     * @return 参照カウントが 1 以上なら true。
     */
    static bool IsInitialized() noexcept;

    /** 参照数・cleanup debt・資源解放失敗数を一貫した時点で取得する。 */
    static FNetworkDiagnostics CaptureDiagnostics() noexcept;
};

} // namespace acs
