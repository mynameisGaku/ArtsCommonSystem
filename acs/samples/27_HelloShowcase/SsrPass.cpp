// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — SSR / SSAO pass の実装。
#include "SsrPass.h"

#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"

using namespace acs;

namespace helloshowcase {

void ExecuteSsrPass(Assets& a, IRhiDevice& dev, IRhiCommandList& cl,
                    IRhiTexture& hdr, IRhiTexture& depth,
                    const Mat4& view_proj,
                    const Mat4& inv_view_proj,
                    const Mat4& prev_view_proj,
                    Vec3 cam_pos,
                    IRhiTexture* motion_or_null,
                    bool show_ssr) noexcept {
    if (!show_ssr) return;

    // Hi-Z: SSR の skip-ahead 用 coarse min-depth を depth から焼く。
    // SSR ray-march の早期 stride を稼ぐためのアクセラレータ。
    a.hiz.Build(dev, cl, depth);
    a.ssr.Render(dev, cl, hdr, depth,
                 *a.motion.OutputNormalTexture(),
                 view_proj, inv_view_proj, prev_view_proj,
                 cam_pos, /*intensity=*/0.7f, motion_or_null,
                 a.hiz.Texture());
}

void ExecuteSsaoPass(Assets& a, IRhiDevice& dev, IRhiCommandList& cl,
                     IRhiTexture& depth,
                     const Mat4& view_proj,
                     const Mat4& inv_view_proj,
                     const Mat4& view,
                     Vec3 cam_pos) noexcept {
    a.ssao.Render(dev, cl, depth,
                  *a.motion.OutputNormalTexture(),
                  view_proj, inv_view_proj, view,
                  cam_pos, a.sky.SunDirection(),
                  /*intensity=*/0.7f, /*radius=*/0.5f);
}

} // namespace helloshowcase
