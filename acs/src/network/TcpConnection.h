// SPDX-License-Identifier: Apache-2.0
// TCP 接続（送信・受信）
//
// 使い方 (クライアント側):
//   auto cr = TcpConnection::Connect(IpAddress::FromString("127.0.0.1"), 8080);
//   if (cr.IsErr()) { ... }
//   TcpConnection& c = cr.Value();
//   c.Send(data, size);
//   isize n = c.Recv(buf, sizeof(buf));
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

/**
 * TCP ストリーム接続を表す single-owner なソケットハンドル。
 *
 * @details
 * クライアントは Connect で接続し、サーバ側は TcpListener::Accept の戻り値から
 * FromAccepted 経由で構築される。Send/Recv でバイト列を送受信し、Close または
 * デストラクタで切断する。OS の SOCKET を単独所有する non-copy / move-only 型で、
 * 無効値は ~uptr{0} (=INVALID_SOCKET 相当) を用いる。
 */
class TcpConnection {
public:
    /** 無効な (未接続の) 接続を構築する。 */
    TcpConnection() noexcept = default;

    /** デストラクタ。接続が開いていれば Close で切断する。 */
    ~TcpConnection() noexcept;

    /** コピー禁止 (ソケットを単独所有するため)。 */
    TcpConnection(const TcpConnection&) = delete;

    /** コピー代入も禁止。 */
    TcpConnection& operator=(const TcpConnection&) = delete;

    /**
     * ムーブ構築する。
     *
     * @details ソケットの所有権を移し、移動元は無効値 (~uptr{0}) になる。
     * @param o 移動元の接続。
     */
    TcpConnection(TcpConnection&& o) noexcept;

    /**
     * ムーブ代入する。
     *
     * @details 既存の接続を Close してから所有権を移し、移動元は無効値になる。
     *          自己代入は無視する。
     * @param o 移動元の接続。
     * @return 自身への参照。
     */
    TcpConnection& operator=(TcpConnection&& o) noexcept;

    /**
     * 指定アドレス・ポートへ TCP 接続する。
     *
     * @details
     * Network::Init() が未呼出ならエラーを返す。socket → connect を行い、成功すれば
     * remote に port をセットした接続を返す。失敗時はソケットを閉じて OS エラーを返す。
     * @param addr 接続先 IP アドレス。
     * @param port 接続先ポート番号。
     * @return 接続済みの TcpConnection、または接続失敗を表すエラー。
     */
    static TResult<TcpConnection> Connect(IpAddress addr, u16 port) noexcept;

    /**
     * 受理済みソケットから接続を構築する (内部用)。
     *
     * @details TcpListener::Accept が受理した SOCKET と相手アドレスを受け取って包む。
     * @param socket 受理済みソケットハンドル。
     * @param remote 相手側の IP アドレス。
     * @return 受理済みソケットを所有する TcpConnection。
     */
    static TcpConnection FromAccepted(uptr socket, IpAddress remote) noexcept;

    /**
     * 接続を切断する (多重呼び出し安全。デストラクタからも呼ばれる)。
     *
     * @details 開いていれば shutdown(SD_BOTH) → closesocket し、ソケットを無効値に戻す。
     */
    void Close() noexcept;

    /**
     * バッファを送信する。
     *
     * @details 部分送信があり得る (要求サイズより少ないことがある)。未接続またはエラー時は -1。
     * @param data 送信するデータの先頭ポインタ。
     * @param size 送信するバイト数。
     * @return 実際に送れたバイト数。失敗時は -1。
     */
    isize Send(const void* data, usize size) noexcept;

    /**
     * バッファへ受信する。
     *
     * @details 未接続またはエラー時は -1、相手が切断した場合は 0 を返す。
     * @param buf 受信先バッファの先頭ポインタ。
     * @param size 受信先バッファのバイト数。
     * @return 受信したバイト数 (0 は相手切断)。失敗時は -1。
     */
    isize Recv(void* buf, usize size) noexcept;

    /**
     * ソケットをノンブロッキング/ブロッキングモードに切り替える。
     *
     * @details ioctlsocket(FIONBIO) を使う。未接続または失敗時はエラーを返す。
     * @param enable true でノンブロッキング、false でブロッキング。
     * @return 成功なら空の TResult、失敗なら OS エラー。
     */
    TResult<void> SetNonBlocking(bool enable) noexcept;

    /**
     * 接続が有効 (ソケットが開いている) かを返す。
     *
     * @return 有効なら true。
     */
    bool      IsValid()  const noexcept { return m_Socket != ~uptr{0}; }

    /**
     * 相手側の IP アドレスを返す。
     *
     * @return 接続相手の IpAddress。
     */
    IpAddress Remote()   const noexcept { return m_Remote; }

private:
    /** OS のソケットハンドル (~uptr{0} を無効値とする)。 */
    uptr      m_Socket = ~uptr{0};

    /** 接続相手の IP アドレス。 */
    IpAddress m_Remote {};
};

} // namespace acs
