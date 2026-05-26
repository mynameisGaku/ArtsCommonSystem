// SPDX-License-Identifier: Apache-2.0
// HelloShowcase — Application 派生のメインクラス。
// PBR / IBL / SSR / SSAO / Refraction / Bloom / ACES + TAA を 1 つのフレームで
// 連携させる cinematic demo。auto-orbit カメラで scene を 1 周。
//
// キー:
//   P  : auto-orbit を pause / resume
//   R  : SSR (env reflection mix) の toggle
//   X  : refraction glass の toggle
//   Esc: 終了
#pragma once

#include "app/Application.h"

#include "asset/MeshAsset.h"

#include "render/Ibl.h"
#include "render/Sky.h"
#include "render/PbrShader.h"
#include "render/RefractionShader.h"
#include "render/Blit.h"
#include "render/Ssr.h"
#include "render/Ssao.h"
#include "render/HiZ.h"
#include "render/MotionVector.h"
#include "render/PostProcess.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Camera.h"
#include "math/Mat.h"

namespace helloshowcase {

class ShowcaseApp : public acs::Application {
public:
    ShowcaseApp() noexcept;
    ~ShowcaseApp() noexcept override;

    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    acs::PostProcess        _post;
    acs::ImageBasedLighting _ibl;
    acs::Sky                _sky;
    acs::PbrShader          _pbr;
    acs::Ssr                _ssr;
    acs::Ssao               _ssao;
    acs::HiZ                _hiz;            // Phase 36-3a: SSR の skip-ahead 用 coarse min-depth
    acs::MotionVector       _motion;
    acs::RefractionShader   _refr;
    acs::Blit               _blit;
    acs::UniquePtr<acs::IRhiTexture> _bg_rt;
    acs::GpuMesh            _gm_sphere;
    acs::GpuMesh            _gm_floor;
    acs::SpriteBatch        _batch;
    acs::Font               _font;
    acs::Camera             _camera;
    acs::PostProcessParams  _post_params;
    acs::Vec3               _cam_pos        = acs::Vec3{0, 1.4f, -5.5f};
    acs::f32                _orbit_angle    = 0.0f;     // カメラ orbit (rad)
    acs::f32                _orb_phase      = 0.0f;     // emissive オーブの位相 (rad)
    acs::f32                _prev_orb_phase = 0.0f;
    acs::f32                _exposure_target  = 0.7f;
    acs::f32                _adapted_exposure = 0.7f;
    acs::Mat4               _prev_vp_no_jitter{};
    bool                    _prev_vp_valid   = false;
    acs::u32                _taa_frame_index = 0;
    bool                    _show_ssr        = true;
    bool                    _ssr_warm        = false;
    bool                    _ssao_warm       = false;
    bool                    _show_refraction = true;
    bool                    _paused          = false;
};

} // namespace helloshowcase
