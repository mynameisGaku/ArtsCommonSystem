// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "app/EntryPoint.h"

/**
 * CGame 派生クラスからエントリポイント (main) を生成するマクロ。
 *
 * @details
 * acs/app/EntryPoint.h の ACS_DEFINE_MAIN を CGame 派生向けに薄くラップしたもの。
 * 将来 AppState の自動初期化や shipping/dev フラグ分岐を入れる余地に備えてこの層を挟む。
 * @param GameClass エントリポイントを生成する CGame 派生クラス。
 */
#define ACS_GAME_MAIN(GameClass) ACS_DEFINE_MAIN(GameClass)
