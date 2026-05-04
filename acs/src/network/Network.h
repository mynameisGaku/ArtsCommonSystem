// ネットワークサブシステム初期化
//
// 使い方: アプリ起動時に一度 Network::Init() を呼ぶ。Application が
//         Audio/Network 系を自動初期化することは無いので、明示的に呼ぶこと。
//
// プラットフォーム:
//   Windows: WSAStartup 内部で呼ぶ
//   POSIX  : Init / Shutdown は no-op (BSD sockets は OS が管理)
//
// TcpConnection / TcpListener / UdpSocket の実装は現在 Windows 限定。
// POSIX 経路は将来 BSD sockets ifdef で追加予定。
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"

namespace acs {

class Network {
public:
    // WinSock を初期化（多重 Init は OK、内部で参照カウント）
    static Result<void> Init() noexcept;
    // 解放
    static void Shutdown() noexcept;
    // 初期化済みか
    static bool IsInitialized() noexcept;
};

} // namespace acs
