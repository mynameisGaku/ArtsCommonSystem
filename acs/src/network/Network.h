// SPDX-License-Identifier: Apache-2.0
// ネットワークサブシステム初期化（WSAStartup ラッパ）
//
// 使い方: アプリ起動時に一度 Network::Init() を呼ぶ。FApplication が
//         Audio/Network 系を自動初期化することは無いので、明示的に呼ぶこと。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

/**
 * ネットワークサブシステムの初期化・終了を司る WSAStartup ラッパ。
 *
 * @details
 * 全メンバが static。多重 Init は内部の参照カウントで安全に扱い、最後の Shutdown で
 * 実際に WSACleanup を呼ぶ。FApplication は自動初期化しないので明示的に呼ぶこと。
 */
class Network {
public:
    /**
     * WinSock を初期化する (多重呼び出し可)。
     *
     * @details
     * 内部の参照カウントを CAS で進め、カウントが 0 のときだけ WSAStartup を呼ぶ。
     * 既に初期化済みなら相乗りしてカウントのみ +1 する。
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
};

} // namespace acs
