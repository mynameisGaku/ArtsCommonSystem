// SPDX-License-Identifier: Apache-2.0
// UDP ソケット実装
#include "network/UdpSocket.h"
#include "network/Network.h"
#include "foundation/Limits.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

namespace {

/** WinSock の長さ引数で安全に表せる最大バイト数。 */
constexpr usize kMaximumSocketBufferSize = static_cast<usize>(TNumLimits<i32>::Max());

/** 空データグラム送信を OS へ渡すための参照可能な 1 バイト。 */
constexpr char kEmptyDatagramByte = '\0';

/** WinSock へ渡せる領域と長さの組かを返す。 */
bool IsSocketBufferValid(const void* buffer, usize size) noexcept {
    return size <= kMaximumSocketBufferSize && (buffer != nullptr || size == 0);
}

/** WinSock の IPv4 値を ACS のホストバイト順値へ変換する。 */
FIpAddress ToIpAddress(const sockaddr_in& address) noexcept {
    FIpAddress result{};
    result.octets[0] = address.sin_addr.S_un.S_un_b.s_b1;
    result.octets[1] = address.sin_addr.S_un.S_un_b.s_b2;
    result.octets[2] = address.sin_addr.S_un.S_un_b.s_b3;
    result.octets[3] = address.sin_addr.S_un.S_un_b.s_b4;
    result.port = ::ntohs(address.sin_port);
    return result;
}

} // namespace

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
    if (!CNetwork::IsInitialized())
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
isize FUdpSocket::SendTo(FIpAddress dst_addr, u16 dst_port, const void* data, usize size) noexcept {
    if (m_Socket == ~uptr{0} || !IsSocketBufferValid(data, size)) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = ::htons(dst_port);
    sa.sin_addr.S_un.S_un_b.s_b1 = dst_addr.octets[0];
    sa.sin_addr.S_un.S_un_b.s_b2 = dst_addr.octets[1];
    sa.sin_addr.S_un.S_un_b.s_b3 = dst_addr.octets[2];
    sa.sin_addr.S_un.S_un_b.s_b4 = dst_addr.octets[3];
    /** 空データグラムでも WinSock に非 null の領域を渡す。 */
    const char* send_buffer = size == 0 ? &kEmptyDatagramByte : static_cast<const char*>(data);
    const int n = ::sendto(static_cast<SOCKET>(m_Socket), send_buffer, static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
    return (n == SOCKET_ERROR) ? -1 : n;
}

/** データグラムを受信し、送信元アドレスを from に書き込む。 */
isize FUdpSocket::RecvFrom(void* buf, usize size, FIpAddress& from) noexcept {
    if (m_Socket == ~uptr{0} || !IsSocketBufferValid(buf, size)) return -1;
    sockaddr_in sa{};
    int len = sizeof(sa);
    /** 空データグラムでも WinSock に非 null の領域を渡す。 */
    char empty_datagram_byte = '\0';
    /** 受信サイズに応じて利用側領域か空データグラム用領域を選ぶ。 */
    char* receive_buffer = size == 0 ? &empty_datagram_byte : static_cast<char*>(buf);
    const int n = ::recvfrom(static_cast<SOCKET>(m_Socket), receive_buffer, static_cast<int>(size), 0, reinterpret_cast<sockaddr*>(&sa), &len);
    if (n == SOCKET_ERROR) return -1;
    from = ToIpAddress(sa);
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

/** OS が割り当てたローカル IPv4 アドレスとポートを取得する。 */
TResult<FIpAddress> FUdpSocket::LocalAddress() const noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 235, "socket not open");
    sockaddr_in address{};
    int length = sizeof(address);
    if (::getsockname(static_cast<SOCKET>(m_Socket), reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        return ACS_ERR_OS(IO, 236, "getsockname failed", static_cast<u32>(::WSAGetLastError()));
    }
    return TResult<FIpAddress>(OkInit, ToIpAddress(address));
}

/** ソケットを閉じてハンドルを無効化する (多重呼び出し安全)。 */
void FUdpSocket::Close() noexcept {
    if (m_Socket != ~uptr{0}) {
        ::closesocket(static_cast<SOCKET>(m_Socket));
        m_Socket = ~uptr{0};
    }
}

} // namespace acs
