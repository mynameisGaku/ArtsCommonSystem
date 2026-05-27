// SPDX-License-Identifier: Apache-2.0
// UDP ソケット（コネクションレス）
//
// 使い方:
//   auto sr = FUdpSocket::Bind(IpAddress::Any(), 8080);
//   if (sr.IsErr()) { ... }
//   FUdpSocket& s = sr.Value();
//
//   IpAddress from{};
//   isize n = s.RecvFrom(buf, sizeof(buf), from);
//
//   s.SendTo(IpAddress::FromString("192.168.0.1"), 8080, data, size);
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

class FUdpSocket {
public:
    FUdpSocket() noexcept = default;
    ~FUdpSocket() noexcept;

    FUdpSocket(const FUdpSocket&) = delete;
    FUdpSocket& operator=(const FUdpSocket&) = delete;
    FUdpSocket(FUdpSocket&& o) noexcept;
    FUdpSocket& operator=(FUdpSocket&& o) noexcept;

    // 指定アドレス/ポートにバインド（addr=Any() で全インターフェイス）
    // 受信専用なら port を指定、送信専用なら port=0 で OS 任せ
    static TResult<FUdpSocket> Bind(IpAddress addr, u16 port) noexcept;

    // 指定先に送信（送信バイト数、失敗時 -1）
    isize SendTo(IpAddress dst_addr, u16 dst_port, const void* data, usize size) noexcept;

    // 受信（受信バイト数、from に送信元アドレスを書く、失敗時 -1）
    isize RecvFrom(void* buf, usize size, IpAddress& from) noexcept;

    TResult<void> SetNonBlocking(bool enable) noexcept;

    void Close() noexcept;
    bool IsValid() const noexcept { return _socket != ~uptr{0}; }

private:
    uptr _socket = ~uptr{0};
};

} // namespace acs
