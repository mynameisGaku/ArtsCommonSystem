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

// IPv4 アドレス + ポート
struct IpAddress {
    u8  octets[4] = {0, 0, 0, 0};
    u16 port      = 0;

    // 「0.0.0.0」(全インターフェイス)
    static IpAddress Any() noexcept   { return IpAddress{}; }
    // 「127.0.0.1」(ループバック)
    static IpAddress Loopback() noexcept {
        IpAddress a;
        a.octets[0] = 127;
        a.octets[3] = 1;
        return a;
    }

    // "192.168.0.1" のような文字列から生成（不正な書式は 0.0.0.0 を返す）
    static IpAddress FromString(const char* dotted) noexcept;
};

} // namespace acs
