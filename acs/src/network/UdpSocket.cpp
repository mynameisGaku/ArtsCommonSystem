// SPDX-License-Identifier: Apache-2.0
// UDP ソケット実装
#include "network/UdpSocket.h"
#include "network/Network.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

UdpSocket::~UdpSocket() noexcept {
    Close();
}

UdpSocket::UdpSocket(UdpSocket&& o) noexcept : _socket(o._socket) {
    o._socket = ~uptr{0};
}
UdpSocket& UdpSocket::operator=(UdpSocket&& o) noexcept {
    if (this == &o) return *this;
    Close();
    _socket = o._socket;
    o._socket = ~uptr{0};
    return *this;
}

Result<UdpSocket> UdpSocket::Bind(IpAddress addr, u16 port) noexcept {
    if (!Network::IsInitialized())
        return ACS_ERR(IO, 230, "Network::Init() not called");

    SOCKET s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
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
        u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 232, "bind failed", err);
    }

    UdpSocket u;
    u._socket = static_cast<uptr>(s);
    return Result<UdpSocket>(OkInit, Move(u));
}

isize UdpSocket::SendTo(IpAddress dst_addr, u16 dst_port,
                        const void* data, usize size) noexcept {
    if (_socket == ~uptr{0}) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(dst_port);
    sa.sin_addr.S_un.S_un_b.s_b1 = dst_addr.octets[0];
    sa.sin_addr.S_un.S_un_b.s_b2 = dst_addr.octets[1];
    sa.sin_addr.S_un.S_un_b.s_b3 = dst_addr.octets[2];
    sa.sin_addr.S_un.S_un_b.s_b4 = dst_addr.octets[3];
    int n = ::sendto(static_cast<SOCKET>(_socket),
                     static_cast<const char*>(data), static_cast<int>(size), 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    return (n == SOCKET_ERROR) ? -1 : n;
}

isize UdpSocket::RecvFrom(void* buf, usize size, IpAddress& from) noexcept {
    if (_socket == ~uptr{0}) return -1;
    sockaddr_in sa{};
    int len = sizeof(sa);
    int n = ::recvfrom(static_cast<SOCKET>(_socket),
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

Result<void> UdpSocket::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 233, "socket not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(_socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 234, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

void UdpSocket::Close() noexcept {
    if (_socket != ~uptr{0}) {
        ::closesocket(static_cast<SOCKET>(_socket));
        _socket = ~uptr{0};
    }
}

} // namespace acs
