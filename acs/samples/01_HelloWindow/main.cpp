// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 01 — HelloWindow エントリポイント。
//
// 構成:
//   main.cpp                 - ACS_DEFINE_MAIN(HelloWindowApp) のみ
//   HelloWindowApp.{h,cpp}   - CApplication 派生クラス本体
#include "HelloWindowApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellowin::CHelloWindowApp)
