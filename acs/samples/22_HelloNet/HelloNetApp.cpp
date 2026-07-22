// SPDX-License-Identifier: Apache-2.0
// HelloNet — HelloNetApp 実装。
#include "HelloNetApp.h"
#include "EchoServer.h"

#include "network/Network.h"
#include "network/TcpConnection.h"
#include "threading/Thread.h"
#include "foundation/Log.h"

#include <cstdio>
#include <cstring>

using namespace acs;

namespace hellonet {

int FHelloNetApp::Run() noexcept {
    FLogConfig lc{};
    lc.console = true;
    FLogger::Init(lc);

    if (auto r = FNetwork::Init(); r.IsErr()) {
        ACS_LOG_ERROR("Network::Init failed: %s", r.Error().message);
        return 1;
    }

    auto sr = FThread::Spawn(&ServerThread, nullptr);
    if (sr.IsErr()) {
        ACS_LOG_ERROR("Server thread spawn failed");
        return 2;
    }
    FThread server = Move(sr.Value());

    // 少し待ってから接続 (Listener が起動するまで)
    SleepMs(100);

    auto cr = FTcpConnection::Connect(FIpAddress::Loopback(), kEchoPort);
    if (cr.IsErr()) {
        ACS_LOG_ERROR("Connect failed: %s", cr.Error().message);
        server.Join();
        FNetwork::Shutdown();
        return 3;
    }
    FTcpConnection client = Move(cr.Value());

    const char* msg = "Hello, ACS Network!";
    client.Send(msg, ::strlen(msg));

    char reply[256] = {};
    isize n = client.Recv(reply, sizeof(reply) - 1);
    if (n > 0) {
        reply[n] = 0;
        ::printf("Echoed back: %s\n", reply);
    }

    server.Join();
    FNetwork::Shutdown();
    FLogger::Shutdown();
    return 0;
}

} // namespace hellonet
