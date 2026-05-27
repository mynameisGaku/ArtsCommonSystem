// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — post-process pass の実装。
#include "BloomPass.h"

#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"

using namespace acs;

namespace helloshowcase {

void ExecuteBloomPass(Assets& a, IRhiCommandList& cl,
                      IRhiSwapchain& sc, u32 buffer_index,
                      IRhiTexture& depth,
                      PostProcessParams& post_params,
                      const FMat4& vp_no_jitter,
                      const FMat4& prev_vp_no_jitter,
                      bool prev_vp_valid,
                      IRhiTexture* motion_or_null) noexcept {
    // TAA: motion vector + (前 VP が valid なら) depth を渡し、history を
    // 前フレームから reproject する。1 フレーム目は depth=null で OK
    // (FPostProcess 側で no-history のパスになる)。
    post_params.taa_enabled                   = true;
    post_params.taa_blend_factor              = 0.1f;
    post_params.taa_motion_texture            = motion_or_null;
    post_params.taa_depth_texture             = prev_vp_valid ? &depth : nullptr;
    post_params.taa_view_proj_no_jitter       = vp_no_jitter;
    post_params.taa_prev_view_proj_no_jitter  = prev_vp_no_jitter;

    // GPU auto-exposure (key=0.5 は target 平均輝度)。exposure は GPU 側で上書き。
    post_params.auto_exposure_enabled = true;
    post_params.auto_exposure_key     = 0.5f;
    post_params.exposure              = 1.0f;

    a.post.Render(cl, sc, buffer_index, post_params);
}

} // namespace helloshowcase
