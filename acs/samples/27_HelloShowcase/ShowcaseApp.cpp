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
    ACS_SAMPLE_INIT(InitializeAssets(_assets, *dev, sw, sh,
                                      GetRenderer().ColorFormat(),
                                      GetRenderer().DepthFormat()));

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    _camera.SetPerspective(45.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    // post params: 控えめな bloom + ACES + mild vignette。
    // sun.color が 1.2x (HelloIbl 0.9x 互換) なので threshold もそれ相応に下げて
    // emissive orb (strength=4.0) の bloom がちゃんと光るようにする。
    _post_params.bloom_threshold      = 1.0f;
    _post_params.bloom_intensity      = 0.40f;
    _post_params.vignette_intensity   = 0.25f;
    _post_params.chromatic_aberration = 0.0f;        // 邪魔なので OFF
    _post_params.grain_intensity      = 0.0f;
    _post_params.tonemap_kind         = 0;           // 0=ACES (Narkowicz)

    // exposure を Day 想定にセット
    _exposure_target  = 0.7f;
    _adapted_exposure = 0.7f;
}

void ShowcaseApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) { Quit(); return; }
    if (Input::IsKeyPressed(EKey::P)) _paused = !_paused;
    if (Input::IsKeyPressed(EKey::R)) _show_ssr = !_show_ssr;
    if (Input::IsKeyPressed(EKey::X)) _show_refraction = !_show_refraction;

    if (!_paused) {
        _orbit_angle += dt * 0.20f;     // 約 31 秒で 1 周
        _orb_phase   += dt * 0.50f;
    }

    // カメラ: scene 中心 (0, 0.4, 0) を見ながら半径 5.5 で水平回転 +
    // 微小な垂直 bob (cinematic ペン回し風)
    const f32 cam_radius = 5.5f;
    const f32 cam_y_base = 1.4f;
    const f32 cam_y_bob  = 0.18f * Sin(_orbit_angle * 1.3f);     // ±18cm 縦揺れ
    const FVec3 cam_target{0.0f, 0.4f, 0.0f};
    _cam_pos = FVec3{
        cam_target.x + cam_radius * Sin(_orbit_angle),
        cam_y_base + cam_y_bob,
        cam_target.z + cam_radius * Cos(_orbit_angle),
    };
    _camera.SetLookAt(_cam_pos, cam_target, FVec3::Up());
}

bool ShowcaseApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    IRhiTexture*     hdr   = _assets.post.HdrRenderTarget();
    if (!dev || !cl || !sc || !hdr || !depth) return false;

    cl->Begin();

    // ===== TAA jitter (Halton 2,3) =====
    // Halton(0, b) = 0 を避けるため +1 オフセット (HelloIbl と同様)
    const f32 jx = Halton((_taa_frame_index & 31) + 1, 2) - 0.5f;
    const f32 jy = Halton((_taa_frame_index & 31) + 1, 3) - 0.5f;
    ++_taa_frame_index;
    const f32 jx_ndc = jx * 2.0f / static_cast<f32>(hdr->Width());
    const f32 jy_ndc = jy * 2.0f / static_cast<f32>(hdr->Height());
    const FMat4 view_proj_jittered = _camera.ViewProjection() *
                                    FMat4::Translation(FVec3{jx_ndc, jy_ndc, 0.0f});
    const FMat4 vp_no_jitter = _camera.ViewProjection();
    const FMat4 inv_vp        = Inverse(view_proj_jittered);

    // ===== IBL warmup (1 度だけ走る) =====
    if (!_assets.ibl.HasBrdfLut())       (void)_assets.ibl.EnsureBrdfLut(*dev, *cl);
    if (!_assets.ibl.HasEnvCubemap())    (void)_assets.ibl.EnsureEnvCubemap(*dev, *cl, _assets.sky);
    if (!_assets.ibl.HasIrradianceMap()) (void)_assets.ibl.EnsureIrradiance(*dev, *cl);
    if (!_assets.ibl.HasPrefilterMap())  (void)_assets.ibl.EnsurePrefilter(*dev, *cl);

    // ===== Opaque HDR pass (FSky + PBR + emissive orb) =====
    FMat4 orb_curr[kOrbCount]{};
    ExecutePbrPass(_assets, *cl, *hdr, *depth, _camera,
                   view_proj_jittered, _cam_pos, _orb_phase,
                   _ssr_warm, _ssao_warm, orb_curr);

    // ===== Refraction pass (clear / frosted glass) =====
    if (_show_refraction) {
        ExecuteRefractionPass(_assets, *cl, *hdr, *depth,
                              view_proj_jittered, _cam_pos);
    }

    // ===== Motion + normal G-buffer pass (TAA / SSR / SSAO 用) =====
    IRhiTexture* motion_tex = ExecuteMotionPass(_assets, *cl, vp_no_jitter,
                                                 _prev_vp_no_jitter, _prev_vp_valid,
                                                 _prev_orb_phase, orb_curr);

    // ===== SSR / SSAO (1-frame latency で次フレームの PBR が合成) =====
    const FMat4& ssr_prev_vp = _prev_vp_valid ? _prev_vp_no_jitter : vp_no_jitter;
    ExecuteSsrPass(_assets, *dev, *cl, *hdr, *depth,
                   view_proj_jittered, inv_vp, ssr_prev_vp,
                   _cam_pos, motion_tex, _show_ssr);
    if (_show_ssr) _ssr_warm = true;
    ExecuteSsaoPass(_assets, *dev, *cl, *depth,
                    view_proj_jittered, inv_vp, _camera.View(), _cam_pos);
    _ssao_warm = true;

    // ===== Post-process (HDR -> LDR backbuffer) =====
    const u32 buf_idx = sc->AcquireNextImage();
    ExecuteBloomPass(_assets, *cl, *sc, buf_idx, *depth,
                     _post_params, vp_no_jitter, _prev_vp_no_jitter,
                     _prev_vp_valid, motion_tex);

    // ===== HUD overlay =====
    ExecuteHudPass(_assets, *cl, *sc, _paused, _show_ssr, _show_refraction);

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();

    // 次フレーム用の前 VP / orb phase (TAA reprojection / motion 用)
    _prev_vp_no_jitter = vp_no_jitter;
    _prev_orb_phase    = _orb_phase;
    _prev_vp_valid     = true;
    return true;
}

void ShowcaseApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    ShutdownAssets(_assets);
}

} // namespace helloshowcase
