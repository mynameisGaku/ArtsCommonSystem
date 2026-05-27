// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — ShowcaseApp 実装。OnStart / OnUpdate / OnCustomFrame /
// OnShutdown でフレームをまわす orchestration 層。実際の draw は pass 系
// helper (PbrPass / RefractionPass / MotionPass / SsrPass / BloomPass /
// HudPass) と GPU resource は Assets (ShowcaseAssets.{h,cpp}) に分割している。
#include "ShowcaseApp.h"

#include "BloomPass.h"
#include "HudPass.h"
#include "MotionPass.h"
#include "PbrPass.h"
#include "RefractionPass.h"
#include "ShowcaseTypes.h"
#include "SsrPass.h"

#include "app/Sample.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiDevice.h"
#include "render/IRhiTexture.h"

using namespace acs;

namespace helloshowcase {

// ctor/dtor は明示的に cpp 側に定義する。Assets が TUniquePtr<IRhiTexture> を
// 抱えるため、ヘッダ側に dtor を書くと include 側が完全型を要求してしまう。
ShowcaseApp::ShowcaseApp() noexcept = default;
ShowcaseApp::~ShowcaseApp() noexcept = default;

void ShowcaseApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    const u32 sw = sc->Width();
    const u32 sh = sc->Height();
    ACS_SAMPLE_INIT(InitializeAssets(m_Assets, *dev, sw, sh,
                                      GetRenderer().ColorFormat(),
                                      GetRenderer().DepthFormat()));

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    m_Camera.SetPerspective(45.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    // post params: 控えめな bloom + ACES + mild vignette。
    // sun.color が 1.2x (HelloIbl 0.9x 互換) なので threshold もそれ相応に下げて
    // emissive orb (strength=4.0) の bloom がちゃんと光るようにする。
    m_PostParams.bloom_threshold      = 1.0f;
    m_PostParams.bloom_intensity      = 0.40f;
    m_PostParams.vignette_intensity   = 0.25f;
    m_PostParams.chromatic_aberration = 0.0f;        // 邪魔なので OFF
    m_PostParams.grain_intensity      = 0.0f;
    m_PostParams.tonemap_kind         = 0;           // 0=ACES (Narkowicz)

    // exposure を Day 想定にセット
    m_ExposureTarget  = 0.7f;
    m_AdaptedExposure = 0.7f;
}

void ShowcaseApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) { Quit(); return; }
    if (Input::IsKeyPressed(EKey::P)) m_Paused = !m_Paused;
    if (Input::IsKeyPressed(EKey::R)) m_ShowSsr = !m_ShowSsr;
    if (Input::IsKeyPressed(EKey::X)) m_ShowRefraction = !m_ShowRefraction;

    if (!m_Paused) {
        m_OrbitAngle += dt * 0.20f;     // 約 31 秒で 1 周
        m_OrbPhase   += dt * 0.50f;
    }

    // カメラ: scene 中心 (0, 0.4, 0) を見ながら半径 5.5 で水平回転 +
    // 微小な垂直 bob (cinematic ペン回し風)
    const f32 cam_radius = 5.5f;
    const f32 cam_y_base = 1.4f;
    const f32 cam_y_bob  = 0.18f * Sin(m_OrbitAngle * 1.3f);     // ±18cm 縦揺れ
    const FVec3 cam_target{0.0f, 0.4f, 0.0f};
    m_CamPos = FVec3{
        cam_target.x + cam_radius * Sin(m_OrbitAngle),
        cam_y_base + cam_y_bob,
        cam_target.z + cam_radius * Cos(m_OrbitAngle),
    };
    m_Camera.SetLookAt(m_CamPos, cam_target, FVec3::Up());
}

