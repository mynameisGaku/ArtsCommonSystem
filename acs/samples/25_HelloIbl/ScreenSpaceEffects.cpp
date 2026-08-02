// SPDX-License-Identifier: Apache-2.0
// HelloIbl — SSR / SSAO / SSGI の dispatch (1-frame latency 設計)。
//
// いずれも color pass の後、CPostProcess の前に走る。出力は次フレームの
// CPbrShader が読む (1-frame latency)。60 FPS なら 16ms 遅延でカメラ追従が
// 微かに遅れる程度、視覚的に許容。frame 0 は未書込みで garbage を読みうるため
// m_XxxWarm フラグが立つまで CPbrShader 側は null bind (フォールバック値) に戻す。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

void RenderSsrPass(CHelloIblApp& app, const FMat4& vp_for_render,
                   const FMat4& inv_vp, const FMat4& vp_no_jitter) noexcept {
    if (!app.m_ShowSsr) return;
    if (!app.m_MotionGBufferValid) {
        app.m_SsrWarm = false;
        return;
    }

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     hdr   = app.m_Post.HdrRenderTarget();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !hdr || !depth) return;

    // SSR を計算して m_Ssr.OutputTexture() に書く。最終合成は CPbrShader 側で
    // roughness 依存に env prefilter と blend する。intensity は 1.0 固定 —
    // 反射強度は CPbrShader::SetSsr 側で一元管理する。

    // temporal SSR の reproject 用に前フレームの jitter なし VP を渡す。
    // frame 0 は未確定なので現 VP を渡し reprojection を無効化。
    const FMat4& ssr_prev_vp = app.m_TaaPrevVpValid ? app.m_PrevVpNoJitter
                                                     : vp_no_jitter;
    IRhiTexture* motion_tex =
        (app.m_bUseMotionVec && app.m_TaaPrevVpValid)
            ? app.m_Motion.OutputTexture()
            : nullptr;
    app.m_Ssr.Render(*dev, *cl, *hdr, *depth,
                    *app.m_Motion.OutputNormalTexture(),
                    vp_for_render, inv_vp, ssr_prev_vp,
                    app.m_Camera.Eye(),
                    /*intensity=*/1.0f,
                    motion_tex);
    app.m_SsrWarm = true;     // 次フレームから CPbrShader が SSR texture を読める
}

void RenderSsaoPass(CHelloIblApp& app, const FMat4& vp_for_render,
                    const FMat4& inv_vp, const FVec3& sun_dir) noexcept {
    if (!app.m_bUseSsao) return;
    if (!app.m_MotionGBufferValid) {
        app.m_bSsaoWarm = false;
        return;
    }

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !depth) return;

    // GTAO は view 空間で計算するので view 行列も渡す。analytical 積分は
    // 適度な遮蔽量に収まるので intensity 1.0、radius 0.5m。
    app.m_Ssao.Render(*dev, *cl, *depth,
                     *app.m_Motion.OutputNormalTexture(),
                     vp_for_render, inv_vp, app.m_Camera.View(),
                     app.m_Camera.Eye(), sun_dir,
                     /*intensity=*/1.0f,
                     /*radius=*/0.5f);
    app.m_bSsaoWarm = true;     // 次フレームから CPbrShader が SSAO texture を読める
}

void RenderSsgiPass(CHelloIblApp& app, const FMat4& vp_for_render,
                    const FMat4& inv_vp, const FMat4& vp_no_jitter) noexcept {
    if (!app.m_bUseSsgi) return;
    if (!app.m_MotionGBufferValid) {
        app.m_bSsgiWarm = false;
        return;
    }

    IRhiDevice*      dev   = app.GetRenderer().Device();
    IRhiCommandList* cl    = app.GetRenderer().CommandList();
    IRhiTexture*     hdr   = app.m_Post.HdrRenderTarget();
    IRhiTexture*     depth = app.GetRenderer().DepthBuffer();
    if (!dev || !cl || !hdr || !depth) return;

    // temporal accumulation 用に前フレームの jitter なし VP を渡す (TAA と共用)。
    // frame 0 は m_PrevVpNoJitter が default identity なので、現フレームの VP を
    // prev として渡し reprojection を motion 0 にしておく (TAA と同じ cold-start ガード)。
    // motion_tex を渡すと temporal pass が動く mesh も正しく reproject する
    // (null なら従来の depth reprojection にフォールバック)。
    const FMat4& ssgi_prev_vp = app.m_TaaPrevVpValid ? app.m_PrevVpNoJitter
                                                      : vp_no_jitter;
    IRhiTexture* motion_tex = (app.m_bUseMotionVec && app.m_TaaPrevVpValid)
                                  ? app.m_Motion.OutputTexture() : nullptr;
    app.m_Ssgi.Render(*dev, *cl, *hdr, *depth,
                     *app.m_Motion.OutputNormalTexture(),
                     vp_for_render, inv_vp, ssgi_prev_vp,
                     app.m_Camera.Eye(),
                     /*intensity=*/1.0f,
                     /*max_distance=*/5.0f,
                     motion_tex);
    app.m_bSsgiWarm = true;
}

} // namespace helloibl
