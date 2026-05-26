// SPDX-License-Identifier: Apache-2.0
// ネットワークサブシステム初期化（WSAStartup ラッパ）
//
// 使い方: アプリ起動時に一度 FNetwork::Init() を呼ぶ。FApplication が
//         Audio/FNetwork 系を自動初期化することは無いので、明示的に呼ぶこと。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

class FNetwork {
public:
    // WinSock を初期化（多重 Init は OK、内部で参照カウント）
    static TResult<void> Init() noexcept;
    // 解放
    static void Shutdown() noexcept;
    // 初期化済みか
    static bool IsInitialized() noexcept;
};

} // namespace acs
