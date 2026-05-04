// TCP 接続実装 (Win32 winsock2 / POSIX BSD sockets)
#include "network/TcpConnection.h"
#include "network/Network.h"
#include "foundation/Compiler.h"

#include <cstdio>

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t = SOCKET;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    #define ACS_SOCKET_ERROR_RETVAL SOCKET_ERROR
    static inline int LastSocketError() noexcept { return ::WSAGetLastError(); }
    static inline int CloseSocket(socket_t s) noexcept { return ::closesocket(s); }
#elif ACS_PLATFORM_POSIX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <netinet/tcp.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t = int;
    static constexpr socket_t kInvalidSocket = -1;
    #define ACS_SOCKET_ERROR_RETVAL (-1)
    static inline int LastSocketError() noexcept { return errno; }
    static inline int CloseSocket(socket_t s) noexcept { return ::close(s); }
    // Win32 互換シンボル
    static constexpr int SD_BOTH = SHUT_RDWR;
#else
    #error "TcpConnection: unsupported platform"
#endif

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

    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket)
        return ACS_ERR_OS(IO, 211, "socket failed", static_cast<u32>(LastSocketError()));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    // octets[0..3] を 32bit に詰めて inet_pton 経由で代入 (Win/POSIX 共通)
    char ip_str[16];
    std::snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                  addr.octets[0], addr.octets[1], addr.octets[2], addr.octets[3]);
    if (::inet_pton(AF_INET, ip_str, &sa.sin_addr) != 1) {
        CloseSocket(s);
        return ACS_ERR(IO, 215, "invalid IP address");
    }

    if (::connect(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == ACS_SOCKET_ERROR_RETVAL) {
        u32 err = static_cast<u32>(LastSocketError());
        CloseSocket(s);
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
        ::shutdown(static_cast<socket_t>(_socket), SD_BOTH);
        CloseSocket(static_cast<socket_t>(_socket));
        _socket = ~uptr{0};
    }
}

isize TcpConnection::Send(const void* data, usize size) noexcept {
    if (_socket == ~uptr{0}) return -1;
#if ACS_PLATFORM_WINDOWS
    int n = ::send(static_cast<socket_t>(_socket), static_cast<const char*>(data),
                   static_cast<int>(size), 0);
#else
    isize n = ::send(static_cast<socket_t>(_socket), data, size, 0);
#endif
    if (n == ACS_SOCKET_ERROR_RETVAL) return -1;
    return n;
}

isize TcpConnection::Recv(void* buf, usize size) noexcept {
    if (_socket == ~uptr{0}) return -1;
#if ACS_PLATFORM_WINDOWS
    int n = ::recv(static_cast<socket_t>(_socket), static_cast<char*>(buf),
                   static_cast<int>(size), 0);
#else
    isize n = ::recv(static_cast<socket_t>(_socket), buf, size, 0);
#endif
    if (n == ACS_SOCKET_ERROR_RETVAL) return -1;
    return n;  // 0 は相手切断
}

Result<void> TcpConnection::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 213, "socket not open");
#if ACS_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<socket_t>(_socket), FIONBIO, &mode) == ACS_SOCKET_ERROR_RETVAL)
        return ACS_ERR_OS(IO, 214, "ioctlsocket failed",
                          static_cast<u32>(LastSocketError()));
#else
    int flags = ::fcntl(static_cast<socket_t>(_socket), F_GETFL, 0);
    if (flags < 0) return ACS_ERR_OS(IO, 214, "fcntl(F_GETFL) failed",
                                      static_cast<u32>(errno));
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(static_cast<socket_t>(_socket), F_SETFL, flags) < 0)
        return ACS_ERR_OS(IO, 214, "fcntl(F_SETFL) failed",
                          static_cast<u32>(errno));
#endif
    return Ok();
}

} // namespace acs
