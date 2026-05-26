// SPDX-License-Identifier: Apache-2.0
// HelloIbl — HelloIblApp の実装 (薄い orchestration レイヤ)。
//
// 各 pass は helper file (ShadowPass / GBufferPass / ScreenSpaceEffects /
// RefractionPass / DynamicOrbs / SceneDraw / PbrLightingBindings /
// IblPresetBuilder / TaaJitter / ExposureControl / HudOverlay) に分割。
// ここでは OnStart で資源を確保し、OnCustomFrame でその helper 群を順に呼ぶ
// だけのフレーム駆動ロジックを記述する。
//
// 1 フレーム = OnCustomFrame() の中で:
//   ・preset 切替 → env / irradiance / prefilter 再生成 (必要なら)
//   ・shadow pass (CSM 3 cascade)
//   ・HDR RT: skybox → IBL sphere grid → 動的オーブ → ガラス球 (refraction)
//   ・geometry G-buffer pass: motion vector + world normal を MRT に焼く
//   ・SSR / SSAO / SSGI を計算 (次フレーム用に 1-frame latency)
//   ・PostProcess: Bloom + ACES tonemap + CAS sharpen + auto-expo + grading
//   ・SpriteBatch HUD (LDR backbuffer に sub-window + テキスト)
#include "HelloIblApp.h"
#include "IblEnvBuilder.h"
#include "IblLightmapBaker.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "math/Mat.h"
#include "math/Math.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloibl {

void HelloIblApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }
    IRhiSwapchain* sc = GetRenderer().Swapchain();
    if (!sc) { Quit(); return; }

    const u32 sw = sc->Width();
    const u32 sh = sc->Height();

    // HDR PostProcess (Bloom + ACES Tonemap) — HDR RT は R16G16B16A16_Float
    ACS_SAMPLE_INIT(_post.Init(*dev, sw, sh, GetRenderer().ColorFormat()));

    // シーン側は HDR RT format に揃える
    ACS_SAMPLE_INIT(_sky.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
    _sky.PresetDay();
    ACS_SAMPLE_INIT(_pbr.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(0.55f, 48, 24);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
    auto plane  = Primitive::MakePlane(40.0f, 40.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, _gm_plane));

    // ShadowMap: 2048 px × 3 cascade atlas (CSM)
    ACS_SAMPLE_INIT(_shadow.Init(*dev, 2048, /*cascade_count=*/3));
    // SSR: HDR と同フォーマット / 同サイズで scratch を確保
    ACS_SAMPLE_INIT(_ssr.Init(*dev, _post.HdrFormat(), sw, sh));
    // SSAO: depth から visibility を計算、frame size と同じ解像度
    ACS_SAMPLE_INIT(_ssao.Init(*dev, sw, sh));
    // SSGI: scene_color + depth → 1 bounce indirect light
    ACS_SAMPLE_INIT(_ssgi.Init(*dev, sw, sh));
    // Motion vector: 動的 mesh の screen-space motion を焼く
    ACS_SAMPLE_INIT(_motion.Init(*dev, sw, sh));

    // Refraction: screen-space 屈折 + 背景キャプチャ用 RT。
    // _bg_rt は opaque pass の HDR をコピーする先 (同一 RT の read+write 不可 回避)。
    // HDR と同じフォーマット・解像度で確保。
    ACS_SAMPLE_INIT(_refr.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(_blit.Init(*dev, _post.HdrFormat()));
    {
        TextureDesc bg_td{};
        bg_td.width            = sw;
        bg_td.height           = sh;
        bg_td.format           = _post.HdrFormat();
        bg_td.is_render_target = true;
        auto bg_r = CreateRhiTexture(*dev, bg_td);
        if (bg_r.IsErr()) {
            ACS_LOG_ERROR("HelloIbl: refraction background RT 作成に失敗: %s",
                          bg_r.Error().message);
            Quit();
            return;
        }
        _bg_rt = Move(bg_r.Value());
    }

    // 床用 lightmap を CPU 焼き。FSphere-FRay analytical hit で覆い、各 plane texel
    // で hemisphere sampling して visibility を求める。実装は IblLightmapBaker.cpp。
    {
        auto lm_r = BakeFloorLightmap(*dev);
        if (lm_r.IsErr()) {
            ACS_LOG_ERROR("HelloIbl: floor lightmap bake failed");
            Quit();
            return;
        }
        _lightmap = Move(lm_r.Value());
    }

    // SpriteBatch は LDR backbuffer (tonemap 後)
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    _cam_pos = FVec3{0, 1.0f, -5.0f};

    // Bloom 強度はデフォルトより少し弱めに (Day sky は十分明るいので)
    _post_params.bloom_threshold = 1.5f;
    _post_params.bloom_intensity = 0.4f;
    // 手動モード用の初期露出。preset Day = 0.7、adapted も同値に。
    _exposure_target   = 0.7f;
    _adapted_exposure  = 0.7f;
    // 既定で GPU auto-exposure を有効化。シーン輝度を GPU で実測して露出を
    // 自動算出 → preset 切替 (Day↔Night) でも自動で再露出され、eye adaptation
    // が演出として効く。'U' で手動モードへ切替可。
    _use_auto_exposure = true;
    _post_params.auto_exposure_enabled = true;
    _post_params.auto_exposure_key     = _auto_key;
    _post_params.auto_exposure_speed   = 1.5f;   // τ≈0.7s のシネマ的順応速度
    _post_params.exposure              = 1.0f;   // 露出は GPU 側が決める

    // Color grading cinematic look: 軽い暖色 + 彩度ブースト + コントラスト
    _post_params.cg_saturation   = 1.10f;
    _post_params.cg_contrast     = 1.08f;
    _post_params.cg_temperature  = 0.08f;     // 暖色寄り
    _post_params.cg_tint         = -0.02f;    // 軽く緑寄り (映画 teal-orange)
    _post_params.cg_lift         = FVec3{0.005f, 0.0f, 0.01f};      // 影に微かな青冷感
    _post_params.cg_gain         = FVec3{1.04f, 1.02f, 0.98f};      // ハイライトを暖色に

    // CAS sharpening: subtle clarity boost、UE5 デフォルト相当 0.4
    _post_params.cas_strength    = 0.4f;
    // SSR は PbrShader 側で合成するので tonemap 側の SSR は無効化。intensity 0 に
    // しないと fallback bloom mip が誤加算される。
    _post_params.ssr_intensity   = 0.0f;

    // 動的球の初期 transform。frame 0 で prev==curr になるよう curr と同値で
    // prev も初期化する。
    for (u32 i = 0; i < kDynCount; ++i) {
        _dyn_curr[i] = ComputeDynTransform(i, 0.0f);
        _dyn_prev[i] = _dyn_curr[i];
    }
}

FMat4 HelloIblApp::ComputeDynTransform(u32 i, f32 t) const noexcept {
    const f32 a = t * 1.8f + static_cast<f32>(i) * (2.0f * kPi / 3.0f);
    const f32 r = 2.2f;
    return FMat4::Translation(FVec3{ Cos(a) * r, 3.0f + Sin(a) * r, 1.0f });
}

void HelloIblApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();

    // 1/2/3/4/5 で env preset 切替。SH9 mode が有効中は SH 9 係数も再計算が必要。
    // preset ごとに露出目標を変える → _adapted_exposure がじわっと追従して
    // eye adaptation (目が明暗に慣れる) 演出になる。
    if (Input::IsKeyPressed(EKey::Num1)) {
        _sky.PresetDay();    _current_preset = 0;
        _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        _exposure_target = 0.7f;     // Day: 明るいので露出を絞る
    }
    if (Input::IsKeyPressed(EKey::Num2)) {
        _sky.PresetSunset(); _current_preset = 1;
        _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        _exposure_target = 1.0f;
    }
    if (Input::IsKeyPressed(EKey::Num3)) {
        _sky.PresetNight();  _current_preset = 2;
        _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        _exposure_target = 1.8f;     // Night: 暗いので露出を上げる
    }
    if (Input::IsKeyPressed(EKey::Num4)) {
        _current_preset = 3;
        _need_studio_hdr = true; _need_sh9_rebuild = _use_sh9;
        _exposure_target = 1.0f;
    }
    if (Input::IsKeyPressed(EKey::Num5)) {
        _current_preset = 4;
        _need_atmosphere = true; _need_sh9_rebuild = _use_sh9;
        _exposure_target = 0.85f;
    }

    if (Input::IsKeyPressed(EKey::I)) _display_mode = (_display_mode + 1) % 7;
    // SH9 toggle: 現在の irradiance cubemap から計算した SH 9 で diffuse を再構築
    if (Input::IsKeyPressed(EKey::S)) {
        _use_sh9 = !_use_sh9;
        _need_sh9_rebuild = _use_sh9;     // 必要なときに再計算
    }
    if (Input::IsKeyPressed(EKey::C)) _use_clearcoat = !_use_clearcoat;
    if (Input::IsKeyPressed(EKey::Z)) _use_anisotropy = !_use_anisotropy;
    if (Input::IsKeyPressed(EKey::L)) _use_area_light = !_use_area_light;
    if (Input::IsKeyPressed(EKey::G)) _use_probe_grid = !_use_probe_grid;
    if (Input::IsKeyPressed(EKey::F)) _use_fog = !_use_fog;
    if (Input::IsKeyPressed(EKey::H)) _use_shadows = !_use_shadows;
    if (Input::IsKeyPressed(EKey::R)) _show_ssr = !_show_ssr;
    if (Input::IsKeyPressed(EKey::X)) _show_refraction = !_show_refraction;
    if (Input::IsKeyPressed(EKey::O)) _use_ssao = !_use_ssao;
    if (Input::IsKeyPressed(EKey::T)) _use_taa  = !_use_taa;
    if (Input::IsKeyPressed(EKey::J)) _use_ssgi = !_use_ssgi;
    if (Input::IsKeyPressed(EKey::K)) _use_lightmap = !_use_lightmap;
    if (Input::IsKeyPressed(EKey::M)) _use_motion_vec = !_use_motion_vec;
    if (Input::IsKeyPressed(EKey::B)) _post_params.bloom_enabled = !_post_params.bloom_enabled;

    // film grain アニメ用に時間累積 + 動的球の公転時刻
    _post_params.grain_time += dt;
    _anim_time += dt;

    UpdateExposureControls(*this, dt);

    // 視点を矢印 (回転) + WASD (移動) で操作
    const f32 mv = 4.0f * dt, tr = 1.5f * dt;
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= tr;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += tr;
    if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= tr * 0.8f;
    if (Input::IsKeyDown(EKey::Down))  _cam_pitch += tr * 0.8f;
    const f32 limit = 0.45f * kPi;
    if (_cam_pitch >  limit) _cam_pitch =  limit;
    if (_cam_pitch < -limit) _cam_pitch = -limit;

    FVec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                 -Sin(_cam_pitch),
                  Cos(_cam_yaw) * Cos(_cam_pitch) };
    FVec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
    if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * mv;
    if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * mv;
    if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * mv;
    if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * mv;
    _camera.SetLookAt(_cam_pos, _cam_pos + forward);
}

