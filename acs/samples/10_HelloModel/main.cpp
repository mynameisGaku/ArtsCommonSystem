// SPDX-License-Identifier: Apache-2.0
// HelloModel — エントリポイント。
//
// 構成:
//   Types.h               - カメラ初期値 / ライト / マテリアル色 etc. 定数
//   HelloModelApp.{h,cpp} - FApplication 派生 (FStandardShader / async ロード所有)
//   ModelScene.{h,cpp}    - プリミティブ + カメラ + 描画
//
// 動作:
//   ・中央に回転する球 (Primitive::MakeSphere)
//   ・足元に地面プレーン (Primitive::MakePlane)
//   ・側にキューブが並ぶ
//   ・全部 Lambert ライティング (FStandardShader)
//   ・カメラ: WASD で水平移動、矢印キーで視点回転
//   ・Esc 終了
#include "app/EntryPoint.h"
#include "HelloModelApp.h"

ACS_DEFINE_MAIN(hellomodel::HelloModelApp)
