// SPDX-License-Identifier: Apache-2.0
// エントリポイント生成マクロ (ACS_GAME_MAIN)
//
// 使い方:
//   class FMyGame : public acs::game::FGame { ... };
//   ACS_GAME_MAIN(FMyGame)
//
// 中身は acs/app/EntryPoint.h の ACS_DEFINE_MAIN を FGame 派生向けに薄く
// ラップしたもの。将来 AppState の自動初期化や
// shipping/dev フラグの分岐を入れる余地に備えてこの層を作っておく。
#pragma once

#include "app/EntryPoint.h"

/**
 * FGame 派生クラスからエントリポイント (main) を生成するマクロ。
 *
 * @details
 * acs/app/EntryPoint.h の ACS_DEFINE_MAIN を FGame 派生向けに薄くラップしたもの。
 * 将来 AppState の自動初期化や shipping/dev フラグ分岐を入れる余地に備えてこの層を挟む。
 * @param GameClass エントリポイントを生成する FGame 派生クラス。
 */
#define ACS_GAME_MAIN(GameClass) ACS_DEFINE_MAIN(GameClass)
