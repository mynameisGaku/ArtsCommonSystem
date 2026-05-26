// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — FApplication 派生のメインクラス。
// PBR / IBL / SSR / SSAO / Refraction / Bloom / ACES + TAA を 1 つのフレームで
// 連携させる cinematic demo。auto-orbit カメラで scene を 1 周。
//
// 実装は pass 系 helper (PbrPass / RefractionPass / MotionPass / SsrPass /
// BloomPass / HudPass) と GPU resource bundle (Assets) に分割している。
// ShowcaseApp 自身は state (camera, orbit, toggle) と OnStart / OnUpdate /
// OnCustomFrame の orchestration だけを持つ。
//
// キー:
//   P  : auto-orbit を pause / resume
//   R  : SSR (env reflection mix) の toggle
//   X  : refraction glass の toggle
//   Esc: 終了
#pragma once

#include "ShowcaseAssets.h"

#include "app/Application.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "render/PostProcess.h"

namespace helloshowcase {

class ShowcaseApp : public acs::FApplication {
public:
    ShowcaseApp() noexcept;
    ~ShowcaseApp() noexcept override;

    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    Assets                  _assets;
    acs::FCamera             _camera;
    acs::FPostProcessParams  _post_params;
    acs::FVec3               _cam_pos        = acs::FVec3{0, 1.4f, -5.5f};
    acs::f32                _orbit_angle    = 0.0f;     // カメラ orbit (rad)
    acs::f32                _orb_phase      = 0.0f;     // emissive オーブの位相 (rad)
    acs::f32                _prev_orb_phase = 0.0f;
    acs::f32                _exposure_target  = 0.7f;
    acs::f32                _adapted_exposure = 0.7f;
    acs::FMat4               _prev_vp_no_jitter{};
    bool                    _prev_vp_valid   = false;
    acs::u32                _taa_frame_index = 0;
    bool                    _show_ssr        = true;
    bool                    _ssr_warm        = false;
    bool                    _ssao_warm       = false;
    bool                    _show_refraction = true;
    bool                    _paused          = false;
};

} // namespace helloshowcase
