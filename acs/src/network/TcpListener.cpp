// TCP リスナー実装
#include "network/TcpListener.h"
#include "network/Network.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

TcpListener::~TcpListener() noexcept {
    Close();
}

TcpListener::TcpListener(TcpListener&& o) noexcept : _socket(o._socket) {
    o._socket = ~uptr{0};
}
TcpListener& TcpListener::operator=(TcpListener&& o) noexcept {
    if (this == &o) return *this;
    Close();
    _socket = o._socket;
    o._socket = ~uptr{0};
    return *this;
}

Result<TcpListener> TcpListener::Listen(IpAddress addr, u16 port, u32 backlog) noexcept {
    if (!Network::IsInitialized())
        return ACS_ERR(IO, 220, "Network::Init() not called");

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 221, "socket failed", static_cast<u32>(::WSAGetLastError()));

    // SO_REUSEADDR を有効化（再起動時の TIME_WAIT 対策）
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

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
        return ACS_ERR_OS(IO, 222, "bind failed", err);
    }
    if (::listen(s, static_cast<int>(backlog)) == SOCKET_ERROR) {
        u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 223, "listen failed", err);
    }

    TcpListener l;
    l._socket = static_cast<uptr>(s);
    return Result<TcpListener>(OkInit, Move(l));
}

Result<TcpConnection> TcpListener::Accept() noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 224, "listener not open");
    sockaddr_in sa{};
    int len = sizeof(sa);
    SOCKET cs = ::accept(static_cast<SOCKET>(_socket),
                          reinterpret_cast<sockaddr*>(&sa), &len);
    if (cs == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 225, "accept failed", static_cast<u32>(::WSAGetLastError()));
    IpAddress remote{};
    remote.octets[0] = sa.sin_addr.S_un.S_un_b.s_b1;
    remote.octets[1] = sa.sin_addr.S_un.S_un_b.s_b2;
    remote.octets[2] = sa.sin_addr.S_un.S_un_b.s_b3;
    remote.octets[3] = sa.sin_addr.S_un.S_un_b.s_b4;
    remote.port      = ::ntohs(sa.sin_port);
    return Result<TcpConnection>(OkInit,
        TcpConnection::FromAccepted(static_cast<uptr>(cs), remote));
}

Result<void> TcpListener::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 226, "listener not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(_socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 227, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

void TcpListener::Close() noexcept {
    if (_socket != ~uptr{0}) {
        ::closesocket(static_cast<SOCKET>(_socket));
        _socket = ~uptr{0};
    }
}

} // namespace acs
