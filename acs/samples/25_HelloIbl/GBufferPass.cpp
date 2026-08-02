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

void RenderMotionAndNormalGBuffer(CHelloIblApp& app,
                                  const FMat4& vp_no_jitter) noexcept {
    app.m_MotionGBufferValid = false;
    IRhiCommandList* cl = app.GetRenderer().CommandList();
    if (!cl) return;

    constexpr u32 kGrid = 5u;
    const u32 required_draws =
        1u + kGrid * kGrid + kDynCount +
        (app.m_ShowRefraction ? 2u : 0u);

    // frame 0 は前フレーム VP が未確定なので prev=curr で motion 0 にする。
    const FMat4 motion_prev_vp = app.m_TaaPrevVpValid ? app.m_PrevVpNoJitter
                                                       : vp_no_jitter;
    if (!app.m_Motion.BeginFrame(required_draws) ||
        !app.m_Motion.Begin(*cl, vp_no_jitter, motion_prev_vp)) {
        return;
    }

    bool complete = true;

    // 床 (静的: prev == curr)
    const FMat4 plane_model = FMat4::Translation(FVec3{0, -0.6f, 3.0f});
    if (!app.m_Motion.DrawMesh(
            *cl, app.m_GmPlane, plane_model, plane_model)) {
        complete = false;
    }

    // 静的グリッド球 25 (color pass と同じ transform 計算、prev == curr)
    constexpr f32 kSpacing = 1.4f;
    for (u32 y = 0; y < kGrid; ++y) {
        for (u32 x = 0; x < kGrid; ++x) {
            const f32 px = (static_cast<f32>(x) - (kGrid - 1) * 0.5f) * kSpacing;
            const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 2.5f;
            const FMat4 m = FMat4::Translation(FVec3{px, py, 3.0f});
            if (!app.m_Motion.DrawMesh(
                    *cl, app.m_GmSphere, m, m)) {
                complete = false;
            }
        }
    }

    // 動的球 (prev != curr → object motion を含む)
    for (u32 i = 0; i < kDynCount; ++i) {
        if (!app.m_Motion.DrawMesh(
                *cl, app.m_GmSphere,
                app.m_DynCurr[i], app.m_DynPrev[i])) {
            complete = false;
        }
    }

    // ガラス球: 静止 (prev == curr)。motion パスに含めると TAA が動かない
    // silhouette を正しく扱える (SSGI/SSR が後段で normal を読むためにも、
    // ガラス位置の normal が G-buffer に必要)。
    if (app.m_ShowRefraction) {
        const FMat4 clear_model =
            FMat4::Scale(FVec3{1.4f, 1.4f, 1.4f}) * FMat4::Translation(kGlassPos);
        if (!app.m_Motion.DrawMesh(
                *cl, app.m_GmSphere, clear_model, clear_model)) {
            complete = false;
        }
        const FMat4 frosted_model =
            FMat4::Scale(FVec3{1.2f, 1.2f, 1.2f}) * FMat4::Translation(kFrostedGlassPos);
        if (!app.m_Motion.DrawMesh(
                *cl, app.m_GmSphere, frosted_model, frosted_model)) {
            complete = false;
        }
    }
    app.m_Motion.End(*cl);
    app.m_MotionGBufferValid =
        complete &&
        app.m_Motion.ObjectDrawCount() == required_draws;
}

} // namespace helloibl
