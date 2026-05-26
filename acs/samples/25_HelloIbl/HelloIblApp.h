// SPDX-License-Identifier: Apache-2.0
// HelloIbl — Application 派生クラス。
//
// IBL + HDR / ACES tonemap のメインデモ。BRDF LUT / env cube / irradiance /
// prefilter を初フレームで一括生成し、以降は preset 切替 / displaymode /
// material lobe 行 (sheen / iridescence / subsurface) と post-process (Bloom +
// ACES + CAS sharpening + auto-exposure + grading) を組み合わせて表示する。
// Shadow / SSR / SSAO / SSGI / Motion / TAA / Refraction / Lightmap が全部入りで
// キーで toggle 可能。フル機能 sample。
//
// 実装は機能ごとに分割している:
//   ShadowPass.{h,cpp}            - CSM (3 cascade atlas) caster
//   GBufferPass.{h,cpp}           - motion vector + world normal MRT
//   ScreenSpaceEffects.{h,cpp}    - SSR / SSAO / SSGI dispatch
//   RefractionPass.{h,cpp}        - screen-space 屈折ガラス
//   DynamicOrbs.{h,cpp}           - 公転する発光オーブ
//   SceneDraw.{h,cpp}             - 5x5 sphere grid + floor の PBR draw
//   PbrLightingBindings.{h,cpp}   - PbrShader の各種テクスチャ/パラメータ bind
//   IblPresetBuilder.{h,cpp}      - preset 切替時の env / irradiance / prefilter 再生成
//   TaaJitter.{h,cpp}             - Halton(2,3) sub-pixel jitter
//   ExposureControl.{h,cpp}       - auto / manual 露出補間
//   HudOverlay.{h,cpp}            - SpriteBatch HUD + デバッグオーバーレイ
//
// 操作 (主要なもの):
//   1/2/3 Sky preset / 4 Studio HDR / 5 Atmosphere
//   I display mode / S SH9 / C clear-coat / Z aniso / L area / G probe / F fog
//   H shadow / R SSR / X refraction / O SSAO / T TAA / J SSGI / K lightmap / M motion
//   B bloom / U auto-expo / Q-E expo / WASD 移動 / 矢印 視点 / Esc 終了
#pragma once

#include "app/Application.h"
#include "render/Ibl.h"
#include "render/Sky.h"
#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/ShadowMap.h"
#include "render/Ssr.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/MotionVector.h"
#include "render/RefractionShader.h"
#include "render/Blit.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"

#include "container/Array.h"
#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Vec.h"
#include "memory/UniquePtr.h"
#include "foundation/Types.h"

#include "IblTypes.h"

namespace helloibl {

// 機能別 helper の前方宣言 (friend にして private メンバへアクセスさせる)。
class HelloIblApp;
void ApplyPresetRebuilds(HelloIblApp& app) noexcept;
acs::FMat4 BuildJitteredViewProjection(HelloIblApp& app, const acs::FMat4& vp_no_jitter,
                                      acs::u32 hdr_width, acs::u32 hdr_height) noexcept;
void RenderShadowPass(HelloIblApp& app, const acs::FVec3& sun_dir) noexcept;
acs::FVec3 ResolveSunDirection(const HelloIblApp& app) noexcept;
void BindPbrLighting(HelloIblApp& app, const acs::FMat4& vp_for_render,
                     const acs::DirLight& sun) noexcept;
void DrawFloor(HelloIblApp& app) noexcept;
void DrawSphereGrid(HelloIblApp& app) noexcept;
void UpdateDynamicOrbs(HelloIblApp& app) noexcept;
void DrawDynamicOrbs(HelloIblApp& app) noexcept;
void RenderRefractionPass(HelloIblApp& app, const acs::FMat4& vp_for_render,
                          const acs::Viewport& vp, const acs::ScissorRect& svr) noexcept;
void RenderMotionAndNormalGBuffer(HelloIblApp& app,
                                  const acs::FMat4& vp_no_jitter) noexcept;
void RenderSsrPass(HelloIblApp& app, const acs::FMat4& vp_for_render,
                   const acs::FMat4& inv_vp, const acs::FMat4& vp_no_jitter) noexcept;
void RenderSsaoPass(HelloIblApp& app, const acs::FMat4& vp_for_render,
                    const acs::FMat4& inv_vp, const acs::FVec3& sun_dir) noexcept;
void RenderSsgiPass(HelloIblApp& app, const acs::FMat4& vp_for_render,
                    const acs::FMat4& inv_vp, const acs::FMat4& vp_no_jitter) noexcept;
void UpdateExposureControls(HelloIblApp& app, acs::f32 dt) noexcept;
void DrawHud(HelloIblApp& app, acs::u32 sw, acs::u32 sh) noexcept;

class HelloIblApp : public acs::Application {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    // 動的球 i の時刻 t における transform。中心 (0, 3, 1) のまわりを XY 平面で
    // 公転させる (画面内を大きく掃くので、motion vector 無しだと TAA で trail が出る)。
    acs::FMat4 ComputeDynTransform(acs::u32 i, acs::f32 t) const noexcept;

