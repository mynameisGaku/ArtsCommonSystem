// SPDX-License-Identifier: Apache-2.0
// HelloIbl — SSR / SSAO / SSGI の dispatch (1-frame latency 設計)。
//
// いずれも color pass の後、FPostProcess の前に走る。出力は次フレームの
// FPbrShader が読む (1-frame latency)。60 FPS なら 16ms 遅延でカメラ追従が
// 微かに遅れる程度、視覚的に許容。frame 0 は未書込みで garbage を読みうるため
// _xxx_warm フラグが立つまで FPbrShader 側は null bind (フォールバック値) に戻す。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

void RenderSsrPass(HelloIblApp& app, const FMat4& vp_for_render,
                   const FMat4& inv_vp, const FMat4& vp_no_jitter) noexcept {
    if (!app._show_ssr) return;

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     hdr   = app._post.HdrRenderTarget();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !hdr || !depth) return;

    // SSR を計算して _ssr.OutputTexture() に書く。最終合成は FPbrShader 側で
    // roughness 依存に env prefilter と blend する。intensity は 1.0 固定 —
    // 反射強度は FPbrShader::SetSsr 側で一元管理する。

    // temporal SSR の reproject 用に前フレームの jitter なし VP を渡す。
    // frame 0 は未確定なので現 VP を渡し reprojection を無効化。
    const FMat4& ssr_prev_vp = app._taa_prev_vp_valid ? app._prev_vp_no_jitter
                                                     : vp_no_jitter;
    IRhiTexture* motion_tex = (app._use_motion_vec && app._taa_prev_vp_valid)
                                  ? app._motion.OutputTexture() : nullptr;
    app._ssr.Render(*dev, *cl, *hdr, *depth,
                    *app._motion.OutputNormalTexture(),
                    vp_for_render, inv_vp, ssr_prev_vp,
                    app._camera.Eye(),
                    /*intensity=*/1.0f,
                    motion_tex);
    app._ssr_warm = true;     // 次フレームから FPbrShader が SSR texture を読める
}

void RenderSsaoPass(HelloIblApp& app, const FMat4& vp_for_render,
                    const FMat4& inv_vp, const FVec3& sun_dir) noexcept {
    if (!app._use_ssao) return;

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !depth) return;

    // GTAO は view 空間で計算するので view 行列も渡す。analytical 積分は
    // 適度な遮蔽量に収まるので intensity 1.0、radius 0.5m。
    app._ssao.Render(*dev, *cl, *depth,
                     *app._motion.OutputNormalTexture(),
                     vp_for_render, inv_vp, app._camera.View(),
                     app._camera.Eye(), sun_dir,
                     /*intensity=*/1.0f,
                     /*radius=*/0.5f);
    app._ssao_warm = true;     // 次フレームから FPbrShader が SSAO texture を読める
}

void RenderSsgiPass(HelloIblApp& app, const FMat4& vp_for_render,
                    const FMat4& inv_vp, const FMat4& vp_no_jitter) noexcept {
    if (!app._use_ssgi) return;

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     hdr   = app._post.HdrRenderTarget();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !hdr || !depth) return;

    // temporal accumulation 用に前フレームの jitter なし VP を渡す (TAA と共用)。
    // frame 0 は _prev_vp_no_jitter が default identity なので、現フレームの VP を
    // prev として渡し reprojection を motion 0 にしておく (TAA と同じ cold-start ガード)。
    // motion_tex を渡すと temporal pass が動く mesh も正しく reproject する
    // (null なら従来の depth reprojection にフォールバック)。
    const FMat4& ssgi_prev_vp = app._taa_prev_vp_valid ? app._prev_vp_no_jitter
                                                      : vp_no_jitter;
    IRhiTexture* motion_tex = (app._use_motion_vec && app._taa_prev_vp_valid)
                                  ? app._motion.OutputTexture() : nullptr;
    app._ssgi.Render(*dev, *cl, *hdr, *depth,
                     *app._motion.OutputNormalTexture(),
                     vp_for_render, inv_vp, ssgi_prev_vp,
                     app._camera.Eye(),
                     /*intensity=*/1.0f,
                     /*max_distance=*/5.0f,
                     motion_tex);
    app._ssgi_warm = true;
}

} // namespace helloibl
