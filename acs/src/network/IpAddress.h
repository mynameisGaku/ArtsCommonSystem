// SPDX-License-Identifier: Apache-2.0
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
struct FIpAddress {
    /** ドット区切り表記の各オクテット (octets[0] が最上位)。既定 0.0.0.0。 */
    u8  octets[4] = {0, 0, 0, 0};

    /** ポート番号 (ホストバイトオーダ)。既定 0。 */
    u16 port      = 0;

    /**
     * 全インターフェイスを表す 0.0.0.0 を返す。
     *
     * @return octets が全 0、port が 0 の FIpAddress。
     */
    static FIpAddress Any() noexcept   { return FIpAddress{}; }

    /**
     * ループバックアドレス 127.0.0.1 を返す。
     *
     * @return 127.0.0.1 (port は 0) を表す FIpAddress。
     */
    static FIpAddress Loopback() noexcept {
        FIpAddress a;
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
     * @return 解析した FIpAddress (不正なら 0.0.0.0)。
     */
    static FIpAddress FromString(const char* dotted) noexcept;
};

} // namespace acs
