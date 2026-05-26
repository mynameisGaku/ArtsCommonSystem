// SPDX-License-Identifier: Apache-2.0
// ACS サンプル 02 — HelloSprite エントリポイント。
//
// 構成:
//   main.cpp                 - ACS_DEFINE_MAIN(HelloSpriteApp) のみ
//   Types.h                  - 共通定数 + Sprite struct + テクスチャ生成
//   HelloSpriteApp.{h,cpp}   - Application 派生クラス本体
#include "HelloSpriteApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellosprite::HelloSpriteApp)
