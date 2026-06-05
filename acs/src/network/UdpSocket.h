// SPDX-License-Identifier: Apache-2.0
// UDP ソケット（コネクションレス）
//
// 使い方:
//   auto sr = FUdpSocket::Bind(IpAddress::Any(), 8080);
//   if (sr.IsErr()) { ... }
//   FUdpSocket& s = sr.Value();
//
//   IpAddress from{};
//   isize n = s.RecvFrom(buf, sizeof(buf), from);
//
//   s.SendTo(IpAddress::FromString("192.168.0.1"), 8080, data, size);
#pragma once

#include "foundation/Types.h"
#include "foundation/Result.h"
#include "network/IpAddress.h"

namespace acs {

/**
 * コネクションレスな UDP ソケット。
 *
 * @details
 * Bind() で生成し、SendTo()/RecvFrom() でデータグラムを送受信する。OS の
 * ソケットハンドルを単独所有する non-copy / move-only 型で、デストラクタや
 * Close() で確実にハンドルを閉じる。内部ハンドルは ~uptr{0} を無効値として持つ。
 */
class FUdpSocket {
public:
    /** 無効なソケット (ハンドル未確保) を構築する。 */
    FUdpSocket() noexcept = default;

    /** ソケットを閉じて破棄する。 */
    ~FUdpSocket() noexcept;

    /** コピー禁止 (OS ソケットハンドルを単独所有するため)。 */
    FUdpSocket(const FUdpSocket&) = delete;

    /** コピー代入も禁止。 */
    FUdpSocket& operator=(const FUdpSocket&) = delete;

    /**
     * ムーブ構築する (移動元のハンドルを奪い、移動元は無効化する)。
     *
     * @param o 移動元ソケット (ムーブ後は無効状態になる)。
     */
    FUdpSocket(FUdpSocket&& o) noexcept;

    /**
     * ムーブ代入する (自身の既存ハンドルを閉じてから移動元のハンドルを奪う)。
     *
     * @param o 移動元ソケット (ムーブ後は無効状態になる)。
     * @return 自身への参照。
     */
    FUdpSocket& operator=(FUdpSocket&& o) noexcept;

    /**
     * 指定アドレス/ポートにバインドした UDP ソケットを生成する。
     *
     * @details
     * 受信専用なら受信ポートを指定し、送信専用なら port=0 を渡して OS にポートを
     * 任せる。事前に Network::Init() が呼ばれている必要がある。
     * @param addr バインドするローカルアドレス (Any() で全インターフェイス)。
     * @param port バインドするローカルポート (0 で OS 任せ)。
     * @return 成功なら生成した FUdpSocket、失敗ならエラー。
     */
    static TResult<FUdpSocket> Bind(IpAddress addr, u16 port) noexcept;

    /**
     * 指定先へデータグラムを送信する。
     *
     * @param dst_addr 送信先 IPv4 アドレス。
     * @param dst_port 送信先ポート。
     * @param data 送信するデータの先頭。
     * @param size 送信するバイト数。
     * @return 送信できたバイト数、失敗時は -1。
     */
    isize SendTo(IpAddress dst_addr, u16 dst_port, const void* data, usize size) noexcept;

    /**
     * データグラムを受信する。
     *
     * @param buf 受信データを書き込むバッファ。
     * @param size buf の容量 (バイト)。
     * @param from 送信元アドレス/ポートの書き込み先 (出力)。
     * @return 受信できたバイト数、失敗時は -1。
     */
    isize RecvFrom(void* buf, usize size, IpAddress& from) noexcept;

    /**
     * ノンブロッキングモードを設定する。
     *
     * @param enable true でノンブロッキング、false でブロッキング。
     * @return 成功なら空の TResult、失敗ならエラー。
     */
    TResult<void> SetNonBlocking(bool enable) noexcept;

    /** ソケットを閉じてハンドルを無効化する (多重呼び出し安全)。 */
    void Close() noexcept;

    /**
     * 有効なソケットハンドルを保持しているかを返す。
     *
     * @return 有効なら true。
     */
    bool IsValid() const noexcept { return m_Socket != ~uptr{0}; }

private:
    /** OS ソケットハンドル (~uptr{0} は無効値)。 */
    uptr m_Socket = ~uptr{0};
};

} // namespace acs
