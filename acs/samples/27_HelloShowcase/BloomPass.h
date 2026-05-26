// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — post-process pass (Bloom + ACES tonemap + auto-exposure +
// TAA history blend)。HDR RT を読んで swapchain backbuffer へ LDR で書く。
//
// PostProcess::Render は内部で BeginRenderToSwapchain を発行するので、本 pass
// の呼び出し後の swapchain は (HUD overlay が描ける) "RT bound" 状態になる。
#pragma once

#include "ShowcaseAssets.h"

#include "math/Mat.h"
#include "render/PostProcess.h"     // PostProcessParams

namespace acs { class IRhiCommandList; class IRhiSwapchain; class IRhiTexture; }

namespace helloshowcase {

// post_params は呼び出し側で TAA jitter / exposure / bloom 等を予めセット
// しておく (本関数は taa_motion_texture / taa_depth_texture と前後 VP だけ
// 上書きする)。
void ExecuteBloomPass(Assets& a,
                       acs::IRhiCommandList& cl,
                       acs::IRhiSwapchain& sc,
                       acs::u32 buffer_index,
                       acs::IRhiTexture& depth,
                       acs::PostProcessParams& post_params,
                       const acs::FMat4& vp_no_jitter,
                       const acs::FMat4& prev_vp_no_jitter,
                       bool prev_vp_valid,
                       acs::IRhiTexture* motion_or_null) noexcept;

} // namespace helloshowcase
