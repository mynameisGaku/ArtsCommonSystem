// SPDX-License-Identifier: Apache-2.0
// HelloNet — アプリケーションクラス。
// Network::Init / Logger::Init → TCP echo サーバスレッド起動 → クライアント
// 接続 → 送受信 → Join まで一連の流れを Run() で実行する。
//
// Application 派生ではなく自前 main から呼ぶ console アプリ (HelloNet には
// window / renderer が要らないため)。
#pragma once

#include "foundation/Types.h"
#include "threading/Thread.h"

namespace hellonet {

// サーバスレッド本体 (TcpListener::Listen → Accept → Recv → Send) は
// EchoServer.h/.cpp に free function で分離している (FThread::Spawn の引数仕様に
// 合わせるため、メンバ関数ではなく free function にしている)。
class HelloNetApp {
public:
    // 一連のフロー (Init → サーバ起動 → クライアント送受信 → Shutdown) を実行し
    // プロセス終了 code を返す (0 成功 / 1-3 失敗段階)。
    int Run() noexcept;
};

} // namespace hellonet
