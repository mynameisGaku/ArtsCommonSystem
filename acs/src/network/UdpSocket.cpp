// SPDX-License-Identifier: Apache-2.0
// UDP ソケット実装
#include "network/UdpSocket.h"
#include "network/Network.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

/** ソケットを閉じて破棄する。 */
FUdpSocket::~FUdpSocket() noexcept {
    Close();
}

/** ムーブ構築する (移動元のハンドルを奪い、移動元を無効化する)。 */
FUdpSocket::FUdpSocket(FUdpSocket&& o) noexcept : m_Socket(o.m_Socket) {
    o.m_Socket = ~uptr{0};
}

/** ムーブ代入する (既存ハンドルを閉じてから移動元のハンドルを奪う)。 */
FUdpSocket& FUdpSocket::operator=(FUdpSocket&& o) noexcept {
    if (this == &o) return *this;
    Close();
    m_Socket = o.m_Socket;
    o.m_Socket = ~uptr{0};
    return *this;
}

/** 指定アドレス/ポートにバインドした UDP ソケットを生成する。 */
TResult<FUdpSocket> FUdpSocket::Bind(FIpAddress addr, u16 port) noexcept {
    if (!FNetwork::IsInitialized())
        return ACS_ERR(IO, 230, "Network::Init() not called");

    const SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 231, "socket failed", static_cast<u32>(::WSAGetLastError()));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    sa.sin_addr.S_un.S_un_b.s_b1 = addr.octets[0];
    sa.sin_addr.S_un.S_un_b.s_b2 = addr.octets[1];
    sa.sin_addr.S_un.S_un_b.s_b3 = addr.octets[2];
    sa.sin_addr.S_un.S_un_b.s_b4 = addr.octets[3];

    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        const u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 232, "bind failed", err);
    }

    FUdpSocket u;
    u.m_Socket = static_cast<uptr>(s);
    return TResult<FUdpSocket>(OkInit, Move(u));
}

/** 指定先へデータグラムを送信する。 */
isize FUdpSocket::SendTo(FIpAddress dst_addr, u16 dst_port,
                        const void* data, usize size) noexcept {
    if (m_Socket == ~uptr{0}) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(dst_port);
    sa.sin_addr.S_un.S_un_b.s_b1 = dst_addr.octets[0];
    sa.sin_addr.S_un.S_un_b.s_b2 = dst_addr.octets[1];
    sa.sin_addr.S_un.S_un_b.s_b3 = dst_addr.octets[2];
    sa.sin_addr.S_un.S_un_b.s_b4 = dst_addr.octets[3];
    const int n = ::sendto(static_cast<SOCKET>(m_Socket),
                     static_cast<const char*>(data), static_cast<int>(size), 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    return (n == SOCKET_ERROR) ? -1 : n;
}

/** データグラムを受信し、送信元アドレスを from に書き込む。 */
isize FUdpSocket::RecvFrom(void* buf, usize size, FIpAddress& from) noexcept {
    if (m_Socket == ~uptr{0}) return -1;
    sockaddr_in sa{};
    int len = sizeof(sa);
    const int n = ::recvfrom(static_cast<SOCKET>(m_Socket),
                       static_cast<char*>(buf), static_cast<int>(size), 0,
                       reinterpret_cast<sockaddr*>(&sa), &len);
    if (n == SOCKET_ERROR) return -1;
    from.octets[0] = sa.sin_addr.S_un.S_un_b.s_b1;
    from.octets[1] = sa.sin_addr.S_un.S_un_b.s_b2;
    from.octets[2] = sa.sin_addr.S_un.S_un_b.s_b3;
    from.octets[3] = sa.sin_addr.S_un.S_un_b.s_b4;
    from.port      = ::ntohs(sa.sin_port);
    return n;
}

/** ノンブロッキングモードを設定する。 */
TResult<void> FUdpSocket::SetNonBlocking(bool enable) noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 233, "socket not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(m_Socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 234, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

/** ソケットを閉じてハンドルを無効化する (多重呼び出し安全)。 */
void FUdpSocket::Close() noexcept {
    if (m_Socket != ~uptr{0}) {
        ::closesocket(static_cast<SOCKET>(m_Socket));
        m_Socket = ~uptr{0};
    }
}

} // namespace acs
