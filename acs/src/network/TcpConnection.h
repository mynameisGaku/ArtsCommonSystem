// SPDX-License-Identifier: Apache-2.0
// TCP 接続（送信・受信）
//
// 使い方 (クライアント側):
//   auto cr = FTcpConnection::Connect(FIpAddress::FromString("127.0.0.1"), 8080);
//   if (cr.IsErr()) { ... }
//   FTcpConnection& c = cr.Value();
//   c.Send(data, size);
//   isize n = c.Recv(buf, sizeof(buf));
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

class FTcpConnection {
public:
    FTcpConnection() noexcept = default;
    ~FTcpConnection() noexcept;

    FTcpConnection(const FTcpConnection&) = delete;
    FTcpConnection& operator=(const FTcpConnection&) = delete;
    FTcpConnection(FTcpConnection&& o) noexcept;
    FTcpConnection& operator=(FTcpConnection&& o) noexcept;

    // 指定アドレス/ポートに接続
    static TResult<FTcpConnection> Connect(FIpAddress addr, u16 port) noexcept;

    // 内部用: FTcpListener::Accept から SOCKET を受け取って構築
    static FTcpConnection FromAccepted(uptr socket, FIpAddress remote) noexcept;

    // 切断（デストラクタでも呼ばれる）
    void Close() noexcept;

    // バッファを送信。送れたバイト数を返す（部分送信あり）。失敗時は -1。
    isize Send(const void* data, usize size) noexcept;

    // バッファに受信。受信バイト数を返す（0 なら相手が切断、-1 はエラー）
    isize Recv(void* buf, usize size) noexcept;

    // ノンブロッキングモードに切り替え
    TResult<void> SetNonBlocking(bool enable) noexcept;

    bool      IsValid()  const noexcept { return _socket != ~uptr{0}; }
    FIpAddress Remote()   const noexcept { return _remote; }

private:
    uptr      _socket = ~uptr{0};   // SOCKET (~0 を無効値とする)
    FIpAddress _remote {};
};

} // namespace acs