// OnCustomFrame() で HDR 経路に切替えてデフォルトフローを置き換える。
// 1) IBL build (必要なら、RT を一時的に切替)
// 2) HDR RT にシーン (skybox + sphere grid) を描画
// 3) PostProcess.Render で Bloom + Tonemap → LDR backbuffer
// 4) SpriteBatch HUD を LDR backbuffer に
bool HelloIblApp::OnCustomFrame() noexcept {
    IRhiDevice*      dev   = GetRenderer().Device();
    IRhiCommandList* cl    = GetRenderer().CommandList();
    IRhiSwapchain*   sc    = GetRenderer().Swapchain();
    IRhiTexture*     hdr   = _post.HdrRenderTarget();
    IRhiTexture*     depth = GetRenderer().DepthBuffer();
    if (!dev || !cl || !sc || !hdr) return false;

    UpdateDynamicOrbs(*this);

    // TAA Halton(2,3) sub-pixel jitter を skybox / PbrShader / SSR / SSAO の
    // VP に適用する。複数フレームの累積でエッジが滑らかになる。
    const FMat4 vp_no_jitter = _camera.ViewProjection();
    const FMat4 vp_for_render = BuildJitteredViewProjection(*this, vp_no_jitter,
                                                           hdr->Width(), hdr->Height());

    const u32 buf_idx = sc->AcquireNextImage();
    cl->Begin();

    ApplyPresetRebuilds(*this);

    // ===== Shadow pass (CSM、'H' で有効) =====
    const FVec3 sun_dir = ResolveSunDirection(*this);
    RenderShadowPass(*this, sun_dir);

    // ===== 1) HDR RT にシーン描画 =====
    cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);

    Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                   vp.height = static_cast<f32>(hdr->Height());
    cl->SetViewport(vp);
    ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                       svr.bottom = static_cast<i32>(hdr->Height());
    cl->SetScissor(svr);

    // 背景 skybox。表示モードに応じて env / irradiance / prefilter を切替える。
    IRhiTexture* display_cube = nullptr;
    f32          mip_level    = 0.0f;
    if (_display_mode == 0) {
        display_cube = _ibl.EnvCubemap();
    } else if (_display_mode == 1) {
        display_cube = _ibl.IrradianceMap();
    } else {
        display_cube = _ibl.PrefilterMap();
        mip_level    = static_cast<f32>(_display_mode - 2);
    }
    if (display_cube) {
        _ibl.DrawSkybox(*dev, *cl, *display_cube,
                        vp_for_render, _camera.Eye(),
                        _post.HdrFormat(), GetRenderer().DepthFormat(),
                        mip_level);
    }

    // SH9 mode: 現在の env cubemap (sky or studio HDR) から SH 9 を計算
    if (_need_sh9_rebuild) {
        // Studio HDR は別 builder で既に焼かれている。それ以外は Sky 評価から焼く。
        if (_current_preset != 3) {
            BuildEquirectFromSky(_sky, _equirect_rgba);
        }
        ImageBasedLighting::ComputeSh9FromEquirect(
            _equirect_rgba.Data(), kEquirectWidth, kEquirectHeight, _sh9);
        _need_sh9_rebuild = false;
    }

    // 太陽の direct light を 1 灯追加 (Studio HDR では中央パネルを sun に見立てる)。
    // これで clear-coat / anisotropic の direct specular が見える。
    DirLight sun;
    if (_current_preset == 3) {
        sun.direction = FVec3{0, 0.4f, 1.0f};
        sun.color     = FVec3{0.7f, 0.7f, 0.7f};
    } else {
        sun.direction = _sky.SunDirection();
        sun.color     = _sky.SunColor() * 0.9f;
    }

    BindPbrLighting(*this, vp_for_render, sun);
    DrawFloor(*this);
    DrawSphereGrid(*this);
    DrawDynamicOrbs(*this);

    cl->EndRenderToTexture(*hdr);

    // ===== Refraction pass (ガラス球) =====
    RenderRefractionPass(*this, vp_for_render, vp, svr);

    // ===== Geometry G-buffer pass (motion + normal) =====
    RenderMotionAndNormalGBuffer(*this, vp_no_jitter);

    // motion texture を TAA / SSGI へ渡す。frame 0 は prev VP 未確定なので渡さず、
    // depth reprojection 側の cold-start ガードに委ねる。
    IRhiTexture* motion_tex =
        (_use_motion_vec && _taa_prev_vp_valid) ? _motion.OutputTexture() : nullptr;
    _post_params.taa_motion_texture = motion_tex;

    // ===== Screen-space effects (1-frame latency) =====
    const FMat4 inv_vp = Inverse(vp_for_render);
    RenderSsrPass(*this, vp_for_render, inv_vp, vp_no_jitter);
    RenderSsaoPass(*this, vp_for_render, inv_vp, sun.direction);
    RenderSsgiPass(*this, vp_for_render, inv_vp, vp_no_jitter);

    // TAA を毎フレーム params に反映。
    // 注意 (frame 0 garbage 回避): まだ _prev_vp_no_jitter が default (identity) のときに
    // reproject すると world 座標を clip 座標と誤解して prev_ndc が破綻する。
    // _taa_prev_vp_valid フラグで「前フレーム VP を本物で書いた」状態を保証してから
    // depth_texture を渡す。最初の 1 フレームは depth=null で reproject 無効化。
    _post_params.taa_enabled                  = _use_taa;
    _post_params.taa_blend_factor             = 0.1f;     // current 10% + history 90%
    _post_params.taa_depth_texture            = (_use_taa && _taa_prev_vp_valid) ? depth : nullptr;
    _post_params.taa_view_proj_no_jitter      = vp_no_jitter;
    _post_params.taa_prev_view_proj_no_jitter = _prev_vp_no_jitter;

    // ===== 2) Bloom + ACES Tonemap → LDR backbuffer (SSR も additive mix) =====
    _post.Render(*cl, *sc, buf_idx, _post_params);

    // 次フレーム用に保存 (jitter なしの true VP)
    _prev_vp_no_jitter = vp_no_jitter;
    _taa_prev_vp_valid = true;

    // ===== 3) SpriteBatch HUD (LDR backbuffer) =====
    DrawHud(*this, sc->Width(), sc->Height());

    cl->EndRenderToSwapchain(*sc, buf_idx);
    cl->End();
    cl->Submit();
    sc->Present();
    return true;
}

void HelloIblApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _bg_rt.Reset();
    _blit.Shutdown();
    _refr.Shutdown();
    _motion.Shutdown();
    _ssgi.Shutdown();
    _ssao.Shutdown();
    _ssr.Shutdown();
    _shadow.Shutdown();
    _gm_plane = GpuMesh{};
    _gm_sphere = GpuMesh{};
    _pbr.Shutdown();
    _ibl.Shutdown();
    _sky.Shutdown();
    _post.Shutdown();
}

} // namespace helloibl
