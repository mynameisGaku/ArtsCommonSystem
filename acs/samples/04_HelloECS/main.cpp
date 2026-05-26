// SPDX-License-Identifier: Apache-2.0
// HelloECS — エントリポイント。
//
// 構成:
//   Types.{h,cpp}        - Position/Velocity/Color POD + SpawnEvent +
//                          ball texture 生成 + MessageBroker 購読コールバック
//   HelloECSApp.{h,cpp}  - Application 派生クラス (World / EachParallel /
//                          TimerManager / MessageBroker のデモ)
//
// 操作:
//   Space : Entity を 50 個追加
//   Esc   : 終了
#include "HelloECSApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hello04::HelloECSApp)
