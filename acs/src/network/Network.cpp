// ネットワーク共通（WSAStartup, IpAddress 文字列パース）
#include "network/Network.h"
#include "network/IpAddress.h"
#include "foundation/Platform.h"
#include "threading/Atomic.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

namespace {
Atomic<u32> g_init_count{0};
}

Result<void> Network::Init() noexcept {
    // 多重 Init は参照カウントで安全に
    if (g_init_count.FetchAdd(1) > 0) return Ok();
    WSADATA d{};
    int r = ::WSAStartup(MAKEWORD(2, 2), &d);
    if (r != 0) {
        g_init_count.FetchSub(1);
        return ACS_ERR_OS(IO, 200, "WSAStartup failed", static_cast<u32>(r));
    }
    return Ok();
}

void Network::Shutdown() noexcept {
    u32 prev = g_init_count.FetchSub(1);
    if (prev == 1) ::WSACleanup();
}

bool Network::IsInitialized() noexcept {
    return g_init_count.Load(MemoryOrder::Acquire) > 0;
}

// "192.168.0.1" 等の文字列を IpAddress に変換
IpAddress IpAddress::FromString(const char* dotted) noexcept {
    IpAddress a{};
    if (!dotted) return a;

    u32 cur = 0;
    int oct = 0;
    bool digit = false;
    while (*dotted && oct < 4) {
        if (*dotted >= '0' && *dotted <= '9') {
            cur = cur * 10 + static_cast<u32>(*dotted - '0');
            digit = true;
            if (cur > 255) return IpAddress{};  // 不正値
        } else if (*dotted == '.') {
            if (!digit) return IpAddress{};
            a.octets[oct++] = static_cast<u8>(cur);
            cur = 0;
            digit = false;
        } else {
            return IpAddress{};  // 不正な文字
        }
        ++dotted;
    }
    if (oct < 4 && digit) a.octets[oct++] = static_cast<u8>(cur);
    if (oct != 4) return IpAddress{};  // 4 octet 揃っていない
    return a;
}

} // namespace acs
