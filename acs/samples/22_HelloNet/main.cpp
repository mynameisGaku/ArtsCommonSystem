// SPDX-License-Identifier: Apache-2.0
// ACS の Hello Net サンプル
//
// 動作:
//   ・スレッドで TCP echo サーバを起動 (port 8765, ループバック)
//   ・メインスレッドからクライアント接続して送受信
//   ・受信したテキストをコンソール出力

#include "network/Network.h"
#include "network/TcpListener.h"
#include "network/TcpConnection.h"
#include "threading/Thread.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cstring>

using namespace acs;

namespace {

void ServerThread(void* /*user*/) noexcept {
    auto lr = TcpListener::Listen(IpAddress::Any(), 8765);
    if (lr.IsErr()) {
        ACS_LOG_ERROR("Listener failed: %s", lr.Error().message);
        return;
    }
    auto& listener = lr.Value();

    auto cr = listener.Accept();
    if (cr.IsErr()) {
        ACS_LOG_ERROR("Accept failed: %s", cr.Error().message);
        return;
    }
    TcpConnection conn = Move(cr.Value());

    char buf[256];
    isize n = conn.Recv(buf, sizeof(buf));
    if (n > 0) {
        // 受信内容をそのまま echo back
        conn.Send(buf, static_cast<usize>(n));
    }
}

} // namespace

int main() {
    LogConfig lc{};
    lc.console = true;
    Logger::Init(lc);

    if (auto r = Network::Init(); r.IsErr()) {
        ACS_LOG_ERROR("Network::Init failed: %s", r.Error().message);
        return 1;
    }

    // サーバスレッド起動
    auto sr = Thread::Spawn(&ServerThread, nullptr);
    if (sr.IsErr()) {
        ACS_LOG_ERROR("Server thread spawn failed");
        return 2;
    }
    Thread server = Move(sr.Value());

    // 少し待ってから接続（Listener が起動するまで）
    SleepMs(100);

    auto cr = TcpConnection::Connect(IpAddress::Loopback(), 8765);
    if (cr.IsErr()) {
        ACS_LOG_ERROR("Connect failed: %s", cr.Error().message);
        server.Join();
        Network::Shutdown();
        return 3;
    }
    TcpConnection client = Move(cr.Value());

    const char* msg = "Hello, ACS Network!";
    client.Send(msg, ::strlen(msg));

    char reply[256] = {};
    isize n = client.Recv(reply, sizeof(reply) - 1);
    if (n > 0) {
        reply[n] = 0;
        ::printf("Echoed back: %s\n", reply);
    }

    server.Join();
    Network::Shutdown();
    Logger::Shutdown();
    return 0;
}
