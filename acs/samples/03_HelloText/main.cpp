// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 03 — HelloText エントリポイント。
//
// 構成:
//   main.cpp                - ACS_DEFINE_MAIN(HelloTextApp) のみ
//   HelloTextApp.{h,cpp}    - Application 派生クラス本体
#include "HelloTextApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellotext::HelloTextApp)