bool ShowcaseApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    IRhiTexture*     hdr   = m_Assets.post.HdrRenderTarget();
    if (!dev || !cl || !sc || !hdr || !depth) return false;

    cl->Begin();

    // ===== TAA jitter (Halton 2,3) =====
    // Halton(0, b) = 0 を避けるため +1 オフセット (HelloIbl と同様)
    const f32 jx = Halton((m_TaaFrameIndex & 31) + 1, 2) - 0.5f;
    const f32 jy = Halton((m_TaaFrameIndex & 31) + 1, 3) - 0.5f;
    ++m_TaaFrameIndex;
    const f32 jx_ndc = jx * 2.0f / static_cast<f32>(hdr->Width());
    const f32 jy_ndc = jy * 2.0f / static_cast<f32>(hdr->Height());
    const FMat4 view_proj_jittered = m_Camera.ViewProjection() *
                                    FMat4::Translation(FVec3{jx_ndc, jy_ndc, 0.0f});
    const FMat4 vp_no_jitter = m_Camera.ViewProjection();
    const FMat4 inv_vp        = Inverse(view_proj_jittered);

    // ===== IBL warmup (1 度だけ走る) =====
    if (!m_Assets.ibl.HasBrdfLut())       (void)m_Assets.ibl.EnsureBrdfLut(*dev, *cl);
    if (!m_Assets.ibl.HasEnvCubemap())    (void)m_Assets.ibl.EnsureEnvCubemap(*dev, *cl, m_Assets.sky);
    if (!m_Assets.ibl.HasIrradianceMap()) (void)m_Assets.ibl.EnsureIrradiance(*dev, *cl);
    if (!m_Assets.ibl.HasPrefilterMap())  (void)m_Assets.ibl.EnsurePrefilter(*dev, *cl);

    // ===== Opaque HDR pass (FSky + PBR + emissive orb) =====
    FMat4 orb_curr[kOrbCount]{};
    ExecutePbrPass(m_Assets, *cl, *hdr, *depth, m_Camera,
                   view_proj_jittered, m_CamPos, m_OrbPhase,
                   m_SsrWarm, m_SsaoWarm, orb_curr);

    // ===== Refraction pass (clear / frosted glass) =====
    if (m_ShowRefraction) {
        ExecuteRefractionPass(m_Assets, *cl, *hdr, *depth,
                              view_proj_jittered, m_CamPos);
    }

    // ===== Motion + normal G-buffer pass (TAA / SSR / SSAO 用) =====
    IRhiTexture* motion_tex = ExecuteMotionPass(m_Assets, *cl, vp_no_jitter,
                                                 m_PrevVpNoJitter, m_PrevVpValid,
                                                 m_PrevOrbPhase, orb_curr);

    // ===== SSR / SSAO (1-frame latency で次フレームの PBR が合成) =====
    const FMat4& ssr_prev_vp = m_PrevVpValid ? m_PrevVpNoJitter : vp_no_jitter;
    ExecuteSsrPass(m_Assets, *dev, *cl, *hdr, *depth,
                   view_proj_jittered, inv_vp, ssr_prev_vp,
                   m_CamPos, motion_tex, m_ShowSsr);
    if (m_ShowSsr) m_SsrWarm = true;
    ExecuteSsaoPass(m_Assets, *dev, *cl, *depth,
                    view_proj_jittered, inv_vp, m_Camera.View(), m_CamPos);
    m_SsaoWarm = true;

    // ===== Post-process (HDR -> LDR backbuffer) =====
    const u32 buf_idx = sc->AcquireNextImage();
    ExecuteBloomPass(m_Assets, *cl, *sc, buf_idx, *depth,
                     m_PostParams, vp_no_jitter, m_PrevVpNoJitter,
                     m_PrevVpValid, motion_tex);

    // ===== HUD overlay =====
    ExecuteHudPass(m_Assets, *cl, *sc, m_Paused, m_ShowSsr, m_ShowRefraction);

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();

    // 次フレーム用の前 VP / orb phase (TAA reprojection / motion 用)
    m_PrevVpNoJitter = vp_no_jitter;
    m_PrevOrbPhase    = m_OrbPhase;
    m_PrevVpValid     = true;
    return true;
}

void ShowcaseApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    ShutdownAssets(m_Assets);
}

} // namespace helloshowcase
