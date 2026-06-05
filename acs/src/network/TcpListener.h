// SPDX-License-Identifier: Apache-2.0
// TCP リスナー（接続を待ち受ける側）
//
// 使い方 (サーバ側):
//   auto lr = TcpListener::Listen(IpAddress::Any(), 8080);
//   if (lr.IsErr()) { ... }
//   TcpListener& l = lr.Value();
//   while (true) {
//       auto cr = l.Accept();
//       if (cr.IsOk()) HandleClient(Move(cr.Value()));
//   }
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"
#include "network/TcpConnection.h"

namespace acs {

/**
 * TCP 接続を待ち受けるリスナー (サーバ側)。
 *
 * @details
 * Listen() で bind/listen まで済んだソケットを生成し、Accept() で 1 接続ずつ
 * TcpConnection を受け取る。ソケットを単独所有する non-copy / move-only 型で、
 * ムーブ後の元オブジェクトは無効状態 (m_Socket = ~uptr{0}) になる。
 */
class TcpListener {
public:
    /** 無効状態 (ソケット未確保) で構築する。 */
    TcpListener() noexcept = default;

    /** ソケットが開いていれば閉じてから破棄する。 */
    ~TcpListener() noexcept;

    /** コピー禁止 (ソケットを単独所有するため)。 */
    TcpListener(const TcpListener&) = delete;

    /** コピー代入も禁止。 */
    TcpListener& operator=(const TcpListener&) = delete;

    /**
     * ムーブ構築する (ソケットの所有権を奪い、元を無効化する)。
     *
     * @param o ムーブ元のリスナー (実行後は無効状態になる)。
     */
    TcpListener(TcpListener&& o) noexcept;

    /**
     * ムーブ代入する (自分のソケットを閉じてから所有権を奪う)。
     *
     * @param o ムーブ元のリスナー (実行後は無効状態になる)。
     * @return *this への参照。
     */
    TcpListener& operator=(TcpListener&& o) noexcept;

    /**
     * 指定アドレス/ポートで Listen を開始する。
     *
     * @details
     * socket→SO_REUSEADDR→bind→listen を実行する。Network::Init() 未呼び出し、
     * または各 WinSock 呼び出しの失敗時はエラーを返す。
     * @param addr バインドするアドレス (IpAddress::Any() で全インターフェイス)。
     * @param port 待ち受けるポート番号。
     * @param backlog 接続キューの最大長 (既定 16)。
     * @return 成功なら待ち受け中の TcpListener、失敗ならエラー。
     */
    static TResult<TcpListener> Listen(IpAddress addr, u16 port, u32 backlog = 16) noexcept;

    /**
     * 1 接続を受け付ける (ブロッキング)。
     *
     * @details accept した接続を TcpConnection として返す。リモートのアドレス/ポートも設定される。
     * @return 成功なら受理した TcpConnection、失敗ならエラー。
     */
    TResult<TcpConnection> Accept() noexcept;

    /**
     * ソケットのノンブロッキングモードを切り替える。
     *
     * @param enable true でノンブロッキング、false でブロッキングにする。
     * @return 成功なら空の TResult、ioctlsocket 失敗ならエラー。
     */
    TResult<void> SetNonBlocking(bool enable) noexcept;

    /** ソケットが開いていれば閉じて無効状態にする (多重呼び出し安全)。 */
    void Close() noexcept;

    /**
     * 有効なソケットを保持しているかを返す。
     *
     * @return ソケットが開いていれば true。
     */
    bool IsValid() const noexcept { return m_Socket != ~uptr{0}; }

private:
    /** WinSock ソケットハンドル (~uptr{0} は無効)。 */
    uptr m_Socket = ~uptr{0};
};

} // namespace acs
