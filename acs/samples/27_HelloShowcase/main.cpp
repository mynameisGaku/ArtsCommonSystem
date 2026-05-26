// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — エントリポイント。
//
// 構成:
//   ShowcaseTypes.h        - sphere/orb 配置定数 + Halton helper
//   ShowcaseApp.{h,cpp}    - Application 派生クラス (PBR / IBL / SSR / SSAO /
//                            Refraction / Bloom / ACES + TAA cinematic demo)
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
