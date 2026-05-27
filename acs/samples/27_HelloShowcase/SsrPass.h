// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — SSR + Hi-Z + SSAO の pass。motion / normal G-buffer の後で
// 走らせる。
//
// SSR 出力 / SSAO 出力は次フレームの PBR pass が 1 フレーム遅延で合成する
// (frame 0 の garbage を避けるため呼び出し側で warm flag を管理)。
//
// SSR は m_ShowSsr で toggle。SSAO は常に走らせる (画作りで必須)。
#pragma once

#include "ShowcaseAssets.h"

#include "math/Mat.h"
#include "math/Vec.h"

namespace acs { class IRhiDevice; class IRhiCommandList; class IRhiTexture; }

namespace helloshowcase {

// Hi-Z を焼き、SSR を実行する。show_ssr=false のときは丸ごとスキップ。
// motion_or_null は前フレームが無いと nullptr が来る (SSR は depth fallback)。
void ExecuteSsrPass(Assets& a,
                     acs::IRhiDevice& dev,
                     acs::IRhiCommandList& cl,
                     acs::IRhiTexture& hdr,
                     acs::IRhiTexture& depth,
                     const acs::FMat4& view_proj,
                     const acs::FMat4& inv_view_proj,
                     const acs::FMat4& prev_view_proj,
                     acs::FVec3 cam_pos,
                     acs::IRhiTexture* motion_or_null,
                     bool show_ssr) noexcept;

// GTAO の indirect/ambient AO を計算する。
void ExecuteSsaoPass(Assets& a,
                      acs::IRhiDevice& dev,
                      acs::IRhiCommandList& cl,
                      acs::IRhiTexture& depth,
                      const acs::FMat4& view_proj,
                      const acs::FMat4& inv_view_proj,
                      const acs::FMat4& view,
                      acs::FVec3 cam_pos) noexcept;

} // namespace helloshowcase
