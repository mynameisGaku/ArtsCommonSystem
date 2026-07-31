// SPDX-License-Identifier: Apache-2.0
// TCP リスナー実装
#include "network/TcpListener.h"
#include "network/Network.h"
#include "foundation/Platform.h"
#include "foundation/Move.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

namespace {

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

/** ソケットが開いていれば閉じてから破棄する。 */
FTcpListener::~FTcpListener() noexcept {
    Close();
}

/** ムーブ構築する (ソケットの所有権を奪い、元を無効化する)。 */
FTcpListener::FTcpListener(FTcpListener&& o) noexcept : m_Socket(o.m_Socket) {
    o.m_Socket = ~uptr{0};
}

/** ムーブ代入する (自分のソケットを閉じてから所有権を奪う)。 */
FTcpListener& FTcpListener::operator=(FTcpListener&& o) noexcept {
    if (this == &o) return *this;
    Close();
    m_Socket = o.m_Socket;
    o.m_Socket = ~uptr{0};
    return *this;
}

/** 指定アドレス/ポートで socket→bind→listen を実行し、待ち受け中のリスナーを返す。 */
TResult<FTcpListener> FTcpListener::Listen(FIpAddress addr, u16 port, u32 backlog) noexcept {
    if (!FNetwork::IsInitialized())
        return ACS_ERR(IO, 220, "Network::Init() not called");

    const SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 221, "socket failed", static_cast<u32>(::WSAGetLastError()));

    // SO_REUSEADDR を有効化（再起動時の TIME_WAIT 対策）
    const int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

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
        return ACS_ERR_OS(IO, 222, "bind failed", err);
    }
    if (::listen(s, static_cast<int>(backlog)) == SOCKET_ERROR) {
        const u32 err = static_cast<u32>(::WSAGetLastError());
        ::closesocket(s);
        return ACS_ERR_OS(IO, 223, "listen failed", err);
    }

    FTcpListener l;
    l.m_Socket = static_cast<uptr>(s);
    return TResult<FTcpListener>(OkInit, Move(l));
}

/** 1 接続を accept し、リモートアドレスを設定した TcpConnection を返す。 */
TResult<FTcpConnection> FTcpListener::Accept() noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 224, "listener not open");
    sockaddr_in sa{};
    int len = sizeof(sa);
    const SOCKET cs = ::accept(static_cast<SOCKET>(m_Socket),
                          reinterpret_cast<sockaddr*>(&sa), &len);
    if (cs == INVALID_SOCKET)
        return ACS_ERR_OS(IO, 225, "accept failed", static_cast<u32>(::WSAGetLastError()));
    /** 接続元を ACS のアドレス値へ変換する。 */
    const FIpAddress remote = ToIpAddress(sa);
    return TResult<FTcpConnection>(OkInit, FTcpConnection::FromAccepted(static_cast<uptr>(cs), remote));
}

/** ソケットのノンブロッキングモードを切り替える。 */
TResult<void> FTcpListener::SetNonBlocking(bool enable) noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 226, "listener not open");
    u_long mode = enable ? 1 : 0;
    if (::ioctlsocket(static_cast<SOCKET>(m_Socket), FIONBIO, &mode) == SOCKET_ERROR)
        return ACS_ERR_OS(IO, 227, "ioctlsocket failed",
                          static_cast<u32>(::WSAGetLastError()));
    return Ok();
}

/** OS が割り当てたローカル IPv4 アドレスとポートを取得する。 */
TResult<FIpAddress> FTcpListener::LocalAddress() const noexcept {
    if (m_Socket == ~uptr{0}) return ACS_ERR(IO, 228, "listener not open");
    sockaddr_in address{};
    int length = sizeof(address);
    if (::getsockname(static_cast<SOCKET>(m_Socket), reinterpret_cast<sockaddr*>(&address), &length) == SOCKET_ERROR) {
        return ACS_ERR_OS(IO, 229, "getsockname failed", static_cast<u32>(::WSAGetLastError()));
    }
    return TResult<FIpAddress>(OkInit, ToIpAddress(address));
}

/** ソケットが開いていれば閉じて無効状態にする (多重呼び出し安全)。 */
void FTcpListener::Close() noexcept {
    if (m_Socket != ~uptr{0}) {
        ::closesocket(static_cast<SOCKET>(m_Socket));
        m_Socket = ~uptr{0};
    }
}

} // namespace acs
