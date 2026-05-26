// SPDX-License-Identifier: Apache-2.0
// HelloLights — エントリポイント。
//
// 構成:
//   Types.h                - ObjectInst POD + 共通定数 (kCubeCount / kPointCount)
//   LightsScene.{h,cpp}    - シーン描画ロジック (ライト計算 + 物体描画)
//   HelloLightsApp.{h,cpp} - Application 派生クラス (resource 所有 + 入力)
//
// 動作:
//   ・暗い「部屋」(地面 + 4 つの壁) の代わりに開けた地面 + 散らした物体を
//     ベタなライトで照らす最小サンプル
//   ・床に複数のキューブと球を配置
//   ・4 つの色違いの点光源が円周状を移動して照らす
//   ・主光源 (dir) はほぼ無効 (ambient 主体) にして点光源の効果を強調
//
// 操作:
//   WASD : カメラ移動
//   矢印 : 視点回転
//   Esc  : 終了
//
// ACS_DEFINE_MAIN は HelloLightsApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloLightsApp.h"

ACS_DEFINE_MAIN(hellolights::HelloLightsApp)
