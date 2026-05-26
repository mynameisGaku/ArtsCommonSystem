// SPDX-License-Identifier: Apache-2.0
// HelloIbl — screen-space 屈折ガラス球の描画。
//
// opaque HDR scene を _bg_rt へ複製 (同一 RT の read+write 不可なので)、
// HDR を clear なし (Load) で再 bind → ガラス球を 2 個描く:
//   - クリアガラス (左、roughness=0、dispersion=0.4 でプリズム風色分離)
//   - フロステッドガラス (右、roughness=0.5 で 8-tap disk ブラー)
// 屈折 PS は _bg_rt を screen-space で UV ずらし sample、Fresnel で env reflection と
// blend、吸収 tint で着色。深度は読み書き両方 (DSV) として bind し z-test を有効に。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

void RenderRefractionPass(HelloIblApp& app, const FMat4& vp_for_render,
                          const FViewport& vp, const FScissorRect& svr) noexcept {
    if (!app._show_refraction) return;

    IRhiCommandList* cl  = app.GetRenderer().CommandList();
    IRhiTexture*     hdr = app._post.HdrRenderTarget();
    IRhiTexture* depth = app.GetRenderer().DepthBuffer();
    if (!cl || !hdr || !app._bg_rt) return;

    app._blit.Copy(*cl, *hdr, *app._bg_rt);
    cl->BeginRenderToTextureLoad(*hdr, depth);
    cl->SetViewport(vp);
    cl->SetScissor(svr);
    app._refr.SetFrame(vp_for_render, app._camera.Eye());

    // env cubemap: prefilter cube (env_cube より rough lobe にマッチ。mip 0 が
    // 鏡面に最も近く、IBL.PrefilterMap が無いケースは EnvCubemap にフォールバック)。
    IRhiTexture* env_for_refr = app._ibl.HasPrefilterMap() ? app._ibl.PrefilterMap()
                                                            : app._ibl.EnvCubemap();
    if (env_for_refr) {
        // 透明ガラス球 (左): roughness=0 で 1-tap シャープな屈折 +
        // dispersion=0.4 でダイヤ/プリズム風の色分離
        const FMat4 clear_model =
            FMat4::Scale(FVec3{1.4f, 1.4f, 1.4f}) * FMat4::Translation(kGlassPos);
        app._refr.DrawMesh(*cl, app._gm_sphere, clear_model, *app._bg_rt, *env_for_refr,
                           /*ior=*/1.5f, /*thickness=*/0.4f, FVec3{1, 1, 1},
                           /*roughness=*/0.0f, /*dispersion=*/0.4f);
        // フロステッドガラス球 (右): roughness=0.5 で 8-tap disk ブラー
        const FMat4 frosted_model =
            FMat4::Scale(FVec3{1.2f, 1.2f, 1.2f}) * FMat4::Translation(kFrostedGlassPos);
        app._refr.DrawMesh(*cl, app._gm_sphere, frosted_model, *app._bg_rt, *env_for_refr,
                           /*ior=*/1.5f, /*thickness=*/0.4f,
                           FVec3{0.95f, 0.97f, 1.0f},     // 弱く青に着色
                           /*roughness=*/0.5f);
    }
    cl->EndRenderToTexture(*hdr);
}

} // namespace helloibl
