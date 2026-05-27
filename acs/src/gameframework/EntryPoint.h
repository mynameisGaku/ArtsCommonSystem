// SPDX-License-Identifier: Apache-2.0
// GameFramework Pillar A — エントリポイント生成マクロ (Phase 1 着手)
//
// 使い方:
//   class MyGame : public acs::game::FGame { ... };
//   ACS_GAME_MAIN(MyGame)
//
// 中身は acs/app/EntryPoint.h の ACS_DEFINE_MAIN を FGame 派生向けに薄く
// ラップしたもの (Phase 1 は同等)。Phase 2 で AppState の自動初期化や
// shipping/dev フラグの分岐を入れる余地に備えてこの層を作っておく。
#pragma once

#include "app/EntryPoint.h"

#define ACS_GAME_MAIN(GameClass) ACS_DEFINE_MAIN(GameClass)
