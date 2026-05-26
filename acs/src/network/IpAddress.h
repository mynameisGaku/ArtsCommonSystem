// SPDX-License-Identifier: Apache-2.0
// IPv4 アドレス（v1 では v6 は未対応）
//
// 使い方:
//   FIpAddress addr = FIpAddress::FromString("127.0.0.1");
//   FIpAddress any  = FIpAddress::Any();
//   FIpAddress lo   = FIpAddress::Loopback();
//   addr.port = 8080;
#pragma once

#include "foundation/Types.h"

namespace acs {

// IPv4 アドレス + ポート
struct FIpAddress {
    u8  octets[4] = {0, 0, 0, 0};
    u16 port      = 0;

    // 「0.0.0.0」(全インターフェイス)
    static FIpAddress Any() noexcept   { return FIpAddress{}; }
    // 「127.0.0.1」(ループバック)
    static FIpAddress Loopback() noexcept {
        FIpAddress a;
        a.octets[0] = 127;
        a.octets[3] = 1;
        return a;
    }

    // "192.168.0.1" のような文字列から生成（不正な書式は 0.0.0.0 を返す）
    static FIpAddress FromString(const char* dotted) noexcept;
};

} // namespace acs
