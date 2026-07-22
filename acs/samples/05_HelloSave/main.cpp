// SPDX-License-Identifier: Apache-2.0
// HelloSave — エントリポイント。
//
// 構成:
//   HelloSaveApp.{h,cpp} - FApplication 派生クラス (FStorage で設定 / セーブの永続化)
//
// 操作:
//   Space : クリック数 +1
//   S     : 手動保存
//   R     : 全リセット
//   Esc   : 終了 (自動保存)
#include "HelloSaveApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellosave::FHelloSaveApp)
