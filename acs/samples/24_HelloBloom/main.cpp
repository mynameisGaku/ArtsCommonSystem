// SPDX-License-Identifier: Apache-2.0
// HelloBloom — エントリポイント。
//
// 構成:
//   FHelloBloomApp.{h,cpp} - FApplication 派生 (HDR シーン + Bloom + Tonemap)
//
// 動作:
//   ・暗いシーンに非常に明るい (HDR) 球を 4 つ配置
//   ・FPostProcess (Bloom + ACES Tonemap) を通して LDR backbuffer に合成
//   ・1 / 2 / 3 で Bloom 強度切替、左右で camera yaw、Esc 終了
//
// 学習ポイント:
//   ・OnCustomFrame() を override して HDR RT 経由のレンダリングを組む
//   ・FPostProcess::Render が Bloom + Tonemap + swapchain 合成を担当する
//   ・HUD は Tonemap 後の LDR backbuffer に直接書く (色が吹き飛ばないため)
//
// 注: FPostProcess は Diligent backend 専用機能。Dx12 raw backend では未対応のため
//     -DACS_RENDER_DILIGENT=ON で configure する必要がある。
#include "HelloBloomApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellobloom::FHelloBloomApp)
