// TCP 接続（送信・受信）
//
// 使い方 (クライアント側):
//   auto cr = TcpConnection::Connect(IpAddress::FromString("127.0.0.1"), 8080);
//   if (cr.IsErr()) { ... }
//   TcpConnection& c = cr.Value();
//   c.Send(data, size);
//   isize n = c.Recv(buf, sizeof(buf));
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

class TcpConnection {
public:
    TcpConnection() noexcept = default;
    ~TcpConnection() noexcept;

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&& o) noexcept;
    TcpConnection& operator=(TcpConnection&& o) noexcept;

    // 指定アドレス/ポートに接続
    static Result<TcpConnection> Connect(IpAddress addr, u16 port) noexcept;

    // 内部用: TcpListener::Accept から SOCKET を受け取って構築
    static TcpConnection FromAccepted(uptr socket, IpAddress remote) noexcept;

    // 切断（デストラクタでも呼ばれる）
    void Close() noexcept;

    // バッファを送信。送れたバイト数を返す（部分送信あり）。失敗時は -1。
    isize Send(const void* data, usize size) noexcept;

    // バッファに受信。受信バイト数を返す（0 なら相手が切断、-1 はエラー）
    isize Recv(void* buf, usize size) noexcept;

    // ノンブロッキングモードに切り替え
    Result<void> SetNonBlocking(bool enable) noexcept;

    bool      IsValid()  const noexcept { return _socket != ~uptr{0}; }
    IpAddress Remote()   const noexcept { return _remote; }

private:
    uptr      _socket = ~uptr{0};   // SOCKET (~0 を無効値とする)
    IpAddress _remote {};
};

} // namespace acs
