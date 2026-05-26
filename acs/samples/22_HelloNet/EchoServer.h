// SPDX-License-Identifier: Apache-2.0
// HelloNet — サーバスレッド本体。Thread::Spawn(&ServerThread, ...) で呼ぶ。
// TcpListener::Listen → Accept → Recv → そのまま Send back (echo)。
#pragma once

#include "foundation/Types.h"

namespace hellonet {

// echo サーバが listen する loopback port。
inline constexpr acs::u16 kEchoPort = 8765;

// Thread::Spawn から呼ばれるサーバ本体。user は使わない (nullptr で OK)。
void ServerThread(void* user) noexcept;

} // namespace hellonet
