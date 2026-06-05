// SPDX-License-Identifier: Apache-2.0
// IPv4 アドレス（v1 では v6 は未対応）
//
// 使い方:
//   IpAddress addr = IpAddress::FromString("127.0.0.1");
//   IpAddress any  = IpAddress::Any();
//   IpAddress lo   = IpAddress::Loopback();
//   addr.port = 8080;
#pragma once

#include "foundation/Types.h"

namespace acs {

/**
 * IPv4 アドレスとポートを保持する値型 (v1 では IPv6 未対応)。
 *
 * @details
 * octets[0..3] にドット区切り表記の各オクテットを格納し、port にポート番号を持つ。
 * 既定値は 0.0.0.0:0。Any()/Loopback()/FromString() で生成する。
 */
struct IpAddress {
    /** ドット区切り表記の各オクテット (octets[0] が最上位)。既定 0.0.0.0。 */
    u8  octets[4] = {0, 0, 0, 0};

    /** ポート番号 (ホストバイトオーダ)。既定 0。 */
    u16 port      = 0;

    /**
     * 全インターフェイスを表す 0.0.0.0 を返す。
     *
     * @return octets が全 0、port が 0 の IpAddress。
     */
    static IpAddress Any() noexcept   { return IpAddress{}; }

    /**
     * ループバックアドレス 127.0.0.1 を返す。
     *
     * @return 127.0.0.1 (port は 0) を表す IpAddress。
     */
    static IpAddress Loopback() noexcept {
        IpAddress a;
        a.octets[0] = 127;
        a.octets[3] = 1;
        return a;
    }

    /**
     * "192.168.0.1" のようなドット区切り文字列から生成する。
     *
     * @details
     * 4 オクテットが揃わない、各値が 255 を超える、数字とドット以外の文字を含む、
     * といった不正な書式の場合は 0.0.0.0 を返す。port は設定しない。
     * @param dotted ドット区切りの IPv4 文字列 (nullptr の場合は 0.0.0.0 を返す)。
     * @return 解析した IpAddress (不正なら 0.0.0.0)。
     */
    static IpAddress FromString(const char* dotted) noexcept;
};

} // namespace acs
