// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — エントリポイント。
//
// 構成:
//   Types.h                   - Ball POD + 物理 / 描画パラメータ定数
//   HelloPhysics2DApp.{h,cpp} - FApplication 派生 (FSpriteBatch / Font / Texture 所有)
//   PhysicsScene.{h,cpp}      - 物理シミュレーション + 描画
//
// 動作:
//   ・複数の円ボールが画面内をバウンドし、ボール同士でも衝突
//   ・重力 + 速度減衰
//   ・マウス左クリックで新しいボールを発射
//   ・スペースで全ボールを削除
//   ・Esc 終了
#include "app/EntryPoint.h"
#include "HelloPhysics2DApp.h"

ACS_DEFINE_MAIN(hellophysics2d::HelloPhysics2DApp)
