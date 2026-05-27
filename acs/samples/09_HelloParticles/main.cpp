// SPDX-License-Identifier: Apache-2.0
// HelloParticles — 2D パーティクルエンジンのサンプル。
//
// 構成:
//   Types.h                   - パーティクルテクスチャサイズ + preset 名一覧
//   HelloParticlesApp.{h,cpp} - FApplication 派生 (FSpriteBatch / Font / Glow 所有)
//   ParticleScene.{h,cpp}     - ParticleSystem の管理 + 描画
//
// 操作はゲーム画面下のヘルプ行に表示される (Esc / 1-4 / Space / 左クリック)。
#include "app/EntryPoint.h"
#include "HelloParticlesApp.h"

ACS_DEFINE_MAIN(helloparticles::HelloParticlesApp)
