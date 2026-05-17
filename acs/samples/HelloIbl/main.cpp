// ACS の HelloIbl サンプル — Phase 31 IBL + Phase 32a HDR / ACES tonemap
//
// 動作:
//   ・初フレームで BRDF LUT (256x256 RG16F) + env cubemap (256x256x6 R11G11B10F)
//     + irradiance (32x32x6) + prefilter (128x128x6 5 mips) を一括生成
//   ・以降のフレームで:
//     - 背景: env / irradiance / prefilter mip 0..4 を切替 (I キー)
//     - 5x5 sphere grid を IBL のみで点灯 (X=metallic, Y=roughness)
//     - BRDF LUT を画面右上にオーバーレイ表示
//     - 1/2/3 で Sky preset (Day / Sunset / Night) 切替 → cubemap 再生成
//     - シーンは HDR R16G16B16A16_Float RT に描画 → Bloom + ACES tonemap で LDR 出力
//
// 注: -DACS_RENDER_DILIGENT=ON 必須 (per-slice RT / cubemap / R11G11B10F / HDR が Diligent 専用)。
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "render/Ibl.h"
#include "render/Sky.h"
#include "render/Atmosphere.h"
#include "render/ShadowMap.h"
#include "render/Ssr.h"
#include "render/Ssao.h"
#include "render/Ssgi.h"
#include "render/MotionVector.h"
#include "math/Mat.h"
#include "render/PbrShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/PostProcess.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "container/Array.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {
// Halton(i, b) ∈ [0, 1): low-discrepancy 1D sequence used as TAA sub-pixel jitter.
// 2 つの異なる base (典型的に 2, 3) で xy ペアを作る (Halton(2,3) sequence)。
constexpr f32 Halton(u32 i, u32 b) noexcept {
    f32 f = 1.0f, r = 0.0f;
    while (i > 0) {
        f /= static_cast<f32>(b);
        r += f * static_cast<f32>(i % b);
        i /= b;
    }
    return r;
}
} // namespace

class HelloIbl : public Application {
public:
    void OnStart() noexcept override {
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

        // ShadowMap (Phase 34b): 2048 pixels
        ACS_SAMPLE_INIT(_shadow.Init(*dev, 2048));
        // SSR (Phase 34e): HDR と同フォーマット / 同サイズで scratch を確保
        ACS_SAMPLE_INIT(_ssr.Init(*dev, _post.HdrFormat(), sw, sh));
        // SSAO (Phase 34j): depth から visibility を計算、frame size と同じ解像度
        ACS_SAMPLE_INIT(_ssao.Init(*dev, sw, sh));
        // SSGI (Phase 33c): scene_color + depth → 1 bounce indirect light
        ACS_SAMPLE_INIT(_ssgi.Init(*dev, sw, sh));
        // Motion vector (Phase 34f-3): 動的 mesh の screen-space motion を焼く
        ACS_SAMPLE_INIT(_motion.Init(*dev, sw, sh));

        // Lightmap (Phase 33f): 球グリッドの直下にある床用に静的 AO を CPU 焼き。
        // 球 5x5 を Sphere-Ray analytical hit で覆い、各 plane texel で hemisphere
        // sampling して visibility を求める。Phase 33f-2 で CPU baker を別 module
        // に切り出す予定だが、今は inline で済ませる (sample 内 demo 用)。
        ACS_SAMPLE_INIT(BakeAndUploadLightmap(*dev));

        // SpriteBatch は LDR backbuffer (tonemap 後)
        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
        _cam_pos = Vec3{0, 1.0f, -5.0f};

        // Bloom 強度はデフォルトより少し弱めに (Day sky は十分明るいので)
        _post_params.bloom_threshold = 1.5f;
        _post_params.bloom_intensity = 0.4f;
        // Phase 34k: 手動モード用の初期露出。preset Day = 0.7、adapted も同値に。
        _exposure_target   = 0.7f;
        _adapted_exposure  = 0.7f;
        // Phase 34k-2: 既定で GPU auto-exposure を有効化。シーン輝度を GPU で実測
        // して露出を自動算出 → preset 切替 (Day↔Night) でも自動で再露出され、
        // eye adaptation が演出として効く。'U' で手動 (Phase 34k) モードへ切替可。
        _use_auto_exposure = true;
        _post_params.auto_exposure_enabled = true;
        _post_params.auto_exposure_key     = _auto_key;
        _post_params.auto_exposure_speed   = 1.5f;   // τ≈0.7s のシネマ的順応速度
        _post_params.exposure              = 1.0f;   // 露出は GPU 側が決める

        // Color grading (Phase 34h) cinematic look: 軽い暖色 + 彩度ブースト + コントラスト
        _post_params.cg_saturation   = 1.10f;
        _post_params.cg_contrast     = 1.08f;
        _post_params.cg_temperature  = 0.08f;     // 暖色寄り
        _post_params.cg_tint         = -0.02f;    // 軽く緑寄り (映画 teal-orange)
        _post_params.cg_lift         = Vec3{0.005f, 0.0f, 0.01f};      // 影に微かな青冷感
        _post_params.cg_gain         = Vec3{1.04f, 1.02f, 0.98f};      // ハイライトを暖色に

        // CAS sharpening (Phase 34i): subtle clarity boost、UE5 デフォルト相当 0.4
        _post_params.cas_strength    = 0.4f;
        // SSR は PbrShader 側で合成する (Phase 34e-2fix) ので tonemap 側の SSR は
        // 無効化。intensity 0 にしないと fallback bloom mip が誤加算される。
        _post_params.ssr_intensity   = 0.0f;

        // 動的球 (Phase 34f-3) の初期 transform。frame 0 で prev==curr になるよう
        // curr と同値で prev も初期化する。
        for (u32 i = 0; i < kDynCount; ++i) {
            _dyn_curr[i] = ComputeDynTransform(i, 0.0f);
            _dyn_prev[i] = _dyn_curr[i];
        }
    }