    // helper モジュール群は private メンバを直接触る (friend)。
    friend void ApplyPresetRebuilds(HelloIblApp&) noexcept;
    friend acs::FMat4 BuildJitteredViewProjection(HelloIblApp&, const acs::FMat4&,
                                                 acs::u32, acs::u32) noexcept;
    friend void RenderShadowPass(HelloIblApp&, const acs::FVec3&) noexcept;
    friend acs::FVec3 ResolveSunDirection(const HelloIblApp&) noexcept;
    friend void BindPbrLighting(HelloIblApp&, const acs::FMat4&, const acs::DirLight&) noexcept;
    friend void DrawFloor(HelloIblApp&) noexcept;
    friend void DrawSphereGrid(HelloIblApp&) noexcept;
    friend void UpdateDynamicOrbs(HelloIblApp&) noexcept;
    friend void DrawDynamicOrbs(HelloIblApp&) noexcept;
    friend void RenderRefractionPass(HelloIblApp&, const acs::FMat4&,
                                     const acs::Viewport&, const acs::ScissorRect&) noexcept;
    friend void RenderMotionAndNormalGBuffer(HelloIblApp&, const acs::FMat4&) noexcept;
    friend void RenderSsrPass(HelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                              const acs::FMat4&) noexcept;
    friend void RenderSsaoPass(HelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                               const acs::FVec3&) noexcept;
    friend void RenderSsgiPass(HelloIblApp&, const acs::FMat4&, const acs::FMat4&,
                               const acs::FMat4&) noexcept;
    friend void UpdateExposureControls(HelloIblApp&, acs::f32) noexcept;
    friend void DrawHud(HelloIblApp&, acs::u32, acs::u32) noexcept;

    acs::PostProcess        _post;
    acs::ImageBasedLighting _ibl;
    acs::Sky                _sky;
    acs::PbrShader          _pbr;
    acs::GpuMesh            _gm_sphere;
    acs::GpuMesh            _gm_plane;
    acs::SpriteBatch        _batch;
    acs::Font               _font;
    acs::Camera             _camera;
    acs::PostProcessParams  _post_params;
    acs::TArray<acs::f32>    _equirect_rgba;          // 4 ch float
    acs::FVec4               _sh9[9]   = {};          // 計算済 SH 9 係数 (xyz=RGB)
    acs::FVec3               _cam_pos  = acs::FVec3{0, 1.0f, -5.0f};
    acs::f32                _cam_yaw   = 0.0f;
    acs::f32                _cam_pitch = 0.0f;
    acs::i32                _current_preset = 0;
    bool                    _need_recapture   = false;
    bool                    _need_studio_hdr  = false;
    bool                    _use_sh9          = false;
    bool                    _need_sh9_rebuild = false;
    bool                    _use_clearcoat    = false;
    bool                    _use_anisotropy   = false;
    bool                    _use_area_light   = false;
    bool                    _use_probe_grid   = false;
    bool                    _use_fog          = false;
    bool                    _need_atmosphere  = false;
    bool                    _use_shadows      = false;
    bool                    _show_ssr         = false;
    bool                    _ssr_warm         = false; // _ssr.Render が 1 度以上走った？
    bool                    _use_ssao         = true;  // PbrShader 側で composite (1-frame latency)
    bool                    _ssao_warm        = false; // _ssao.Render が 1 度以上走った？ (frame 0 garbage 回避)
    bool                    _use_taa          = true;
    acs::u32                _taa_frame_index  = 0;     // Halton(2,3) 用カウンタ
    acs::FMat4               _prev_vp_no_jitter{};      // 前フレームの jitter なし VP
    bool                    _taa_prev_vp_valid = false;// 上が本物の VP (default identity 以外) か
    acs::f32                _exposure_target  = 0.7f;  // 露出目標 (preset / Q-E で動く)
    acs::f32                _adapted_exposure = 0.7f;  // 実露出 (target へ dt 補間)
    bool                    _use_auto_exposure = true; // GPU auto-exposure ('U' で手動切替)
    acs::f32                _auto_key          = 0.5f; // 自動露出の目標平均輝度 (Q/E で調整)
    bool                    _use_ssgi         = true;
    bool                    _ssgi_warm        = false; // _ssgi.Render が 1 度以上走った？
    bool                    _use_lightmap     = true;
    acs::TUniquePtr<acs::IRhiTexture> _lightmap;        // 床用 baked lightmap (256x256 RGBA8)
    acs::ShadowMap          _shadow;
    acs::Ssr                _ssr;
    acs::Ssao               _ssao;
    acs::Ssgi               _ssgi;
    acs::MotionVector       _motion;
    bool                    _use_motion_vec   = true;
    acs::RefractionShader   _refr;                     // screen-space 屈折
    acs::Blit               _blit;                     // HDR -> _bg_rt コピー
    acs::TUniquePtr<acs::IRhiTexture> _bg_rt;           // 屈折用 background キャプチャ
    bool                    _show_refraction  = true;
    acs::FMat4               _dyn_curr[kDynCount] = {}; // 動的球の現フレーム transform
    acs::FMat4               _dyn_prev[kDynCount] = {}; // 動的球の前フレーム transform
    acs::f32                _anim_time        = 0.0f;  // 動的球公転の時刻アキュムレータ
    acs::u32                _display_mode     = 0;
};

} // namespace helloibl
