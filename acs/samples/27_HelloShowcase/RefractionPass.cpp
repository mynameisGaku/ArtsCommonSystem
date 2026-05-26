// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — 屈折ガラス球 pass の実装。
#include "RefractionPass.h"
#include "ShowcaseTypes.h"

#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

using namespace acs;

namespace helloshowcase {

void ExecuteRefractionPass(Assets& a, IRhiCommandList& cl,
                           IRhiTexture& hdr, IRhiTexture& depth,
                           const FMat4& view_proj, FVec3 cam_pos) noexcept {
    // 背景キャプチャ: HDR の現在内容を bg_rt へ複製。
    // 同一 RT を read+write で同時 bind できないので必須。
    a.blit.Copy(cl, hdr, *a.bg_rt);

    cl.BeginRenderToTextureLoad(hdr, &depth);
    Viewport vp{};
    vp.width  = static_cast<f32>(hdr.Width());
    vp.height = static_cast<f32>(hdr.Height());
    cl.SetViewport(vp);
    ScissorRect svr{};
    svr.right  = static_cast<i32>(hdr.Width());
    svr.bottom = static_cast<i32>(hdr.Height());
    cl.SetScissor(svr);

    a.refr.SetFrame(view_proj, cam_pos);
    // env として prefilter (rough) があればそちらを優先、無ければ env cubemap。
    IRhiTexture* env = a.ibl.HasPrefilterMap() ? a.ibl.PrefilterMap()
                                                : a.ibl.EnvCubemap();
    if (!env) {
        cl.EndRenderToTexture(hdr);
        return;
    }

    for (u32 i = 0; i < kSphereCount; ++i) {
        const MaterialKind k = kSphereKind[i];
        if (k != MaterialKind::ClearGlass && k != MaterialKind::FrostedGlass) {
            continue;
        }
        const FMat4 m = FMat4::Scale(FVec3{kSphereScale, kSphereScale, kSphereScale}) *
                       FMat4::Translation(FVec3{kSphereX[i], kSphereY, kSphereZ});
        const f32 roughness = (k == MaterialKind::FrostedGlass) ? 0.5f : 0.0f;
        const FVec3 tint = (k == MaterialKind::FrostedGlass)
                              ? FVec3{0.95f, 0.97f, 1.0f}    // 弱く青
                              : FVec3{1.0f,  1.0f,  1.0f};   // クリア
        // clear glass は dispersion を入れて diamond/prism 風の色分離を見せる
        const f32 dispersion = (k == MaterialKind::ClearGlass) ? 0.5f : 0.0f;
        a.refr.DrawMesh(cl, a.gm_sphere, m, *a.bg_rt, *env,
                        /*ior=*/1.5f, /*thickness=*/0.45f, tint,
                        roughness, dispersion);
    }

    cl.EndRenderToTexture(hdr);
}

} // namespace helloshowcase
