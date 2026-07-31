// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

/**
 * TCP ストリーム接続を表す single-owner なソケットハンドル。
 *
 * @details
 * クライアントは Connect で接続し、サーバ側は FTcpListener::Accept の戻り値から
 * FromAccepted 経由で構築される。Send/Recv でバイト列を送受信し、Close または
 * デストラクタで切断する。OS の SOCKET を単独所有する non-copy / move-only 型で、
 * 無効値は ~uptr{0} (=INVALID_SOCKET 相当) を用いる。同じ接続への呼び出しは
 * 利用側で直列化し、接続を破棄するまで FNetwork の初期化を保つ。
 */
class FTcpConnection {
public:
    /** 無効な (未接続の) 接続を構築する。 */
    FTcpConnection() noexcept = default;

    /** デストラクタ。接続が開いていれば Close で切断する。 */
    ~FTcpConnection() noexcept;

    /** コピー禁止 (ソケットを単独所有するため)。 */
    FTcpConnection(const FTcpConnection&) = delete;

    /** コピー代入も禁止。 */
    FTcpConnection& operator=(const FTcpConnection&) = delete;

    /**
     * ムーブ構築する。
     *
     * @details ソケットの所有権を移し、移動元は無効値 (~uptr{0}) になる。
     * @param o 移動元の接続。
     */
    FTcpConnection(FTcpConnection&& o) noexcept;

    /**
     * ムーブ代入する。
     *
     * @details 既存の接続を Close してから所有権を移し、移動元は無効値になる。
     *          自己代入は無視する。
     * @param o 移動元の接続。
     * @return 自身への参照。
     */
    FTcpConnection& operator=(FTcpConnection&& o) noexcept;

    /**
     * 指定アドレス・ポートへ TCP 接続する。
     *
     * @details
     * FNetwork::Init() が未呼出ならエラーを返す。socket → connect を行い、成功すれば
     * remote に port をセットした接続を返す。失敗時はソケットを閉じて OS エラーを返す。
     * @param addr 接続先 IP アドレス。
     * @param port 接続先ポート番号。
     * @return 接続済みの FTcpConnection、または接続失敗を表すエラー。
     */
    static TResult<FTcpConnection> Connect(FIpAddress addr, u16 port) noexcept;

    /**
     * 受理済みソケットから接続を構築する (内部用)。
     *
     * @details FTcpListener::Accept が受理した SOCKET と相手アドレスを受け取って包む。
     * @param socket 受理済みソケットハンドル。
     * @param remote 相手側の IP アドレス。
     * @return 受理済みソケットを所有する FTcpConnection。
     */
    static FTcpConnection FromAccepted(uptr socket, FIpAddress remote) noexcept;

    /**
     * 接続を切断する (多重呼び出し安全。デストラクタからも呼ばれる)。
     *
     * @details 開いていれば shutdown(SD_BOTH) → closesocket し、ソケットを無効値に戻す。
     */
    void Close() noexcept;

    /**
     * バッファを送信する。
     *
     * @details 部分送信があり得る。未接続、size が 1 以上で null、WinSock の長さ上限超過、OS エラーは -1。
     * size が 0 なら領域を参照せず 0 を返す。
     * @param data 送信するデータの先頭ポインタ。
     * @param size 送信するバイト数。
     * @return 実際に送れたバイト数。事前条件または OS 失敗時は -1。
     */
    isize Send(const void* data, usize size) noexcept;

    /**
     * バッファへ受信する。
     *
     * @details 未接続、size が 1 以上で null、WinSock の長さ上限超過、OS エラーは -1。
     * size が 0 なら OS を呼ばず 0 を返し、それ以外で 0 なら相手切断を示す。
     * @param buf 受信先バッファの先頭ポインタ。
     * @param size 受信先バッファのバイト数。
     * @return 受信したバイト数。size が 1 以上の呼出しで 0 は相手切断。失敗時は -1。
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
     * @return 接続相手の FIpAddress。
     */
    FIpAddress Remote()   const noexcept { return m_Remote; }

private:
    /** OS のソケットハンドル (~uptr{0} を無効値とする)。 */
    uptr      m_Socket = ~uptr{0};

    /** 接続相手の IP アドレス。 */
    FIpAddress m_Remote {};
};

} // namespace acs
