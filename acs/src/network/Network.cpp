// SPDX-License-Identifier: Apache-2.0
// ネットワーク共通（WSAStartup, IpAddress 文字列パース）
#include "network/Network.h"
#include "network/IpAddress.h"
#include "foundation/Platform.h"
#include "threading/Atomic.h"

#include <winsock2.h>
#include <ws2tcpip.h>

namespace acs {

namespace {
TAtomic<u32> g_init_count{0};
}

TResult<void> Network::Init() noexcept {
    // 多重 Init は参照カウントで安全に。
    //
    // 旧実装は WSAStartup を呼ぶ前に FetchAdd(1) でカウントを進めていたため、
    //   (1) WSAStartup 失敗時に一旦カウントが増え、その隙に別スレッドの Init が
    //       count>0 を観測して「初期化済み」と誤判定し得る、
    //   (2) 失敗ロールバックを忘れるとカウントが恒久的にずれる、
    // という不均衡を招いた。ここではカウントは「実際に初期化が成立した時」だけ
    // 進めるよう CAS ループで前値を確認してから増やす。
    for (;;) {
        u32 cur = g_init_count.Load(EMemoryOrder::Acquire);
        if (cur > 0) {
            // 既に誰かが初期化済み。カウントだけ +1 して相乗りする。
            u32 expected = cur;
            if (g_init_count.CompareExchange(expected, cur + 1)) return Ok();
            continue;  // 競合で失敗したら再読み込み
        }
        // cur == 0: 自分が実初期化を担う。先に WSAStartup を成功させてから
        // 0→1 を CAS で確定させる。失敗してもカウントは 0 のまま不変。
        WSADATA d{};
        int r = ::WSAStartup(MAKEWORD(2, 2), &d);
        if (r != 0) {
            return ACS_ERR_OS(IO, 200, "WSAStartup failed", static_cast<u32>(r));
        }
        u32 expected = 0;
        if (g_init_count.CompareExchange(expected, 1)) return Ok();
        // 0→1 の CAS に負けた = 他スレッドが先に初期化を確定させた。
        // 自分が呼んだ WSAStartup の参照は WSACleanup で打ち消し、相乗りし直す。
        ::WSACleanup();
        // ループ先頭へ戻り、今度は cur>0 経路で +1 する。
    }
}

void Network::Shutdown() noexcept {
    // Init と対称に、1→0 の遷移を確定した時だけ WSACleanup を呼ぶ。
    // カウント 0 での Shutdown はアンダーフロー (u32 0→0xFFFFFFFF) を起こさず
    // 単に no-op とする（Init 不均衡や二重 Shutdown に対する安全策）。
    for (;;) {
        u32 cur = g_init_count.Load(EMemoryOrder::Acquire);
        if (cur == 0) return;  // 何も初期化されていない。アンダーフロー防止。
        u32 expected = cur;
        if (g_init_count.CompareExchange(expected, cur - 1)) {
            if (cur == 1) ::WSACleanup();  // 最後の参照が外れた時だけ解放
            return;
        }
        // 競合したら再読み込みして再試行
    }
}

bool Network::IsInitialized() noexcept {
    return g_init_count.Load(EMemoryOrder::Acquire) > 0;
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
