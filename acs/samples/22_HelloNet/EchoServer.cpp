// SPDX-License-Identifier: Apache-2.0
// HelloNet — サーバスレッド実装。
#include "EchoServer.h"

#include "network/TcpListener.h"
#include "network/TcpConnection.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellonet {

void ServerThread(void* /*user*/) noexcept {
    auto lr = TcpListener::Listen(IpAddress::Any(), kEchoPort);
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
        conn.Send(buf, static_cast<usize>(n));
    }
}

} // namespace hellonet
