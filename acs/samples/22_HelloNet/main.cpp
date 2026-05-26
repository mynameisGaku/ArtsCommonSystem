// SPDX-License-Identifier: Apache-2.0
// HelloNet — エントリポイント。
//
// 構成:
//   HelloNetApp.{h,cpp} - 一連のフロー (Init → サーバ起動 → 送受信 → Shutdown)
//   EchoServer.{h,cpp}  - FThread::Spawn から呼ぶサーバ本体 (Listen → Accept → echo)
//
// 動作:
//   ・スレッドで TCP echo サーバを起動 (port 8765, ループバック)
//   ・メインスレッドからクライアント接続して送受信
//   ・受信したテキストをコンソール出力
//
// HelloNet は window / renderer を要らないため Application 派生ではなく自前 main。
#include "HelloNetApp.h"

int main() {
    hellonet::HelloNetApp app;
    return app.Run();
}
