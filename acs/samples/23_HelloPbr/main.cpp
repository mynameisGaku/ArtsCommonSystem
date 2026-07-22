// SPDX-License-Identifier: Apache-2.0
// HelloPbr — エントリポイント。
//
// 構成:
//   HelloPbrApp.{h,cpp} - FApplication 派生クラス (FPbrShader + sphere grid + 床)
//
// 動作:
//   ・5×5 の球を並べて metallic (X 方向) × roughness (Y 方向) を変える
//     クラシックな PBR material ball グリッド
//   ・1 つの dir light + 1 つの point light で照明
//   ・WASD でカメラ移動、矢印で視点回転
//   ・Esc 終了
//
// 学習ポイント:
//   ・FStandardShader (Blinn-Phong) と FPbrShader (Cook-Torrance) の違い
//   ・metallic = 0 (dielectric) で reflectance F0 = 0.04 固定
//   ・metallic = 1 (metal) で diffuse 無し + F0 = base_color
//   ・roughness ↓ で highlight シャープ、↑ でぼやけ
//
// ACS_DEFINE_MAIN は HelloPbrApp を main エントリに登録 (FApplication 派生 →
// `int WINAPI WinMain` / `int main` 両方の通常 main を裏で生成)。
#include "app/EntryPoint.h"
#include "HelloPbrApp.h"

ACS_DEFINE_MAIN(hellopbr::FHelloPbrApp)
