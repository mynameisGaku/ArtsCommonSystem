// UDP ソケット実装 (Win32 winsock2 / POSIX BSD sockets)
#include "network/UdpSocket.h"
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
    #error "UdpSocket: unsupported platform"
#endif

namespace acs {

namespace {
// IpAddress の 4 オクテットを sockaddr_in に書き込む
inline bool WriteIpToSockaddr(const IpAddress& addr, sockaddr_in& sa) noexcept {
    char ip_str[16];
    std::snprintf(ip_str, sizeof(ip_str), "%u.%u.%u.%u",
                  addr.octets[0], addr.octets[1], addr.octets[2], addr.octets[3]);
    return ::inet_pton(AF_INET, ip_str, &sa.sin_addr) == 1;
}

inline void ReadIpFromSockaddr(const sockaddr_in& sa, IpAddress& out) noexcept {
    u32 ip_n = sa.sin_addr.s_addr;
    const u8* p = reinterpret_cast<const u8*>(&ip_n);
    out.octets[0] = p[0];
    out.octets[1] = p[1];
    out.octets[2] = p[2];
    out.octets[3] = p[3];
    out.port      = ::ntohs(sa.sin_port);
}
} // namespace

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

    socket_t s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket)
        return ACS_ERR_OS(IO, 231, "socket failed", static_cast<u32>(LastSocketError()));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(port);
    if (!WriteIpToSockaddr(addr, sa)) {
        CloseSocket(s);
        return ACS_ERR(IO, 235, "invalid IP address");
    }

    if (::bind(s, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) == ACS_SOCKET_ERROR_RETVAL) {
        u32 err = static_cast<u32>(LastSocketError());
        CloseSocket(s);
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
    if (!WriteIpToSockaddr(dst_addr, sa)) return -1;
#if ACS_PLATFORM_WINDOWS
    int n = ::sendto(static_cast<socket_t>(_socket),
                     static_cast<const char*>(data), static_cast<int>(size), 0,
                     reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
#else
    isize n = ::sendto(static_cast<socket_t>(_socket), data, size, 0,
                       reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
#endif
    return (n == ACS_SOCKET_ERROR_RETVAL) ? -1 : n;
}

isize UdpSocket::RecvFrom(void* buf, usize size, IpAddress& from) noexcept {
    if (_socket == ~uptr{0}) return -1;
    sockaddr_in sa{};
    socklen_t_compat len = sizeof(sa);
#if ACS_PLATFORM_WINDOWS
    int n = ::recvfrom(static_cast<socket_t>(_socket),
                       static_cast<char*>(buf), static_cast<int>(size), 0,
                       reinterpret_cast<sockaddr*>(&sa), &len);
#else
    isize n = ::recvfrom(static_cast<socket_t>(_socket), buf, size, 0,
                         reinterpret_cast<sockaddr*>(&sa), &len);
#endif
    if (n == ACS_SOCKET_ERROR_RETVAL) return -1;
    ReadIpFromSockaddr(sa, from);
    return n;
}

Result<void> UdpSocket::SetNonBlocking(bool enable) noexcept {
    if (_socket == ~uptr{0}) return ACS_ERR(IO, 233, "socket not open");
#if ACS_PLATFORM_WINDOWS
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<socket_t>(_socket), FIONBIO, &mode) == ACS_SOCKET_ERROR_RETVAL)
        return ACS_ERR_OS(IO, 234, "ioctlsocket failed",
                          static_cast<u32>(LastSocketError()));
#else
    int flags = ::fcntl(static_cast<socket_t>(_socket), F_GETFL, 0);
    if (flags < 0) return ACS_ERR_OS(IO, 234, "fcntl(F_GETFL) failed",
                                      static_cast<u32>(errno));
    flags = enable ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    if (::fcntl(static_cast<socket_t>(_socket), F_SETFL, flags) < 0)
        return ACS_ERR_OS(IO, 234, "fcntl(F_SETFL) failed",
                          static_cast<u32>(errno));
#endif
    return Ok();
}

void UdpSocket::Close() noexcept {
    if (_socket != ~uptr{0}) {
        CloseSocket(static_cast<socket_t>(_socket));
        _socket = ~uptr{0};
    }
}

} // namespace acs
