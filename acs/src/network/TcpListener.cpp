// TCP リスナー実装 (Win32 winsock2 / POSIX BSD sockets)
#include "network/TcpListener.h"
#include "network/Network.h"
#include "foundation/Compiler.h"
#include "foundation/Move.h"

#include <cstdio>

#if ACS_PLATFORM_WINDOWS
    #include "foundation/Platform.h"
    #include <winsock2.h>
    #include <ws2tcpip.h>
    using socket_t  = SOCKET;
    using socklen_t_compat = int;
    static constexpr socket_t kInvalidSocket = INVALID_SOCKET;
    #define ACS_SOCKET_ERROR_RETVAL SOCKET_ERROR
    static inline int LastSocketError() noexcept { return ::WSAGetLastError(); }
    static inline int CloseSocket(socket_t s) noexcept { return ::closesocket(s); }
#elif ACS_PLATFORM_POSIX
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <unistd.h>
    #include <fcntl.h>
    #include <errno.h>
    using socket_t  = int;
    using socklen_t_compat = ::socklen_t;
    static constexpr socket_t kInvalidSocket = -1;
    #define ACS_SOCKET_ERROR_RETVAL (-1)
    static inline int LastSocketError() noexcept { return errno; }
    static inline int CloseSocket(socket_t s) noexcept { return ::close(s); }
#else
    #error "TcpListener: unsupported platform"
#endif

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

    socket_t s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kInvalidSocket)
        return ACS_ERR_OS(IO, 221, "socket failed", static_cast<u32>(LastSocketError()));

    // SO_REUSEADDR
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                 reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    char ip_str[16];
    std::snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                  addr.octets[0], addr.octets[1], addr.octets[2], addr.octets[3]);
    if (::inet_pton(AF_INET, ip_str, &sa.sin_addr) != 1) {
        CloseSocket(s);
        return ACS_ERR(IO, 228, "invalid IP address");
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == ACS_SOCKET_ERROR_RETVAL) {
        u32 err = static_cast<u32>(LastSocketError());
        CloseSocket(s);
        return ACS_ERR_OS(IO, 222, "bind failed", err);
    }
    if (::listen(s, static_cast<int>(backlog)) == ACS_SOCKET_ERROR_RETVAL) {
        u32 err = static_cast<u32>(LastSocketError());
        CloseSocket(s);
        return ACS_ERR_OS(IO, 223, "listen failed", err);
    }

    TcpListener l;
    l._socket = static_cast<uptr>(s);
    return Result<TcpListener>(OkInit, Move(l));
}

Result<TcpConnection> TcpListener::Accept() noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 224, "listener not open");
    sockaddr_in sa{};
    socklen_t_compat len = sizeof(sa);
    socket_t cs = ::accept(static_cast<socket_t>(_socket),
                            reinterpret_cast<sockaddr*>(&sa), &len);
    if (cs == kInvalidSocket)
        return ACS_ERR_OS(IO, 225, "accept failed", static_cast<u32>(LastSocketError()));

    // sin_addr.s_addr (network byte order, 32bit) を 4 オクテットに分解
    IpAddress remote{};
    u32 ip_n = sa.sin_addr.s_addr;
    const u8* p = reinterpret_cast<const u8*>(&ip_n);
    remote.octets[0] = p[0];
    remote.octets[1] = p[1];
    remote.octets[2] = p[2];
    remote.octets[3] = p[3];
    remote.port      = ::ntohs(sa.sin_port);
    return Result<TcpConnection>(OkInit,
        TcpConnection::FromAccepted(static_cast<uptr>(cs), remote));
}

Result<void> TcpListener::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 226, "listener not open");
#if ACS_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<socket_t>(_socket), FIONBIO, &mode) == ACS_SOCKET_ERROR_RETVAL)
        return ACS_ERR_OS(IO, 227, "ioctlsocket failed",
                          static_cast<u32>(LastSocketError()));
#else
    int flags = ::fcntl(static_cast<socket_t>(_socket), F_GETFL, 0);
    if (flags < 0) return ACS_ERR_OS(IO, 227, "fcntl(F_GETFL) failed",
                                      static_cast<u32>(errno));
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(static_cast<socket_t>(_socket), F_SETFL, flags) < 0)
        return ACS_ERR_OS(IO, 227, "fcntl(F_SETFL) failed",
                          static_cast<u32>(errno));
#endif
    return Ok();
}

void TcpListener::Close() noexcept {
    if (_socket != ~uptr{0}) {
        CloseSocket(static_cast<socket_t>(_socket));
        _socket = ~uptr{0};
    }
}

} // namespace acs
