// SPDX-License-Identifier: Apache-2.0
// HelloNet — サーバスレッド本体。FThread::Spawn(&ServerThread, ...) で呼ぶ。
// FTcpListener::Listen → Accept → Recv → そのまま Send back (echo)。
#pragma once

#include "foundation/Types.h"

namespace hellonet {

// echo サーバとクライアントが合意する port (ローカルテスト用に固定値)。
inline constexpr acs::u16 kEchoPort = 8765;

// FThread::Spawn から呼ばれるサーバ本体。user は使わない (nullptr で OK)。
void ServerThread(void* user) noexcept;

} // namespace hellonet