    // 動的球 i の時刻 t における transform。中心 (0, 3, 1) のまわりを XY 平面で
    // 公転させる (画面内を大きく掃くので、motion vector 無しだと TAA で trail が出る)。
    Mat4 ComputeDynTransform(u32 i, f32 t) const noexcept {
        const f32 a = t * 1.8f + static_cast<f32>(i) * (2.0f * kPi / 3.0f);
        const f32 r = 2.2f;
        return Mat4::Translation(Vec3{ Cos(a) * r, 3.0f + Sin(a) * r, 1.0f });
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();

        // 1/2/3/4 で env 切替。SH9 mode が有効中は SH 9 係数も再計算が必要。
        // Phase 34k: preset ごとに露出目標を変える → _adapted_exposure がじわっと
        // 追従して eye adaptation (目が明暗に慣れる) 演出になる。
        if (Input::IsKeyPressed(Key::Num1)) {
            _sky.PresetDay();    _current_preset = 0;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
            _exposure_target = 0.7f;     // Day: 明るいので露出を絞る
        }
        if (Input::IsKeyPressed(Key::Num2)) {
            _sky.PresetSunset(); _current_preset = 1;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
            _exposure_target = 1.0f;
        }
        if (Input::IsKeyPressed(Key::Num3)) {
            _sky.PresetNight();  _current_preset = 2;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
            _exposure_target = 1.8f;     // Night: 暗いので露出を上げる
        }
        if (Input::IsKeyPressed(Key::Num4)) {
            _current_preset = 3;
            _need_studio_hdr = true; _need_sh9_rebuild = _use_sh9;
            _exposure_target = 1.0f;
        }
        if (Input::IsKeyPressed(Key::Num5)) {
            _current_preset = 4;
            _need_atmosphere = true; _need_sh9_rebuild = _use_sh9;
            _exposure_target = 0.85f;
        }
        if (Input::IsKeyPressed(Key::I)) {
            _display_mode = (_display_mode + 1) % 7;
        }
        // SH9 toggle: 現在の irradiance cubemap から計算した SH 9 で diffuse を再構築
        if (Input::IsKeyPressed(Key::S)) {
            _use_sh9 = !_use_sh9;
            _need_sh9_rebuild = _use_sh9;     // 必要なときに再計算
        }
        if (Input::IsKeyPressed(Key::C)) _use_clearcoat = !_use_clearcoat;
        if (Input::IsKeyPressed(Key::Z)) _use_anisotropy = !_use_anisotropy;
        if (Input::IsKeyPressed(Key::L)) _use_area_light = !_use_area_light;
        if (Input::IsKeyPressed(Key::G)) _use_probe_grid = !_use_probe_grid;
        if (Input::IsKeyPressed(Key::F)) _use_fog = !_use_fog;
        if (Input::IsKeyPressed(Key::H)) _use_shadows = !_use_shadows;
        if (Input::IsKeyPressed(Key::R)) _show_ssr = !_show_ssr;
        if (Input::IsKeyPressed(Key::O)) _use_ssao = !_use_ssao;
        if (Input::IsKeyPressed(Key::T)) _use_taa  = !_use_taa;
        if (Input::IsKeyPressed(Key::J)) _use_ssgi = !_use_ssgi;
        if (Input::IsKeyPressed(Key::K)) _use_lightmap = !_use_lightmap;
        // M で motion vector (動的 mesh の TAA reprojection) を toggle
        if (Input::IsKeyPressed(Key::M)) _use_motion_vec = !_use_motion_vec;
        // film grain アニメ用に時間累積 + 動的球の公転時刻
        _post_params.grain_time += dt;
        _anim_time += dt;
        // B キーで bloom on/off (verify HDR clip 防止効果)
        if (Input::IsKeyPressed(Key::B)) {
            _post_params.bloom_enabled = !_post_params.bloom_enabled;
        }
        // U で露出モード切替: GPU auto-exposure (実測) ⇔ 手動 (Phase 34k CPU 補間)
        if (Input::IsKeyPressed(Key::U)) _use_auto_exposure = !_use_auto_exposure;

        if (_use_auto_exposure) {
            // Phase 34k-2: 露出は GPU が実測輝度から算出。Q/E は目標平均輝度 (key) を
            // 動かして全体の明暗を補正する (EV compensation 相当)。
            if (Input::IsKeyDown(Key::E)) _auto_key += dt * 0.3f;
            if (Input::IsKeyDown(Key::Q)) _auto_key -= dt * 0.3f;
            if (_auto_key < 0.1f) _auto_key = 0.1f;
            if (_auto_key > 2.0f) _auto_key = 2.0f;
            _post_params.auto_exposure_enabled = true;
            _post_params.auto_exposure_key     = _auto_key;
            _post_params.exposure              = 1.0f;   // 露出は GPU 側で適用済み
        } else {
            // Phase 34k: 手動の露出目標 + CPU eye adaptation。Q/E で目標を動かし、
            // _adapted_exposure が dt 補間で追従する (~0.4 秒で順応)。
            if (Input::IsKeyDown(Key::E)) _exposure_target += dt * 0.5f;
            if (Input::IsKeyDown(Key::Q)) _exposure_target -= dt * 0.5f;
            if (_exposure_target < 0.1f) _exposure_target = 0.1f;
            if (_exposure_target > 4.0f) _exposure_target = 4.0f;
            f32 k = dt * 2.5f;
            if (k > 1.0f) k = 1.0f;
            _adapted_exposure += (_exposure_target - _adapted_exposure) * k;
            _post_params.auto_exposure_enabled = false;
            _post_params.exposure              = _adapted_exposure;
        }
        // auto-exposure の eye adaptation 補間に使うフレーム時間を渡す
        _post_params.delta_time = dt;

        // 視点を矢印 (回転) + WASD (移動) で操作
        const f32 mv = 4.0f * dt, tr = 1.5f * dt;
        if (Input::IsKeyDown(Key::Left))  _cam_yaw -= tr;
        if (Input::IsKeyDown(Key::Right)) _cam_yaw += tr;
        if (Input::IsKeyDown(Key::Up))    _cam_pitch -= tr * 0.8f;
        if (Input::IsKeyDown(Key::Down))  _cam_pitch += tr * 0.8f;
        const f32 limit = 0.45f * kPi;
        if (_cam_pitch >  limit) _cam_pitch =  limit;
        if (_cam_pitch < -limit) _cam_pitch = -limit;

        Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                     -Sin(_cam_pitch),
                      Cos(_cam_yaw) * Cos(_cam_pitch) };
        Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
        if (Input::IsKeyDown(Key::W)) _cam_pos += forward * mv;
        if (Input::IsKeyDown(Key::S)) _cam_pos -= forward * mv;
        if (Input::IsKeyDown(Key::D)) _cam_pos += right   * mv;
        if (Input::IsKeyDown(Key::A)) _cam_pos -= right   * mv;
        _camera.SetLookAt(_cam_pos, _cam_pos + forward);
    }

    // OnCustomFrame() で HDR 経路に切替えてデフォルトフローを置き換える。
    // 1) IBL build (必要なら、RT を一時的に切替)
    // 2) HDR RT にシーン (skybox + sphere grid) を描画
    // 3) PostProcess.Render で Bloom + Tonemap → LDR backbuffer
    // 4) SpriteBatch HUD を LDR backbuffer に
    bool OnCustomFrame() noexcept override {
        IRhiDevice*      dev  = GetRenderer().Device();
        IRhiCommandList* cl   = GetRenderer().CommandList();
        IRhiSwapchain*   sc   = GetRenderer().Swapchain();
        IRhiTexture*     hdr  = _post.HdrRenderTarget();
        IRhiTexture*     depth = GetRenderer().DepthBuffer();
        if (!dev || !cl || !sc || !hdr) return false;

        // 動的球 (Phase 34f-3) の transform を更新: prev ← 前フレームの curr。
        // color pass と motion pass の両方がこの curr / prev を参照する。
        for (u32 i = 0; i < kDynCount; ++i) {
            _dyn_prev[i] = _dyn_curr[i];
            _dyn_curr[i] = ComputeDynTransform(i, _anim_time);
        }

        // TAA Halton(2,3) sub-pixel jitter (Phase 34f): 8 フレーム周期で
        // sub-pixel ぶん xy をずらした projection を rendering 全体 (skybox,
        // PbrShader, SSR, SSAO) に渡す。これにより複数フレームの累積でエッジが
        // 滑らかになる。jitter mat = identity with M[3][0..1] = jx_ndc / jy_ndc。
        // 注意: shadow pass はライト視点なので jitter しない (caster と影 map が
        // 同じビューなら遮蔽判定はそのまま正しい)。
        const Mat4 vp_no_jitter = _camera.ViewProjection();
        Mat4 vp_for_render = vp_no_jitter;
        if (_use_taa) {
            const u32 idx = (_taa_frame_index % 8) + 1;  // +1 で Halton(0)=0 を避ける
            const f32 jx = Halton(idx, 2) - 0.5f;        // [-0.5, 0.5) sub-pixel
            const f32 jy = Halton(idx, 3) - 0.5f;
            const f32 jx_ndc = jx * 2.0f / static_cast<f32>(hdr->Width());
            const f32 jy_ndc = jy * 2.0f / static_cast<f32>(hdr->Height());
            Mat4 jmat = Mat4::Identity();
            jmat.m[3][0] = jx_ndc;
            jmat.m[3][1] = jy_ndc;
            vp_for_render = vp_no_jitter * jmat;
            _taa_frame_index = (_taa_frame_index + 1u) % 1024u;
        }

        const u32 buf_idx = sc->AcquireNextImage();
        cl->Begin();

        // Sky preset が変わった場合、env / irradiance / prefilter を作り直す
        if (_need_recapture) {
            dev->WaitIdle();
            _ibl.ResetEnvCubemap();
            _need_recapture = false;
        }
        // Studio HDR preset: equirect float texture を CPU で合成 → LoadEquirectHdr で
        // env cubemap として焼く。
        if (_need_studio_hdr) {
            dev->WaitIdle();
            BuildStudioHdrEquirect();
            auto r = _ibl.LoadEquirectHdrFromMemory(
                *dev, *cl,
                _equirect_rgba.Data(),
                kEquirectWidth, kEquirectHeight);
            if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: LoadEquirectHdr failed");
            _need_studio_hdr = false;
        }
        // Atmosphere preset (Phase 34c): Hillaire 風物理大気を CPU で焼く
        if (_need_atmosphere) {
            dev->WaitIdle();
            AtmosphereParams ap;
            ap.sun_dir       = Vec3{0.3f, 0.5f, 0.5f};
            ap.sun_intensity = Vec3{22.0f, 22.0f, 22.0f};
            ap.ray_steps     = 32;
            ap.sun_steps     = 8;
            auto baked = Atmosphere::BakeEquirect(kEquirectWidth, kEquirectHeight, ap);
            _equirect_rgba = Move(baked);
            auto r = _ibl.LoadEquirectHdrFromMemory(
                *dev, *cl,
                _equirect_rgba.Data(),
                kEquirectWidth, kEquirectHeight);
            if (r.IsErr()) ACS_LOG_ERROR("HelloIbl: Atmosphere bake failed");
            _need_atmosphere = false;
        }

        // ===== IBL build (まだ作ってないものだけ。一度作れば cache される) =====
        // BeginRenderToTexture(hdr) の前にやる: IBL の RT 切替は _main_swapchain を
        // 触らないので、HDR pass に影響しない。
        if (!_ibl.HasBrdfLut())       _ibl.EnsureBrdfLut(*dev, *cl);
        if (!_ibl.HasEnvCubemap())    _ibl.EnsureEnvCubemap(*dev, *cl, _sky);
        if (!_ibl.HasIrradianceMap()) _ibl.EnsureIrradiance(*dev, *cl);
        if (!_ibl.HasPrefilterMap())  _ibl.EnsurePrefilter(*dev, *cl);

        // ===== Shadow pass (Phase 34b、'H' で有効) =====
        // 球グリッドの中心 (0, 2, 3) 周辺を 8.5 半径でカバーする orthographic 影行列。
        // 太陽方角は現在の sky preset に従う (Studio HDR は前方上向き)。
        Vec3 sun_dir;
        if (_current_preset == 3) sun_dir = Vec3{0.3f, 0.6f, -0.5f};
        else                       sun_dir = _sky.SunDirection();
        if (_use_shadows) {
            _shadow.SetDirectionalLight(sun_dir, Vec3{0, 1.5f, 3.0f}, 9.0f);
            cl->BeginShadowPass(*_shadow.DepthTexture(), 1.0f);
            cl->SetPipeline(*_shadow.CasterPipeline());
            cl->SetConstantBuffer(0, *_shadow.LightCB());
            // sphere casters
            constexpr u32 kGridCast = 5;
            constexpr f32 kSpacingCast = 1.4f;
            cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
            cl->SetIndexBuffer(*_gm_sphere.index_buffer);
            for (u32 y = 0; y < kGridCast; ++y) {
                for (u32 x = 0; x < kGridCast; ++x) {
                    const f32 px = (static_cast<f32>(x) - (kGridCast - 1) * 0.5f) * kSpacingCast;
                    const f32 py = (static_cast<f32>(y) - (kGridCast - 1) * 0.5f) * kSpacingCast + 2.5f;
                    _shadow.SetCaster(Mat4::Translation(Vec3{px, py, 3.0f}));
                    cl->SetConstantBuffer(1, *_shadow.CasterObjectCB());
                    cl->DrawIndexed(_gm_sphere.index_count);
                }
            }
            cl->EndShadowPass(*_shadow.DepthTexture());
        }

        // ===== 1) HDR RT にシーン描画 =====
        cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);

        Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                       vp.height = static_cast<f32>(hdr->Height());
        cl->SetViewport(vp);
        ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                           svr.bottom = static_cast<i32>(hdr->Height());
        cl->SetScissor(svr);

        // 背景 skybox
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

        // 5x5 sphere grid (IBL only)
        _pbr.SetIbl(_ibl.IrradianceMap(), _ibl.PrefilterMap(), _ibl.BrdfLut(),
                    _ibl.PrefilterMips());

        // SH9 mode: 現在の env cubemap (sky or studio HDR) から SH 9 を計算して PbrShader へ
        if (_need_sh9_rebuild) {
            BuildEquirectFromCurrentEnv();
            ImageBasedLighting::ComputeSh9FromEquirect(
                _equirect_rgba.Data(), kEquirectWidth, kEquirectHeight, _sh9);
            _need_sh9_rebuild = false;
        }
        _pbr.SetSh9(_use_sh9 ? _sh9 : nullptr);

        // 太陽の direct light を 1 灯追加 (Studio HDR では中央パネルを sun に見立てる)。
        // これで clear-coat / anisotropic の direct specular が見える。
        DirLight sun;
        if (_current_preset == 3) {
            sun.direction = Vec3{0, 0.4f, 1.0f};
            sun.color     = Vec3{0.7f, 0.7f, 0.7f};
        } else {
            sun.direction = _sky.SunDirection();
            sun.color     = _sky.SunColor() * 0.9f;
        }
        _pbr.SetLights(vp_for_render, _camera.Eye(),
                       &sun, 1, Vec3{0, 0, 0});
        _pbr.SetPointLights(nullptr, 0);

        // SSAO map (Phase 34j-2、PbrShader 側で indirect 項に乗算)。
        // 1-frame latency: PbrShader が読むのは前フレーム末で書かれた _ssao の中身。
        // 60 FPS なら 16ms 遅延でカメラ移動時の追従が遅れる程度、視覚的に許容。
        //
        // 注意 (cold-start hazard): _ssao.OutputTexture() を frame 0 で渡すと
        // Diligent は RT 初期化を保証してないので未定義値を読む可能性がある。
        // `_ssao_warm` フラグで「1 回でも _ssao.Render が走った」ことを保証してから
        // bind する。フレーム 0 は PbrShader が _ssao_fb (1x1 白 = visibility 1) に
        // フォールバックする。
        // 注: SSAO 無効時も viewport サイズは渡す。SSGI / SSR が screen UV を
        // ssao_params.zw から得るため、ここを 0 にすると参照が壊れる。
        if (_use_ssao && _ssao_warm) {
            _pbr.SetSsao(_ssao.OutputTexture(), /*intensity=*/1.0f,
                         hdr->Width(), hdr->Height());
        } else {
            _pbr.SetSsao(nullptr, 0.0f, hdr->Width(), hdr->Height());
        }

        // SSGI color (Phase 33c)。SSAO と同じ 1-frame latency パターン。
        // _ssgi_warm で frame 0 の garbage read を回避。intensity 0.6 が typical。
        if (_use_ssgi && _ssgi_warm) {
            _pbr.SetSsgi(_ssgi.OutputTexture(), /*intensity=*/0.6f);
        } else {
            _pbr.SetSsgi(nullptr, 0.0f);
        }

        // SSR (Phase 34e-2fix)。PbrShader 側で roughness 依存合成 (rough 面ほど
        // 反射が弱まる)。SSAO/SSGI と同じ 1-frame latency — SSR pass は color 描画後に
        // 走るので、ここで渡すのは前フレームの結果。_ssr_warm で frame 0 garbage 回避。
        if (_show_ssr && _ssr_warm) {
            _pbr.SetSsr(_ssr.OutputTexture(), /*intensity=*/1.0f);
        } else {
            _pbr.SetSsr(nullptr, 0.0f);
        }

        // Shadow map (Phase 34b)
        if (_use_shadows) {
            _pbr.SetShadowMap(_shadow.DepthTexture(), _shadow.LightViewProjection(),
                              0.002f, 1.0f / static_cast<f32>(_shadow.Size()));
        } else {
            _pbr.SetShadowMap(nullptr, Mat4{}, 0, 0);
        }
        // Volumetric fog (Phase 33e): 灰色 fog、密度 0.12 / 高さ減衰 0.2
        if (_use_fog) {
            _pbr.SetFog(Vec3{0.65f, 0.7f, 0.8f}, 0.12f, 0.2f, 0.0f);
        } else {
            _pbr.SetFog(Vec3{0, 0, 0}, 0.0f);
        }

        // 静的光プローブグリッド (Phase 33d): 2 つの probe を赤/青に手調整
        // 球グリッドの左右に配置 → 球は世界座標 X に応じて赤↔青 ambient が blend
        if (_use_probe_grid) {
            // 計算済 _sh9 (現 env の SH9) をベースに、赤と青に着色
            PbrShader::LightProbe p[2];
            // 左 probe (赤光)
            p[0].position = Vec3{-4.0f, 1.5f, 3.0f};
            for (u32 k = 0; k < 9; ++k) {
                p[0].sh9[k] = _sh9[k];
            }
            // l=0 (DC) に赤を強める
            p[0].sh9[0] = p[0].sh9[0] + Vec4{2.5f, 0.4f, 0.4f, 0};
            // 右 probe (青光)
            p[1].position = Vec3{+4.0f, 1.5f, 3.0f};
            for (u32 k = 0; k < 9; ++k) {
                p[1].sh9[k] = _sh9[k];
            }
            p[1].sh9[0] = p[1].sh9[0] + Vec4{0.4f, 0.4f, 2.5f, 0};
            _pbr.SetProbeGrid(p, 2);
            _need_sh9_rebuild = true;       // SH9 base が古ければ次フレームで再計算
        } else {
            _pbr.SetProbeGrid(nullptr, 0);
        }

        // 矩形 area light (Phase 33b): 球グリッドの前方上空に置いた 2x1 矩形パネル
        if (_use_area_light) {
            PbrShader::AreaLight rect;
            rect.center = Vec3{0.0f, 4.0f, 1.0f};      // 上空、camera 側
            rect.axis_x = Vec3{1.0f, 0.0f, 0.0f};      // 横半幅 = 1
            rect.axis_y = Vec3{0.0f, 0.0f, 0.5f};      // 奥行半高 = 0.5
            rect.color  = Vec3{5.0f, 4.5f, 3.5f};      // 暖色 HDR
            _pbr.SetAreaLights(&rect, 1);
        } else {
            _pbr.SetAreaLights(nullptr, 0);
        }

        // 地面プレーン (Phase 34b shadow caster ターゲット)。中性灰。
        // Phase 34e-2fix: roughness を 0.85→0.35 に下げ glossy な床に。roughness 依存
        // SSR が「磨かれた床に球が映り込む」絵を物理的に正しく出す (rough すぎると
        // 反射が弱く・SSR の意味が薄い)。影は diffuse 項に出るので glossy でも残る。
        // clearcoat / anisotropy は無効に上書き。
        // Phase 33f: 床のみ lightmap を bind (sphere grid 球には焼いてないので OFF)。
        _pbr.SetExtParams(0.0f, 0.08f, 0.0f, Vec3{1, 0, 0});
        // 床・グリッド球は非発光。動的オーブ (後段) が emissive を立てるので、
        // ここで 0 に戻しておく (member は次の SetObject まで持続するため)。
        _pbr.SetEmissive(Vec3{0, 0, 0}, 0.0f);
        if (_use_lightmap && _lightmap) {
            _pbr.SetLightmap(_lightmap.Get(), /*intensity=*/0.8f);
        } else {
            _pbr.SetLightmap(nullptr, 0.0f);
        }
        _pbr.DrawMesh(*cl, _gm_plane,
                      Mat4::Translation(Vec3{0, -0.6f, 3.0f}),
                      Vec3{0.5f, 0.5f, 0.55f}, 0.0f, /*roughness=*/0.35f, 1.0f);
        // 球グリッド draw 前に lightmap を解除 (球には焼いてないので付けない)
        _pbr.SetLightmap(nullptr, 0.0f);

        constexpr u32 kGrid = 5;
        constexpr f32 kSpacing = 1.4f;
        const Vec3 base_color{0.95f, 0.4f, 0.3f};
        // material 拡張 (clear-coat / anisotropic) を有効化
        const f32  cc   = _use_clearcoat  ? 1.0f : 0.0f;
        const f32  ccr  = 0.08f;                      // 鏡面に近い clear-coat
        const f32  aniso = _use_anisotropy ? 0.8f : 0.0f;
        const Vec3 tan_dir{1, 0, 0};                  // 横方向の brushed pattern
        _pbr.SetExtParams(cc, ccr, aniso, tan_dir);
        for (u32 y = 0; y < kGrid; ++y) {
            for (u32 x = 0; x < kGrid; ++x) {
                const f32 metallic  = static_cast<f32>(x) / (kGrid - 1);
                const f32 roughness = 0.05f + static_cast<f32>(y) / (kGrid - 1) * 0.95f;
                const f32 px = (static_cast<f32>(x) - (kGrid - 1) * 0.5f) * kSpacing;
                const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 2.5f;
                _pbr.DrawMesh(*cl, _gm_sphere,
                              Mat4::Translation(Vec3{px, py, 3.0f}),
                              base_color, metallic, roughness, 1.0f);
            }
        }

        // 動的球 (Phase 34f-3 + 34l): グリッド前方を公転する発光オーブ 3 個。
        // emissive + bloom で光り、motion vector で TAA の trail も出ない。
        // glossy な床にも映り込む (SSR が scene color = 発光込みを反射するため)。
        const Vec3 kOrbGlow[3] = {
            Vec3{1.00f, 0.45f, 0.15f},   // 暖色オレンジ
            Vec3{0.25f, 1.00f, 0.40f},   // 緑
            Vec3{0.30f, 0.55f, 1.00f},   // 青
        };
        _pbr.SetExtParams(0.0f, 0.5f, 0.0f, Vec3{1, 0, 0});
        for (u32 i = 0; i < kDynCount; ++i) {
            _pbr.SetEmissive(kOrbGlow[i], /*strength=*/3.0f);
            _pbr.DrawMesh(*cl, _gm_sphere, _dyn_curr[i],
                          kOrbGlow[i], 0.0f, 0.35f, 1.0f);
        }
        _pbr.SetEmissive(Vec3{0, 0, 0}, 0.0f);   // 後続描画のため reset

        cl->EndRenderToTexture(*hdr);

        // ===== Geometry G-buffer pass (motion + normal、Phase 34f-3 / 34m) =====
        // 全 mesh を再ラスタライズし MRT 2 枚 (screen-space motion vector +
        // world-space normal) を焼く。motion は TAA / temporal SSGI が動く mesh を
        // reproject するのに、normal は SSR が faceted な反射ベクトル (旧 ddx/ddy
        // 由来) を避けるのに使う。SSGI が現フレームの motion を読むため SSGI より
        // 前に実行する。静的 mesh は prev == curr。
        // motion + normal は SSR/SSGI/SSAO/TAA 全てが使うため geometry パスは常時
        // 実行する。'M' は motion の TAA/SSGI 消費を toggle するだけで、パスは止めない。
        {
            // frame 0 は前フレーム VP が未確定なので prev=curr で motion 0 にする。
            const Mat4 motion_prev_vp = _taa_prev_vp_valid ? _prev_vp_no_jitter
                                                           : vp_no_jitter;
            _motion.Begin(*cl, vp_no_jitter, motion_prev_vp);
            // 床 (静的: prev == curr)
            const Mat4 plane_model = Mat4::Translation(Vec3{0, -0.6f, 3.0f});
            _motion.DrawMesh(*cl, _gm_plane, plane_model, plane_model);
            // 静的グリッド球 25 (color pass と同じ transform 計算、prev == curr)
            for (u32 y = 0; y < kGrid; ++y) {
                for (u32 x = 0; x < kGrid; ++x) {
                    const f32 px = (static_cast<f32>(x) - (kGrid - 1) * 0.5f) * kSpacing;
                    const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 2.5f;
                    const Mat4 m = Mat4::Translation(Vec3{px, py, 3.0f});
                    _motion.DrawMesh(*cl, _gm_sphere, m, m);
                }
            }
            // 動的球 (prev != curr → object motion を含む)
            for (u32 i = 0; i < kDynCount; ++i) {
                _motion.DrawMesh(*cl, _gm_sphere, _dyn_curr[i], _dyn_prev[i]);
            }
            _motion.End(*cl);
        }
        // motion texture を TAA / SSGI へ渡す。frame 0 は prev VP 未確定なので渡さず、
        // depth reprojection 側の cold-start ガードに委ねる。
        IRhiTexture* motion_tex =
            (_use_motion_vec && _taa_prev_vp_valid) ? _motion.OutputTexture() : nullptr;
        _post_params.taa_motion_texture = motion_tex;

        // ===== SSR pass (Phase 34e、'R' で toggle) =====
        // SSR を計算して _ssr.OutputTexture() に書く。最終合成は PbrShader 側
        // (Phase 34e-2fix、roughness 依存で env prefilter と blend) なので、ここでは
        // 次フレームの PbrShader 用に焼くだけ (1-frame latency)。
        // intensity は 1.0 固定 — 反射強度は PbrShader::SetSsr 側で一元管理する。
        const Mat4 inv_vp = Inverse(vp_for_render);
        if (_show_ssr) {
            // temporal SSR (Phase 34e-3) の reproject 用に前フレームの jitter なし
            // VP を渡す。frame 0 は未確定なので現 VP を渡し reprojection を無効化。
            const Mat4& ssr_prev_vp = _taa_prev_vp_valid ? _prev_vp_no_jitter
                                                         : vp_no_jitter;
            _ssr.Render(*dev, *cl, *hdr, *depth,
                        *_motion.OutputNormalTexture(),
                        vp_for_render,
                        inv_vp,
                        ssr_prev_vp,
                        _camera.Eye(),
                        /*intensity=*/1.0f,
                        motion_tex);
            _ssr_warm = true;     // 次フレームから PbrShader が SSR texture を読める
        }

        // ===== SSAO pass (Phase 34j、'O' で toggle) =====
        // depth から visibility を計算して _ssao.OutputTexture() に書く。
        // 注意: この SSAO pass は HDR scene 描画よりあと (depth が完成済の状態)
        // に走らせる必要があるので、 HDR の EndRenderToTexture と PostProcess
        // の前の間に置く。今回 (Phase 34j-2) はこの後の PbrShader でも sample
        // 対象とするが、PbrShader は次のフレームで適用される (1 frame latency)
        // ことに注意。実用上 60 FPS なら 16ms 遅延で問題なし。
        if (_use_ssao) {
            // Phase 34j-6: GTAO は view 空間で計算するので view 行列も渡す。
            // GTAO の analytical 積分は適度な遮蔽量に収まるので intensity 1.0。
            _ssao.Render(*dev, *cl, *depth,
                         *_motion.OutputNormalTexture(),
                         vp_for_render, inv_vp, _camera.View(),
                         _camera.Eye(), sun.direction,
                         /*intensity=*/1.0f,
                         /*radius=*/0.5f);
            _ssao_warm = true;     // 次フレームから PbrShader が SSAO texture を読める
        }

        // ===== SSGI pass (Phase 33c、'J' で toggle) =====
        // scene HDR + depth から indirect bounce を screen-space で推定。
        // 1-frame latency (SSAO と同じ pattern)。
        if (_use_ssgi) {
            // Phase 33c-3: temporal accumulation 用に前フレームの jitter なし VP を
            // 渡す (TAA と共用)。frame 0 は _prev_vp_no_jitter が default identity
            // なので、現フレームの VP を prev として渡し reprojection を motion 0 に
            // しておく (TAA と同じ cold-start ガード)。
            // Phase 34f-3: motion_tex を渡すと temporal pass が動く mesh も正しく
            // reproject する (null なら従来の depth reprojection にフォールバック)。
            const Mat4& ssgi_prev_vp = _taa_prev_vp_valid ? _prev_vp_no_jitter
                                                          : vp_no_jitter;
            _ssgi.Render(*dev, *cl, *hdr, *depth,
                         *_motion.OutputNormalTexture(),
                         vp_for_render, inv_vp, ssgi_prev_vp,
                         _camera.Eye(),
                         /*intensity=*/1.0f,
                         /*max_distance=*/5.0f,
                         motion_tex);
            _ssgi_warm = true;
        }

        // TAA (Phase 34f) を毎フレーム params に反映
        _post_params.taa_enabled = _use_taa;
        _post_params.taa_blend_factor = 0.1f;     // current 10% + history 90%
        // Phase 34f-2: camera motion 由来の history reprojection。
        // 注意 (frame 0 garbage 回避): まだ _prev_vp_no_jitter が default (identity) のときに
        // reproject すると world 座標を clip 座標と誤解して prev_ndc が破綻する。
        // _taa_prev_vp_valid フラグで「前フレーム VP を本物で書いた」状態を保証してから
        // depth_texture を渡す。最初の 1 フレームは depth=null で reproject 無効化。
        _post_params.taa_depth_texture            = (_use_taa && _taa_prev_vp_valid) ? depth : nullptr;
        _post_params.taa_view_proj_no_jitter      = vp_no_jitter;
        _post_params.taa_prev_view_proj_no_jitter = _prev_vp_no_jitter;

        // ===== 2) Bloom + ACES Tonemap → LDR backbuffer (SSR も additive mix) =====
        _post.Render(*cl, *sc, buf_idx, _post_params);

        // 次フレーム用に保存 (jitter なしの true VP)
        _prev_vp_no_jitter = vp_no_jitter;
        _taa_prev_vp_valid = true;

        // ===== 3) SpriteBatch HUD (LDR backbuffer) =====
        IRhiTexture* lut = _ibl.BrdfLut();
        const u32 sw = sc->Width();
        const u32 sh = sc->Height();

        _batch.Begin(*cl, sw, sh);
        if (lut) {
            _batch.DrawRect(static_cast<f32>(sw) - 280, 20,
                            260, 320, Vec4{0, 0, 0, 0.55f});
            _batch.Draw(*lut,
                        static_cast<f32>(sw) - 270, 60,
                        240, 240);
        }
        // SSR overlay debug (Phase 34e、左下に sub-window)
        if (_show_ssr && _ssr.OutputTexture()) {
            _batch.DrawRect(20, static_cast<f32>(sh) - 280,
                            420, 260, Vec4{0, 0, 0, 0.6f});
            _batch.Draw(*_ssr.OutputTexture(), 30, static_cast<f32>(sh) - 270,
                        400, 240);
            if (_font.AtlasTexture()) {
                _batch.DrawString(_font, "SSR debug overlay",
                                  30, static_cast<f32>(sh) - 268, Vec4{1, 1, 1, 1});
            }
        }
        // SSAO overlay debug (Phase 34j、右下に sub-window)
        if (_use_ssao && _ssao.OutputTexture()) {
            const f32 ax = static_cast<f32>(sw) - 440;
            const f32 ay = static_cast<f32>(sh) - 280;
            _batch.DrawRect(ax, ay, 420, 260, Vec4{0, 0, 0, 0.6f});
            _batch.Draw(*_ssao.OutputTexture(), ax + 10, ay + 10,
                        400, 240);
            if (_font.AtlasTexture()) {
                _batch.DrawString(_font, "SSAO debug (R=AO  G=contact shadow)",
                                  ax + 10, ay + 12, Vec4{1, 1, 1, 1});
            }
        }
        if (_font.AtlasTexture()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Phase 31/32a IBL+HDR  FPS: %.1f", static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});

            const char* preset =
                (_current_preset == 0) ? "Day" :
                (_current_preset == 1) ? "Sunset" :
                (_current_preset == 2) ? "Night" :
                (_current_preset == 3) ? "Studio HDR" : "Hillaire Atmosphere";
            std::snprintf(buf, sizeof(buf),
                          "Env preset: [%s]   (1/2/3 sky / 4 Studio HDR / 5 Atmosphere)", preset);
            _batch.DrawString(_font, buf, 20, 44, Vec4{0.85f, 0.95f, 1.0f, 1});

            const char* view_label = nullptr;
            switch (_display_mode) {
                case 0: view_label = "Env cubemap";                            break;
                case 1: view_label = "Irradiance (Lambert 半球積分)";          break;
                case 2: view_label = "Prefilter mip 0 (roughness 0.00)";       break;
                case 3: view_label = "Prefilter mip 1 (roughness 0.25)";       break;
                case 4: view_label = "Prefilter mip 2 (roughness 0.50)";       break;
                case 5: view_label = "Prefilter mip 3 (roughness 0.75)";       break;
                case 6: view_label = "Prefilter mip 4 (roughness 1.00)";       break;
                default: view_label = "?";                                      break;
            }
            std::snprintf(buf, sizeof(buf),
                          "Display: %s   (I で切替)", view_label);
            _batch.DrawString(_font, buf, 20, 68, Vec4{1.0f, 0.95f, 0.7f, 1});

            char exp_label[48];
            if (_use_auto_exposure) {
                std::snprintf(exp_label, sizeof(exp_label), "AUTO (key %.2f)",
                              static_cast<double>(_auto_key));
            } else {
                std::snprintf(exp_label, sizeof(exp_label), "%.2f (manual)",
                              static_cast<double>(_post_params.exposure));
            }
            std::snprintf(buf, sizeof(buf),
                          "Exposure: %s   Bloom: %s   Diffuse: %s   (U auto, Q/E exp, B bloom)",
                          exp_label,
                          _post_params.bloom_enabled ? "ON" : "OFF",
                          _use_sh9 ? "SH9 (light probe)" : "Irradiance cube");
            _batch.DrawString(_font, buf, 20, 92, Vec4{0.9f, 0.9f, 0.9f, 1});

            std::snprintf(buf, sizeof(buf),
                          "CC=%s Aniso=%s Area=%s ProbeG=%s Fog=%s Shadow=%s SSAO=%s TAA=%s SSGI=%s LM=%s MV=%s",
                          _use_clearcoat ? "ON" : "OFF",
                          _use_anisotropy ? "ON" : "OFF",
                          _use_area_light ? "ON" : "OFF",
                          _use_probe_grid ? "ON" : "OFF",
                          _use_fog ? "ON" : "OFF",
                          _use_shadows ? "ON" : "OFF",
                          _use_ssao ? "ON" : "OFF",
                          _use_taa ? "ON" : "OFF",
                          _use_ssgi ? "ON" : "OFF",
                          _use_lightmap ? "ON" : "OFF",
                          _use_motion_vec ? "ON" : "OFF");
            _batch.DrawString(_font, buf, 20, 116, Vec4{0.9f, 0.9f, 0.9f, 1});
            _batch.DrawString(_font, "WASD: 移動   矢印: 視点回転   Esc: 終了",
                              20, 140, Vec4{0.7f, 0.85f, 1.0f, 1});
            if (lut) {
                _batch.DrawString(_font, "BRDF LUT",
                                  static_cast<f32>(sw) - 260, 36, Vec4{1, 1, 1, 1});
                _batch.DrawString(_font, "X:NdotV  Y:roughness",
                                  static_cast<f32>(sw) - 265, 308, Vec4{0.85f, 0.85f, 0.85f, 1});
            }
        }
        _batch.End();

        cl->EndRenderToSwapchain(*sc, buf_idx);
        cl->End();
        cl->Submit();
        sc->Present();
        return true;
    }

    // Phase 33f: 床用 lightmap を CPU 焼き。256x256 RGBA8、各 texel に
    // 「球グリッド上方からの sky 寄与 × visibility」を入れる。Sphere occlusion
    // は analytical ray-vs-sphere、sky color は固定 (Day preset)。簡易だが
    // 床にスフィア真下の影 + 開けた所の明るい indirect が出る。
    Result<void> BakeAndUploadLightmap(IRhiDevice& dev) noexcept {
        constexpr u32 kSize = 256;
        Array<u8> rgba; rgba.Resize(static_cast<usize>(kSize) * kSize * 4u);

        // 球グリッドのパラメータ (OnCustomFrame の draw 計算と一致)
        constexpr u32 kGrid = 5;
        constexpr f32 kSpacing = 1.4f;
        constexpr f32 kSphereR = 0.55f;
        constexpr f32 kCenterY = 2.5f;       // py = (gy - 2)*1.4 + 2.5
        constexpr f32 kCenterZ = 3.0f;
        constexpr f32 kPlaneY = -0.6f;
        constexpr f32 kPlaneSize = 40.0f;    // MakePlane(40,40)
        const Vec3 kSkyColor{0.85f, 0.92f, 1.0f};   // 弱い暖青 (Day sky horizon 近似)

        for (u32 y = 0; y < kSize; ++y) {
            for (u32 x = 0; x < kSize; ++x) {
                const f32 u = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kSize);
                const f32 v = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kSize);
                // Plane UV は [0,1] が plane center (40x40) を覆う想定。
                // MakePlane の uv 規約に合わせ、中心 (0, kPlaneY, kCenterZ) からの XZ。
                const f32 wx = (u - 0.5f) * kPlaneSize;
                const f32 wz = (v - 0.5f) * kPlaneSize + kCenterZ;

                // 8 方向の hemisphere ray を analytical でテスト (上半球、jitter なし)
                constexpr u32 kRays = 8;
                u32 hit_count = 0;
                for (u32 r = 0; r < kRays; ++r) {
                    // hemisphere direction (上半球の固定方向)
                    const f32 ang = static_cast<f32>(r) * (2.0f * kPi / static_cast<f32>(kRays));
                    const f32 spread = 0.7f;        // sin(45°) 程度の広がり
                    const Vec3 rd{spread * Cos(ang), 1.0f - spread * 0.5f, spread * Sin(ang)};
                    // 球グリッド 25 個と Ray-Sphere intersection
                    bool hit_any = false;
                    for (u32 gy = 0; gy < kGrid && !hit_any; ++gy) {
                        for (u32 gx = 0; gx < kGrid && !hit_any; ++gx) {
                            const f32 px = (static_cast<f32>(gx) - 2.0f) * kSpacing;
                            const f32 py = (static_cast<f32>(gy) - 2.0f) * kSpacing + kCenterY;
                            const Vec3 sp{px, py, kCenterZ};
                            const Vec3 oc{wx - sp.x, kPlaneY - sp.y, wz - sp.z};
                            const f32 b = oc.x*rd.x + oc.y*rd.y + oc.z*rd.z;
                            const f32 c = oc.x*oc.x + oc.y*oc.y + oc.z*oc.z - kSphereR*kSphereR;
                            const f32 disc = b*b - c;
                            if (disc > 0.0f) {
                                const f32 t = -b - Sqrt(disc);
                                if (t > 0.01f && t < 20.0f) hit_any = true;
                            }
                        }
                    }
                    if (hit_any) ++hit_count;
                }
                const f32 vis = 1.0f - static_cast<f32>(hit_count) / static_cast<f32>(kRays);
                // 結果は sky_color * visibility (RGB)。8-bit に量子化。
                const Vec3 lm{kSkyColor.x * vis, kSkyColor.y * vis, kSkyColor.z * vis};
                const usize idx = (static_cast<usize>(y) * kSize + x) * 4u;
                rgba[idx + 0] = static_cast<u8>(Saturate(lm.x) * 255.0f);
                rgba[idx + 1] = static_cast<u8>(Saturate(lm.y) * 255.0f);
                rgba[idx + 2] = static_cast<u8>(Saturate(lm.z) * 255.0f);
                rgba[idx + 3] = 255;
            }
        }

        TextureDesc td{};
        td.width = kSize; td.height = kSize;
        td.format = Format::R8G8B8A8_UNorm;
        td.initial_data = rgba.Data();
        td.initial_data_size = rgba.Size();
        auto r = CreateRhiTexture(dev, td);
        if (r.IsErr()) return Err<void>(r.Error());
        _lightmap = Move(r.Value());
        return Ok();
    }

    // 現在の Sky procedural を CPU 側で評価して equirect float bitmap に焼く。
    // ProcSky (Ibl.cpp の HLSL) と同じ式を C++ で実装。Studio HDR preset の場合は
    // _equirect_rgba にもう焼かれているので何もしない。
    void BuildEquirectFromCurrentEnv() noexcept {
        if (_equirect_rgba.Size() == 0) {
            _equirect_rgba.Resize(static_cast<usize>(kEquirectWidth) * kEquirectHeight * 4u);
        }
        if (_current_preset == 3) {
            // Studio HDR は BuildStudioHdrEquirect で既に焼いてある
            return;
        }
        auto safe_sqrt_n = [](Vec3 v) noexcept {
            f32 len2 = v.x*v.x + v.y*v.y + v.z*v.z;
            if (len2 < 1e-12f) return Vec3{0, 1, 0};
            f32 inv = 1.0f / Sqrt(len2);
            return Vec3{v.x * inv, v.y * inv, v.z * inv};
        };
        auto smoothstep = [](f32 a, f32 b, f32 x) noexcept {
            f32 t = (x - a) / (b - a);
            t = Saturate(t);
            return t * t * (3.0f - 2.0f * t);
        };

        const Vec3 sun_dir = safe_sqrt_n(_sky.SunDirection());
        const Vec3 sun_col = _sky.SunColor();
        const Vec3 zenith  = _sky.ZenithColor();
        const Vec3 horizon = _sky.HorizonColor();
        const Vec3 ground  = _sky.GroundColor();
        const f32  sun_r   = _sky.SunRadius();
        const f32  sun_g   = _sky.SunGlow();

        for (u32 y = 0; y < kEquirectHeight; ++y) {
            const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kEquirectHeight) * kPi;
            const f32 sinT = Sin(theta), cosT = Cos(theta);
            for (u32 x = 0; x < kEquirectWidth; ++x) {
                const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kEquirectWidth);
                const f32 phi = phi_norm * 2.0f * kPi - kPi;
                // equirect 規約: phi=0 が +Z、theta=0 が +Y
                const Vec3 dir{ sinT * Sin(phi), cosT, sinT * Cos(phi) };

                Vec3 sky;
                if (dir.y >= 0.0f) {
                    const f32 k = Pow(Saturate(dir.y), 0.6f);
                    sky = horizon * (1.0f - k) + zenith * k;
                } else {
                    const f32 k = Pow(Saturate(-dir.y), 0.6f);
                    sky = horizon * (1.0f - k) + ground * k;
                }
                const f32 c = Saturate(dir.x * sun_dir.x + dir.y * sun_dir.y + dir.z * sun_dir.z);
                const f32 ang = 1.0f - c;
                if (ang < sun_r) {
                    sky = sun_col;
                } else if (ang < sun_g) {
                    const f32 k = 1.0f - smoothstep(sun_r, sun_g, ang);
                    sky = sky * (1.0f - k) + sun_col * k;
                }
                const u32 idx = (y * kEquirectWidth + x) * 4u;
                _equirect_rgba[idx + 0] = sky.x;
                _equirect_rgba[idx + 1] = sky.y;
                _equirect_rgba[idx + 2] = sky.z;
                _equirect_rgba[idx + 3] = 1.0f;
            }
        }
    }

    // CPU で equirect (256x128 RGBA float) を合成: 4 つの色つき HDR パネル光源 +
    // 暗灰色背景。各パネルは異なる azimuth (phi) の同じ仰角 (theta = 60°、地平より少し上)
    // にあり、Studio 風の「4 灯セットアップ」を模擬する。metallic sphere に 4 色の
    // 明るい反射が現れるはず。
    void BuildStudioHdrEquirect() noexcept {
        if (_equirect_rgba.Size() == 0) {
            _equirect_rgba.Resize(static_cast<usize>(kEquirectWidth) * kEquirectHeight * 4u);
        }
        const f32 background[3] = {0.03f, 0.03f, 0.04f};
        struct Panel { f32 phi; f32 r, g, b; };
        const Panel panels[4] = {
            {0.0f,             8.0f, 2.0f, 1.5f},      // 前方 (赤橙)
            {kPi * 0.5f,       1.5f, 8.0f, 2.5f},      // 右   (黄緑)
            {kPi,              1.5f, 2.5f, 8.0f},      // 後方 (青)
            {kPi * 1.5f,       8.0f, 7.5f, 4.0f},      // 左   (白橙、暖色キー)
        };
        const f32 target_theta = kPi * 0.4f;      // 地平のやや上 (60°≒0.4π)
        const f32 panel_radius = 0.10f;           // panel が広がる角度 (≈18°)

        for (u32 y = 0; y < kEquirectHeight; ++y) {
            const f32 theta = (static_cast<f32>(y) + 0.5f) / static_cast<f32>(kEquirectHeight) * kPi;
            for (u32 x = 0; x < kEquirectWidth; ++x) {
                const f32 phi_norm = (static_cast<f32>(x) + 0.5f) / static_cast<f32>(kEquirectWidth);
                const f32 phi      = phi_norm * 2.0f * kPi - kPi;  // [-π, π]

                f32 r = background[0], g = background[1], b = background[2];
                for (u32 i = 0; i < 4; ++i) {
                    f32 dphi = phi - panels[i].phi;
                    while (dphi >  kPi) dphi -= 2.0f * kPi;
                    while (dphi < -kPi) dphi += 2.0f * kPi;
                    const f32 dtheta = theta - target_theta;
                    const f32 d2 = dphi * dphi + dtheta * dtheta;
                    if (d2 < panel_radius * panel_radius) {
                        // gaussian-ish falloff
                        const f32 k = 1.0f - d2 / (panel_radius * panel_radius);
                        r += panels[i].r * k;
                        g += panels[i].g * k;
                        b += panels[i].b * k;
                    }
                }
                const u32 idx = (y * kEquirectWidth + x) * 4u;
                _equirect_rgba[idx + 0] = r;
                _equirect_rgba[idx + 1] = g;
                _equirect_rgba[idx + 2] = b;
                _equirect_rgba[idx + 3] = 1.0f;
            }
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
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

private:
    static constexpr u32 kEquirectWidth  = 256;       // SH9 計算用、小さめで十分
    static constexpr u32 kEquirectHeight = 128;
    static constexpr u32 kDynCount       = 3;         // Phase 34f-3: 動的球の数

    PostProcess        _post;
    ImageBasedLighting _ibl;
    Sky                _sky;
    PbrShader          _pbr;
    GpuMesh            _gm_sphere;
    SpriteBatch        _batch;
    Font               _font;
    Camera             _camera;
    PostProcessParams  _post_params;
    Array<f32>         _equirect_rgba;          // 4 ch float
    Vec4               _sh9[9]      = {};        // 計算済 SH 9 係数 (xyz=RGB)
    Vec3               _cam_pos   = Vec3{0, 1.0f, -5.0f};
    f32                _cam_yaw   = 0.0f;
    f32                _cam_pitch = 0.0f;
    i32                _current_preset = 0;
    bool               _need_recapture   = false;
    bool               _need_studio_hdr  = false;
    bool               _use_sh9          = false;
    bool               _need_sh9_rebuild = false;
    bool               _use_clearcoat    = false;
    bool               _use_anisotropy   = false;
    bool               _use_area_light   = false;
    bool               _use_probe_grid   = false;
    bool               _use_fog          = false;
    bool               _need_atmosphere  = false;
    bool               _use_shadows      = false;
    bool               _show_ssr         = false;
    bool               _ssr_warm         = false; // Phase 34e-2fix: _ssr.Render が 1 度以上走った？
    bool               _use_ssao         = true;  // Phase 34j-2: PbrShader 側で composite (1-frame latency)
    bool               _ssao_warm        = false; // _ssao.Render が 1 度以上走った？ (frame 0 garbage 回避)
    bool               _use_taa          = true;  // Phase 34f: TAA 有効 ('T' でトグル)
    u32                _taa_frame_index  = 0;     // Halton(2,3) 用カウンタ
    Mat4               _prev_vp_no_jitter{};      // Phase 34f-2: 前フレームの jitter なし VP
    bool               _taa_prev_vp_valid = false;// 上が本物の VP (default identity 以外) か
    f32                _exposure_target  = 0.7f;  // Phase 34k: 露出目標 (preset / Q-E で動く)
    f32                _adapted_exposure = 0.7f;  // Phase 34k: 実露出 (target へ dt 補間)
    bool               _use_auto_exposure = true; // Phase 34k-2: GPU auto-exposure ('U' で手動切替)
    f32                _auto_key          = 0.5f; // Phase 34k-2: 自動露出の目標平均輝度 (Q/E で調整)
    bool               _use_ssgi         = true;  // Phase 33c: SSGI 有効 ('J' でトグル)
    bool               _ssgi_warm        = false; // _ssgi.Render が 1 度以上走った？
    bool               _use_lightmap     = true;  // Phase 33f: lightmap 有効 ('K' でトグル)
    UniquePtr<IRhiTexture> _lightmap;             // 床用 baked lightmap (256x256 RGBA8)
    ShadowMap          _shadow;
    Ssr                _ssr;
    Ssao               _ssao;
    Ssgi               _ssgi;
    MotionVector       _motion;                    // Phase 34f-3: 動的 mesh motion vector
    bool               _use_motion_vec   = true;   // Phase 34f-3: 'M' で toggle
    Mat4               _dyn_curr[kDynCount] = {};  // 動的球の現フレーム transform
    Mat4               _dyn_prev[kDynCount] = {};  // 動的球の前フレーム transform
    f32                _anim_time        = 0.0f;   // 動的球公転の時刻アキュムレータ
    GpuMesh            _gm_plane;
    u32                _display_mode     = 0;
};

ACS_DEFINE_MAIN(HelloIbl)
