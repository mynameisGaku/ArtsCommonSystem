// SPDX-License-Identifier: Apache-2.0
// HelloIbl — エントリポイント。Phase 31 IBL + Phase 32a HDR / ACES tonemap。
//
// 構成:
//   IblTypes.h               - 共通定数 (equirect 解像度 / kDynCount /
//                              kGlassPos など) + Halton(2,3) sequence helper
//   IblEnvBuilder.{h,cpp}    - CPU equirect 生成 (Sky procedural / Studio HDR)
//   IblLightmapBaker.{h,cpp} - 床用 lightmap CPU baker (Sphere-Ray analytical)
//   HelloIblApp.{h,cpp}      - Application 派生 (フル機能 IBL demo)
//
// 動作:
//   ・初フレームで BRDF LUT (256x256 RG16F) + env cubemap (256x256x6 R11G11B10F)
//     + irradiance (32x32x6) + prefilter (128x128x6 5 mips) を一括生成
//   ・以降のフレームで:
//     - 背景: env / irradiance / prefilter mip 0..4 を切替 (I キー)
//     - 5x5 sphere grid (sun + IBL)。最上段=cloth(sheen)、中央段=subsurface、
//       最下段=iridescence(薄膜)。各 lobe 行は weight/膜厚を X 方向に掃引。
//       残り 2 段が X=metallic Y=roughness の素 PBR。(Phase 35-1a/1b/2)
//     - BRDF LUT を画面右上にオーバーレイ表示
//     - 1/2/3 で Sky preset (Day / Sunset / Night) 切替 → cubemap 再生成
//     - シーンは HDR R16G16B16A16_Float RT に描画 → Bloom + ACES tonemap で LDR 出力
//
// 注: -DACS_RENDER_DILIGENT=ON 必須 (per-slice RT / cubemap / R11G11B10F / HDR が Diligent 専用)。
#include "HelloIblApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloibl::HelloIblApp)
