// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — エントリポイント。
//
// 構成:
//   ShowcaseTypes.h           - sphere/orb 配置定数 + Halton helper
//   ShowcaseAssets.{h,cpp}    - GPU resource bundle + 初期化 / 解放
//   PbrPass.{h,cpp}           - FSky + 床 + opaque sphere + emissive orb
//   RefractionPass.{h,cpp}    - ガラス球 (clear / frosted)
//   MotionPass.{h,cpp}        - motion + normal G-buffer
//   SsrPass.{h,cpp}           - Hi-Z + SSR + SSAO
//   BloomPass.{h,cpp}         - FPostProcess (Bloom + ACES + TAA)
//   HudPass.{h,cpp}           - HUD overlay
//   ShowcaseApp.{h,cpp}       - FApplication 派生 (上記 pass を順に駆動)
//
// キー:
//   P  : auto-orbit を pause / resume
//   R  : SSR (env reflection mix) の toggle
//   X  : refraction glass の toggle
//   Esc: 終了
//
// 注: -DACS_RENDER_DILIGENT=ON でのみビルドされる (HDR / per-slice RT / 屈折 demo
// が Diligent backend 専用)。samples/CMakeLists.txt 側で `if(ACS_RENDER_DILIGENT)`
// にラップされる。
#include "ShowcaseApp.h"
#include "app/EntryPoint.h"

ACS_DEFINE_MAIN(helloshowcase::ShowcaseApp)
