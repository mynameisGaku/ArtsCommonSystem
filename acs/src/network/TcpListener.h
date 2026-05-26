// SPDX-License-Identifier: Apache-2.0
// TCP リスナー（接続を待ち受ける側）
//
// 使い方 (サーバ側):
//   auto lr = FTcpListener::Listen(FIpAddress::Any(), 8080);
//   if (lr.IsErr()) { ... }
//   FTcpListener& l = lr.Value();
//   while (true) {
//       auto cr = l.Accept();
//       if (cr.IsOk()) HandleClient(Move(cr.Value()));
//   }
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"
#include "network/TcpConnection.h"

namespace acs {

class FTcpListener {
public:
    FTcpListener() noexcept = default;
    ~FTcpListener() noexcept;

    FTcpListener(const FTcpListener&) = delete;
    FTcpListener& operator=(const FTcpListener&) = delete;
    FTcpListener(FTcpListener&& o) noexcept;
    FTcpListener& operator=(FTcpListener&& o) noexcept;

    // 指定アドレス/ポートで Listen を開始（addr=Any() で全インターフェイス）
    static TResult<FTcpListener> Listen(FIpAddress addr, u16 port, u32 backlog = 16) noexcept;

    // 1 接続を受け付ける（ブロック）
    TResult<FTcpConnection> Accept() noexcept;

    // ノンブロッキング切替
    TResult<void> SetNonBlocking(bool enable) noexcept;

    void Close() noexcept;

    bool IsValid() const noexcept { return _socket != ~uptr{0}; }

private:
    uptr _socket = ~uptr{0};
};

} // namespace acs
