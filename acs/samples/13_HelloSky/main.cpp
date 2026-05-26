// SPDX-License-Identifier: Apache-2.0
// HelloSky — エントリポイント。
//
// 構成:
//   Types.h               - SkyPreset enum + 共通定数 (kAmbientDay/Sunset/Night)
//   SkyScene.{h,cpp}      - シーン描画ロジック (Sky → 地面 → 球)
//   HelloSkyApp.{h,cpp}   - Application 派生クラス (resource 所有 + 入力)
//
// 動作:
//   ・手続き生成スカイ (昼/夕焼け/夜のプリセット切替可)
//   ・地面の上で 1 つのメッシュ (メイン光源と同期した色) が回転
//
// 操作:
//   1 / 2 / 3 : 昼 / 夕焼け / 夜のプリセット
//   ← →     : カメラ周回
//   Esc       : 終了
//
// 学習ポイント:
//   ・Sky::Render を OnRender 先頭で呼ぶ (深度書込み無し)
//   ・StandardShader::SetLights のライト色を Sky と整合させる
//   ・PresetXxx で雰囲気を一発切替
//
// ACS_DEFINE_MAIN は HelloSkyApp を main エントリに登録 (Application 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloSkyApp.h"

ACS_DEFINE_MAIN(hellosky::HelloSkyApp)
