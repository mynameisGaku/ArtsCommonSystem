// SPDX-License-Identifier: Apache-2.0
// TCP リスナー（接続を待ち受ける側）
//
// 使い方 (サーバ側):
//   auto lr = TcpListener::Listen(IpAddress::Any(), 8080);
//   if (lr.IsErr()) { ... }
//   TcpListener& l = lr.Value();
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

class TcpListener {
public:
    TcpListener() noexcept = default;
    ~TcpListener() noexcept;

    TcpListener(const TcpListener&) = delete;
    TcpListener& operator=(const TcpListener&) = delete;
    TcpListener(TcpListener&& o) noexcept;
    TcpListener& operator=(TcpListener&& o) noexcept;

    // 指定アドレス/ポートで Listen を開始（addr=Any() で全インターフェイス）
    static Result<TcpListener> Listen(IpAddress addr, u16 port, u32 backlog = 16) noexcept;

    // 1 接続を受け付ける（ブロック）
    Result<TcpConnection> Accept() noexcept;

    // ノンブロッキング切替
    Result<void> SetNonBlocking(bool enable) noexcept;

    void Close() noexcept;

    bool IsValid() const noexcept { return _socket != ~uptr{0}; }

private:
    uptr _socket = ~uptr{0};
};

} // namespace acs
