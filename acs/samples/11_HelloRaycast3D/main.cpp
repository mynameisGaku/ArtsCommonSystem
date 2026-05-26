// SPDX-License-Identifier: Apache-2.0
// HelloRaycast3D — エントリポイント。
//
// 構成:
//   Types.h                   - Object POD + 配置 / カメラ定数
//   HelloRaycast3DApp.{h,cpp} - FApplication 派生 (リソース所有)
//   RaycastScene.{h,cpp}      - オーケストレーション (メッシュ + 描画)
//   RaycastTargets.{h,cpp}    - オブジェクト配列 + ヒット状態
//   RayCaster.{h,cpp}         - カメラ入力 + レイ判定
//   HudRenderer.{h,cpp}       - 2D HUD (クロスヘア + 当たり情報)
//
// 動作:
//   ・地面プレーン + 8 個の球と立方体が並ぶ 3D シーン
//   ・カメラから前方にレイを撃ち、最初に当たったオブジェクトをハイライト
//   ・WASD でカメラ移動、矢印キーで視点回転
//   ・暖色 + 寒色の 2 灯ライティング + Blinn-Phong スペキュラ
//   ・Esc 終了
#include "app/EntryPoint.h"
#include "HelloRaycast3DApp.h"

ACS_DEFINE_MAIN(helloraycast3d::HelloRaycast3DApp)
