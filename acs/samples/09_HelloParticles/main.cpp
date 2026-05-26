// SPDX-License-Identifier: Apache-2.0
// HelloParticles — エントリポイント。
//
// 構成:
//   Types.h                   - パーティクルテクスチャサイズ + preset 名一覧
//   HelloParticlesApp.{h,cpp} - Application 派生 (SpriteBatch / Font / Glow 所有)
//   ParticleScene.{h,cpp}     - ParticleSystem の管理 + 描画
//
// 動作:
//   ・マウス位置からパーティクル発射
//   ・1 / 2 / 3 / 4 キーで Fire / Sparks / Fountain / Smoke 切替
//   ・左クリックで爆発（バースト）
//   ・スペースで連続生成 ON/OFF
//   ・Esc 終了
#include "app/EntryPoint.h"
#include "HelloParticlesApp.h"

ACS_DEFINE_MAIN(helloparticles::HelloParticlesApp)
