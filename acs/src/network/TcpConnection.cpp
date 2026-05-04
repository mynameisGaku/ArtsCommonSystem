// TCP 接続実装
#include "network/TcpConnection.h"
#include "network/Network.h"
#include "foundation/Platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

TcpConnection::~TcpConnection() noexcept {
    Close();
}

TcpConnection::TcpConnection(TcpConnection&& o) noexcept
    : _socket(o._socket), _remote(o._remote) {
    o._socket = ~uptr{0};
}

TcpConnection& TcpConnection::operator=(TcpConnection&& o) noexcept {
    if (this == &o) return *this;
    Close();
    _socket = o._socket;
    _remote = o._remote;
    o._socket = ~uptr{0};
    return *this;
}

Result<TcpConnection> TcpConnection::Connect(IpAddress addr, u16 port) noexcept {
    if (!Network::IsInitialized())
        return ACS_ERR(IO, 210, "Network::Init() not called");

    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 211, "socket failed", static_cast<u32>(::WSAGetLastError()));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    sa.sin_addr.S_un.S_un_b.s_b1 = addr.octets[0];
    sa.sin_addr.S_un.S_un_b.s_b2 = addr.octets[1];
    sa.sin_addr.S_un.S_un_b.s_b3 = addr.octets[2];
    sa.sin_addr.S_un.S_un_b.s_b4 = addr.octets[3];

    if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == SOCKET_ERROR) {
        u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 212, "connect failed", err);
    }

    TcpConnection c;
    c._socket = static_cast<uptr>(s);
    addr.port = port;
    c._remote = addr;
    return Result<TcpConnection>(OkInit, Move(c));
}

TcpConnection TcpConnection::FromAccepted(uptr socket, IpAddress remote) noexcept {
    TcpConnection c;
    c._socket = socket;
    c._remote = remote;
    return c;
}

void TcpConnection::Close() noexcept {
    if (_socket != ~uptr{0}) {
        ::shutdown(static_cast<SOCKET>(_socket), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(_socket));
        _socket = ~uptr{0};
    }
}

isize TcpConnection::Send(const void* data, usize size) noexcept {
    if (_socket == ~uptr{0}) return -1;
    int n = ::send(static_cast<SOCKET>(_socket), static_cast<const char*>(data),
                   static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) return -1;
    return n;
}

isize TcpConnection::Recv(void* buf, usize size) noexcept {
    if (_socket == ~uptr{0}) return -1;
    int n = ::recv(static_cast<SOCKET>(_socket), static_cast<char*>(buf),
                   static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) return -1;
    return n;  // 0 は相手切断
}

Result<void> TcpConnection::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 213, "socket not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(_socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 214, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

} // namespace acs
