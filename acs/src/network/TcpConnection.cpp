// SPDX-License-Identifier: Apache-2.0
// TCP 接続実装
#include "network/TcpConnection.h"
#include "network/Network.h"
#include "foundation/Limits.h"
#include "foundation/Platform.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

namespace {

/** WinSock の長さ引数で安全に表せる最大バイト数。 */
constexpr usize kMaximumSocketBufferSize = static_cast<usize>(TNumLimits<i32>::Max());

/** WinSock へ渡せる領域と長さの組かを返す。 */
bool IsSocketBufferValid(const void* buffer, usize size) noexcept {
    return size <= kMaximumSocketBufferSize && (buffer != nullptr || size == 0);
}

} // namespace

FTcpConnection::~FTcpConnection() noexcept {
    Close();
}

FTcpConnection::FTcpConnection(FTcpConnection&& o) noexcept
    : m_Socket(o.m_Socket), m_Remote(o.m_Remote) {
    o.m_Socket = ~uptr{0};
}

FTcpConnection& FTcpConnection::operator=(FTcpConnection&& o) noexcept {
    if (this == &o) return *this;
    Close();
    m_Socket = o.m_Socket;
    m_Remote = o.m_Remote;
    o.m_Socket = ~uptr{0};
    return *this;
}

TResult<FTcpConnection> FTcpConnection::Connect(FIpAddress addr, u16 port) noexcept {
    if (!CNetwork::IsInitialized())
        return ACS_ERR(IO, 210, "Network::Init() not called");

    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
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
        const u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 212, "connect failed", err);
    }

    FTcpConnection c;
    c.m_Socket = static_cast<uptr>(s);
    addr.port = port;
    c.m_Remote = addr;
    return TResult<FTcpConnection>(OkInit, Move(c));
}

FTcpConnection FTcpConnection::FromAccepted(uptr socket, FIpAddress remote) noexcept {
    FTcpConnection c;
    c.m_Socket = socket;
    c.m_Remote = remote;
    return c;
}

void FTcpConnection::Close() noexcept {
    if (m_Socket != ~uptr{0}) {
        ::shutdown(static_cast<SOCKET>(m_Socket), SD_BOTH);
        ::closesocket(static_cast<SOCKET>(m_Socket));
        m_Socket = ~uptr{0};
    }
}

isize FTcpConnection::Send(const void* data, usize size) noexcept {
    if (m_Socket == ~uptr{0} || !IsSocketBufferValid(data, size)) return -1;
    if (size == 0) return 0;
    const int n = ::send(static_cast<SOCKET>(m_Socket), static_cast<const char*>(data), static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) return -1;
    return n;
}

isize FTcpConnection::Recv(void* buf, usize size) noexcept {
    if (m_Socket == ~uptr{0} || !IsSocketBufferValid(buf, size)) return -1;
    if (size == 0) return 0;
    const int n = ::recv(static_cast<SOCKET>(m_Socket), static_cast<char*>(buf), static_cast<int>(size), 0);
    if (n == SOCKET_ERROR) return -1;
    return n;
}

TResult<void> FTcpConnection::SetNonBlocking(bool enable) noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 213, "socket not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(m_Socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 214, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

} // namespace acs
