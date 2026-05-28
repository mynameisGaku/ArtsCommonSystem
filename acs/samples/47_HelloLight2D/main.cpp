// SPDX-License-Identifier: Apache-2.0
// HelloLight2D — 2D 動的ライト + ソフト影 (Core Keeper 風) + ブロブ影のデモ。
//
// 操作:
//   マウス … 松明 (主光源) を動かす
//   [B]   … ブロブ影のみの軽量モードに切替
//   Esc   … 終了
//
// 影は occluder (柱/キャラのシルエット) を ray-march して落とすので、スプライト
// 形状に沿った丸い影になる。暗い ambient + カラー点光源で洞窟の雰囲気。
#include "Light2DApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellolight2d::FLight2DApp)
