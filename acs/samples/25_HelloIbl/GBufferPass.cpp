// SPDX-License-Identifier: Apache-2.0
// HelloIbl — motion vector + world normal を MRT に焼く G-buffer pass。
//
// 全 mesh を再ラスタライズし MRT 2 枚 (screen-space motion vector +
// world-space normal) を焼く。motion は TAA / temporal SSGI が動く mesh を
// reproject するのに、normal は SSR が faceted な反射ベクトル (旧 ddx/ddy
// 由来) を避けるのに使う。SSGI が現フレームの motion を読むため SSGI より
// 前に実行する。静的 mesh は prev == curr。
//
// motion + normal は SSR/SSGI/SSAO/TAA 全てが使うため geometry パスは常時
// 実行する。'M' は motion の TAA/SSGI 消費を toggle するだけで、パスは止めない。
#include "HelloIblApp.h"

using namespace acs;

namespace helloibl {

void RenderMotionAndNormalGBuffer(HelloIblApp& app,
                                  const Mat4& vp_no_jitter) noexcept {
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    // frame 0 は前フレーム VP が未確定なので prev=curr で motion 0 にする。
    const Mat4 motion_prev_vp = app._taa_prev_vp_valid ? app._prev_vp_no_jitter
                                                       : vp_no_jitter;
    app._motion.Begin(*cl, vp_no_jitter, motion_prev_vp);

    // 床 (静的: prev == curr)
    const Mat4 plane_model = Mat4::Translation(Vec3{0, -0.6f, 3.0f});
    app._motion.DrawMesh(*cl, app._gm_plane, plane_model, plane_model);

    // 静的グリッド球 25 (color pass と同じ transform 計算、prev == curr)
    constexpr u32 kGrid    = 5;
    constexpr f32 kSpacing = 1.4f;
    for (u32 y = 0; y < kGrid; ++y) {
        for (u32 x = 0; x < kGrid; ++x) {
            const f32 px = (static_cast<f32>(x) - (kGrid - 1) * 0.5f) * kSpacing;
            const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 2.5f;
            const Mat4 m = Mat4::Translation(Vec3{px, py, 3.0f});
            app._motion.DrawMesh(*cl, app._gm_sphere, m, m);
        }
    }

    // 動的球 (prev != curr → object motion を含む)
    for (u32 i = 0; i < kDynCount; ++i) {
        app._motion.DrawMesh(*cl, app._gm_sphere, app._dyn_curr[i], app._dyn_prev[i]);
    }

    // ガラス球: 静止 (prev == curr)。motion パスに含めると TAA が動かない
    // silhouette を正しく扱える (SSGI/SSR が後段で normal を読むためにも、
    // ガラス位置の normal が G-buffer に必要)。
    if (app._show_refraction) {
        const Mat4 clear_model =
            Mat4::Scale(Vec3{1.4f, 1.4f, 1.4f}) * Mat4::Translation(kGlassPos);
        app._motion.DrawMesh(*cl, app._gm_sphere, clear_model, clear_model);
        const Mat4 frosted_model =
            Mat4::Scale(Vec3{1.2f, 1.2f, 1.2f}) * Mat4::Translation(kFrostedGlassPos);
        app._motion.DrawMesh(*cl, app._gm_sphere, frosted_model, frosted_model);
    }
    app._motion.End(*cl);
}

} // namespace helloibl
