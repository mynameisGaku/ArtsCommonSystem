// SPDX-License-Identifier: Apache-2.0
// HelloBloom — エントリポイント。
//
// 構成:
//   HelloBloomApp.{h,cpp} - Application 派生 (HDR シーン + Bloom + Tonemap)
//
// 動作:
//   ・暗いシーンに非常に明るい (HDR) 球をいくつか配置
//   ・PostProcess (Bloom + ACES Tonemap) を通して LDR バックバッファに出す
//   ・1 / 2 / 3 で Bloom 強度切替、Esc 終了
//
// 学習ポイント:
//   ・OnCustomFrame() を override して HDR RT 経由のレンダリングを構築
//   ・PostProcess::Render が swapchain への合成まで担当
//
// 注: -DACS_RENDER_DILIGENT=ON 必須（Dx12 raw backend は HDR/Bloom 未対応）
//
// ACS_DEFINE_MAIN は HelloBloomApp を main エントリに登録 (Win32 subsystem では
// WinMain / wWinMain も生成し、Console subsystem では main を出力)。
#include "HelloBloomApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(hellobloom::HelloBloomApp)
