// SPDX-License-Identifier: Apache-2.0
// HelloIbl — エントリポイント。Image-Based Lighting + HDR / ACES tonemap。
//
// 構成:
//   IblTypes.h                  - 共通定数 (equirect 解像度 / kDynCount /
//                                 kGlassPos など) + Halton(2,3) sequence helper
//   IblEnvBuilder.{h,cpp}       - CPU equirect 生成 (FSky procedural / Studio HDR)
//   IblLightmapBaker.{h,cpp}    - 床用 lightmap CPU baker (FSphere-FRay analytical)
//   HelloIblApp.{h,cpp}         - FApplication 派生 (orchestration のみ)
//   ShadowPass.cpp              - CSM 3 cascade caster
//   GBufferPass.cpp             - motion vector + world normal MRT
//   ScreenSpaceEffects.cpp      - SSR / SSAO / SSGI dispatch
//   RefractionPass.cpp          - screen-space 屈折ガラス
//   DynamicOrbs.cpp             - 公転する発光オーブ
//   SceneDraw.cpp               - 5x5 sphere grid + floor の PBR draw
//   PbrLightingBindings.cpp     - FPbrShader への各種テクスチャ/パラメータ bind
//   IblPresetBuilder.cpp        - preset 切替時の env 再生成
//   TaaJitter.cpp               - Halton(2,3) sub-pixel jitter
//   ExposureControl.cpp         - auto / manual 露出補間
//   HudOverlay.cpp              - FSpriteBatch HUD
//
// 動作:
//   ・初フレームで BRDF LUT (256x256 RG16F) + env cubemap (256x256x6 R11G11B10F)
//     + irradiance (32x32x6) + prefilter (128x128x6 5 mips) を一括生成
//   ・以降のフレームで:
//     - 背景: env / irradiance / prefilter mip 0..4 を切替 (I キー)
//     - 5x5 sphere grid (sun + IBL)。最上段=cloth(sheen)、中央段=subsurface、
//       最下段=iridescence(薄膜)。各 lobe 行は weight/膜厚を X 方向に掃引。
//       残り 2 段が X=metallic Y=roughness の素 PBR。
//     - BRDF LUT を画面右上にオーバーレイ表示
//     - 1/2/3 で FSky preset (Day / Sunset / Night) 切替 → cubemap 再生成
//     - シーンは HDR R16G16B16A16_Float RT に描画 → Bloom + ACES tonemap で LDR 出力
//
// 注: -DACS_RENDER_DILIGENT=ON 必須 (per-slice RT / cubemap / R11G11B10F / HDR が Diligent 専用)。
#include "HelloIblApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloibl::HelloIblApp)
