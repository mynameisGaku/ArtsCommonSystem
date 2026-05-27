// SPDX-License-Identifier: Apache-2.0
// HelloIbl — CSM shadow pass の実装。
//
// 2048 px × 3 cascade atlas に caster (sphere grid) を焼く。
// 球グリッドの中心 (0, 2, 3) 周辺を near=0.1 / far=40 でカバーする
// orthographic 影行列。太陽方角は現在の sky preset に従う (Studio HDR は前方上向き)。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

FVec3 ResolveSunDirection(const HelloIblApp& app) noexcept {
    // Studio HDR preset (3) は前方上向きの固定方向、それ以外は FSky の太陽。
    if (app._current_preset == 3) return FVec3{0.3f, 0.6f, -0.5f};
    return app._sky.SunDirection();
}

void RenderShadowPass(HelloIblApp& app, const FVec3& sun_dir) noexcept {
    if (!app._use_shadows) return;

    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    // CSM: カメラ frustum を 3 cascade に分けて atlas へ焼く。
    // scene 範囲 (object は 30m 内) を near=0.1 / far=40 でカバー。
    app._shadow.SetDirectionalLightCascades(sun_dir,
                                            app._camera.View(), app._camera.Projection(),
                                            /*near=*/0.1f, /*far=*/40.0f);
    cl->BeginShadowPass(*app._shadow.DepthTexture(), 1.0f);   // atlas 全体 clear
    cl->SetPipeline(*app._shadow.CasterPipeline());

    constexpr u32 kGridCast    = 5;
    constexpr f32 kSpacingCast = 1.4f;
    for (u32 c = 0; c < app._shadow.CascadeCount(); ++c) {
        // cascade ごとに viewport / scissor / light VP を切替えて caster 描画
        cl->SetViewport(app._shadow.CascadeViewport(c));
        cl->SetScissor(app._shadow.CascadeScissor(c));
        app._shadow.SetCurrentCascade(c);
        cl->SetConstantBuffer(0, *app._shadow.LightCB());
        cl->SetVertexBuffer(*app._gm_sphere.vertex_buffer, app._gm_sphere.vertex_stride);
        cl->SetIndexBuffer(*app._gm_sphere.index_buffer);
        for (u32 y = 0; y < kGridCast; ++y) {
            for (u32 x = 0; x < kGridCast; ++x) {
                const f32 px = (static_cast<f32>(x) - (kGridCast - 1) * 0.5f) * kSpacingCast;
                const f32 py = (static_cast<f32>(y) - (kGridCast - 1) * 0.5f) * kSpacingCast + 2.5f;
                app._shadow.SetCaster(FMat4::Translation(FVec3{px, py, 3.0f}));
                cl->SetConstantBuffer(1, *app._shadow.CasterObjectCB());
                cl->DrawIndexed(app._gm_sphere.index_count);
            }
        }
    }
    cl->EndShadowPass(*app._shadow.DepthTexture());
}

} // namespace helloibl
