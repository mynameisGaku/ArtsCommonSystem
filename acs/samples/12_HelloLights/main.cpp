// SPDX-License-Identifier: Apache-2.0
// HelloLights — エントリポイント。
//
// 何が見える:
//   開けた地面に複数のキューブと球を散らし、4 色の点光源が円周状に
//   回りながら照らす。dir ライトは ambient 主体まで弱め、点光源の
//   色・減衰がわかりやすくなる構図にしている。
//
// 操作:
//   WASD : カメラ移動
//   矢印 : 視点回転
//   Esc  : 終了
#include "app/EntryPoint.h"
#include "HelloLightsApp.h"

ACS_DEFINE_MAIN(hellolights::HelloLightsApp)
