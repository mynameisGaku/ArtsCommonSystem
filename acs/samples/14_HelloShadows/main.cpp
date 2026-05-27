// SPDX-License-Identifier: Apache-2.0
// HelloShadows — エントリポイント。
//
// 構成:
//   Types.h                   - CasterInst POD + 共通定数 (kCasterCount /
//                               kShadowMapSize)
//   ShadowsScene.{h,cpp}      - シーン描画ロジック (シャドウパス + 主パス)
//   HelloShadowsApp.{h,cpp}   - FApplication 派生クラス (resource 所有 + 入力)
//
// 動作:
//   ・ライト視点の depth-only パスでシャドウマップを焼く
//   ・主パスでメッシュを FStandardShader + シャドウサンプリングで描画
//   ・複数のキューブ + 球が地面に影を落とす
//
// 操作:
//   WASD  : 移動
//   矢印  : 視点回転
//   Space : 太陽の方向を回転 (影の動きが見える)
//   Esc   : 終了
//
// 学習ポイント:
//   ・FShadowMap::SetDirectionalLight でライト VP を計算
//   ・cl->BeginShadowPass / EndShadowPass の使い方
//   ・FStandardShader::SetShadowMap で主パスにシャドウを通知
//   ・PCSS (Fernando 2005) で物理的に正しいソフトシャドウ
//
// ACS_DEFINE_MAIN は HelloShadowsApp を main エントリに登録 (FApplication 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloShadowsApp.h"

ACS_DEFINE_MAIN(helloshadows::HelloShadowsApp)
