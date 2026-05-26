// SPDX-License-Identifier: Apache-2.0
// HelloIbl — Application 派生クラス。
//
// Phase 31 IBL + Phase 32a HDR / ACES tonemap のメインデモ。BRDF LUT / env cube /
// irradiance / prefilter を初フレームで一括生成し、以降は preset 切替 / displaymode /
// material lobe 行 (sheen / iridescence / subsurface) と post-process (Bloom + ACES +
// CAS sharpening + auto-exposure + grading) を組み合わせて表示する。
// Shadow / SSR / SSAO / SSGI / Motion / TAA / Refraction / Lightmap が全部入りで
// キーで toggle 可能。フル機能 sample。
//
// 状態カテゴリ:
//   - レンダリング資源: PostProcess / Sky / IBL / PbrShader / ShadowMap / SSR /
//                       SSAO / SSGI / MotionVector / Refraction / Blit
//   - シーン資源: GpuMesh (sphere + plane)、_lightmap、_bg_rt、_equirect_rgba、_sh9
//   - カメラ: Camera + _cam_pos / _cam_yaw / _cam_pitch
//   - 動的 mesh: _dyn_curr / _dyn_prev (kDynCount)
//   - トグルフラグ: _use_xxx / _show_xxx と warm-up フラグ (_xxx_warm)
//   - TAA: _taa_frame_index / _prev_vp_no_jitter / _taa_prev_vp_valid
//   - exposure: _exposure_target / _adapted_exposure / _auto_key / _use_auto_exposure
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

class HelloIblApp : public acs::Application {
public:
    void OnStart() noexcept override;
    void OnUpdate(acs::f32 dt) noexcept override;
    bool OnCustomFrame() noexcept override;
    void OnShutdown() noexcept override;

private:
    // 動的球 i の時刻 t における transform。中心 (0, 3, 1) のまわりを XY 平面で
    // 公転させる (画面内を大きく掃くので、motion vector 無しだと TAA で trail が出る)。
    acs::Mat4 ComputeDynTransform(acs::u32 i, acs::f32 t) const noexcept;

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
    acs::Array<acs::f32>    _equirect_rgba;          // 4 ch float
    acs::Vec4               _sh9[9]   = {};          // 計算済 SH 9 係数 (xyz=RGB)
    acs::Vec3               _cam_pos  = acs::Vec3{0, 1.0f, -5.0f};
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
    bool                    _ssr_warm         = false; // Phase 34e-2fix: _ssr.Render が 1 度以上走った？
    bool                    _use_ssao         = true;  // Phase 34j-2: PbrShader 側で composite (1-frame latency)
    bool                    _ssao_warm        = false; // _ssao.Render が 1 度以上走った？ (frame 0 garbage 回避)
    bool                    _use_taa          = true;  // Phase 34f: TAA 有効 ('T' でトグル)
    acs::u32                _taa_frame_index  = 0;     // Halton(2,3) 用カウンタ
    acs::Mat4               _prev_vp_no_jitter{};      // Phase 34f-2: 前フレームの jitter なし VP
    bool                    _taa_prev_vp_valid = false;// 上が本物の VP (default identity 以外) か
    acs::f32                _exposure_target  = 0.7f;  // Phase 34k: 露出目標 (preset / Q-E で動く)
    acs::f32                _adapted_exposure = 0.7f;  // Phase 34k: 実露出 (target へ dt 補間)
    bool                    _use_auto_exposure = true; // Phase 34k-2: GPU auto-exposure ('U' で手動切替)
    acs::f32                _auto_key          = 0.5f; // Phase 34k-2: 自動露出の目標平均輝度 (Q/E で調整)
    bool                    _use_ssgi         = true;  // Phase 33c: SSGI 有効 ('J' でトグル)
    bool                    _ssgi_warm        = false; // _ssgi.Render が 1 度以上走った？
    bool                    _use_lightmap     = true;  // Phase 33f: lightmap 有効 ('K' でトグル)
    acs::UniquePtr<acs::IRhiTexture> _lightmap;        // 床用 baked lightmap (256x256 RGBA8)
    acs::ShadowMap          _shadow;
    acs::Ssr                _ssr;
    acs::Ssao               _ssao;
    acs::Ssgi               _ssgi;
    acs::MotionVector       _motion;                   // Phase 34f-3: 動的 mesh motion vector
    bool                    _use_motion_vec   = true;  // Phase 34f-3: 'M' で toggle
    acs::RefractionShader   _refr;                     // Phase 35-3b/3c: screen-space 屈折
    acs::Blit               _blit;                     // Phase 35-3b/3c: HDR -> _bg_rt コピー
    acs::UniquePtr<acs::IRhiTexture> _bg_rt;           // Phase 35-3b/3c: 屈折用 background キャプチャ
    bool                    _show_refraction  = true;  // 'X' で glass sphere demo toggle
    acs::Mat4               _dyn_curr[kDynCount] = {}; // 動的球の現フレーム transform
    acs::Mat4               _dyn_prev[kDynCount] = {}; // 動的球の前フレーム transform
    acs::f32                _anim_time        = 0.0f;  // 動的球公転の時刻アキュムレータ
    acs::u32                _display_mode     = 0;
};

} // namespace helloibl
