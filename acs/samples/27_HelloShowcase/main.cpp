// SPDX-License-Identifier: Apache-2.0
// ACS の HelloShowcase サンプル (Phase 35-3d 直後の cinematic demo)
//
// 動作:
//   ・auto-orbit カメラで scene を 1 周しながら、4 種の sphere を見せる:
//       - polished gold      (metallic 1.0、roughness 0.15、金色)
//       - matte ceramic      (metallic 0.0、roughness 0.75、白)
//       - clear glass        (RefractionShader、IOR 1.5、roughness 0)
//       - frosted glass      (RefractionShader、IOR 1.5、roughness 0.5、薄青)
//   ・床は半光沢ダーク (PBR、metallic 0、roughness 0.45)。
//   ・小さな emissive オーブ 2 つが床上をゆっくり公転 (bloom highlight)。
//   ・IBL: Day sky preset → env / irradiance / specular prefilter / BRDF LUT。
//   ・Post: ACES tonemap + bloom + 中程度の vignette。
//
//   キー:
//     P  : auto-orbit を pause / resume
//     R  : SSR (env reflection mix) の toggle
//     X  : refraction glass の toggle
//     Esc: 終了
//
// 注: -DACS_RENDER_DILIGENT=ON でのみビルドされる (HDR / per-slice RT / 屈折 demo
// が Diligent backend 専用)。samples/CMakeLists.txt 側で `if(ACS_RENDER_DILIGENT)`
// にラップされる。
#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
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
#include "render/IRhiDevice.h"
#include "render/IRhiCommandList.h"
#include "render/IRhiTexture.h"
#include "render/RhiTypes.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

// 4 体の sphere の x 座標と素材
constexpr u32 kSphereCount = 4;
constexpr f32 kSphereX[kSphereCount] = {-2.4f, -0.8f, 0.8f, 2.4f};

enum class MaterialKind : u8 { Gold, Ceramic, ClearGlass, FrostedGlass };
constexpr MaterialKind kSphereKind[kSphereCount] = {
    MaterialKind::Gold,
    MaterialKind::Ceramic,
    MaterialKind::ClearGlass,
    MaterialKind::FrostedGlass,
};

constexpr f32 kSphereRadius = 0.55f;     // MakeSphere の既定半径
constexpr f32 kSphereScale  = 1.4f;      // 視認性のため 1.4 倍にスケール
constexpr f32 kSphereY      = 0.6f;      // 中心 y (床から少し浮かす)
constexpr f32 kSphereZ      = 0.0f;

constexpr f32 kFloorY = -0.2f;
constexpr f32 kFloorSize = 12.0f;

// emissive オーブの定数
constexpr u32 kOrbCount = 2;
constexpr f32 kOrbRadius      = 0.5f;     // sphere scale で小さく
constexpr f32 kOrbScale       = 0.35f;
constexpr f32 kOrbY           = 0.9f;
constexpr f32 kOrbOrbitRadius = 3.5f;     // 床平面で公転
const Vec3 kOrbColors[kOrbCount] = {
    Vec3{1.00f, 0.55f, 0.20f},     // orange
    Vec3{0.30f, 0.85f, 1.00f},     // cyan
};
constexpr f32 kOrbEmissiveStrength = 4.0f;

// Halton(i, b) ∈ [0, 1): TAA 用 low-discrepancy 1D sequence
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

class HelloShowcase : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }
        IRhiSwapchain* sc = GetRenderer().Swapchain();
        if (!sc) { Quit(); return; }

        const u32 sw = sc->Width();
        const u32 sh = sc->Height();

        // HDR + Bloom + ACES tonemap
        ACS_SAMPLE_INIT(_post.Init(*dev, sw, sh, GetRenderer().ColorFormat()));

        // Sky / PBR / mesh は全部 HDR format で初期化
        ACS_SAMPLE_INIT(_sky.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
        _sky.PresetDay();
        ACS_SAMPLE_INIT(_pbr.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));

        auto sphere = Primitive::MakeSphere(kSphereRadius, 48, 24);
        ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
        auto plane = Primitive::MakePlane(kFloorSize, kFloorSize);
        ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, _gm_floor));

        // SSR (env reflection composite via PbrShader、Phase 34e-2fix)
        ACS_SAMPLE_INIT(_ssr.Init(*dev, _post.HdrFormat(), sw, sh));
        // Hi-Z coarse min-depth (Phase 36-3a) — SSR の skip-ahead 用 1/8 解像度
        ACS_SAMPLE_INIT(_hiz.Init(*dev, sw, sh));
        // GTAO (Phase 34j-6) — indirect/ambient AO
        ACS_SAMPLE_INIT(_ssao.Init(*dev, sw, sh));
        // Motion vector — TAA / SSR temporal の object motion 用
        ACS_SAMPLE_INIT(_motion.Init(*dev, sw, sh));
        // Refraction (Phase 35-3b/3c/3d) — 屈折ガラス球
        ACS_SAMPLE_INIT(_refr.Init(*dev, _post.HdrFormat(), GetRenderer().DepthFormat()));
        ACS_SAMPLE_INIT(_blit.Init(*dev, _post.HdrFormat()));
        {
            TextureDesc bg_td{};
            bg_td.width  = sw;
            bg_td.height = sh;
            bg_td.format = _post.HdrFormat();
            bg_td.is_render_target = true;
            auto bg_r = CreateRhiTexture(*dev, bg_td);
            if (bg_r.IsErr()) {
                ACS_LOG_ERROR("HelloShowcase: background RT 作成失敗: %s", bg_r.Error().message);
                Quit(); return;
            }
            _bg_rt = Move(bg_r.Value());
        }

        // SpriteBatch (LDR tonemap 後のオーバーレイ用)
        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
        _camera.SetPerspective(45.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

        // post params: 控えめな bloom + ACES + mild vignette
        // sun.color が 1.2x (HelloIbl 0.9x 互換) なので threshold もそれ相応に下げて
        // emissive orb (strength=4.0) の bloom がちゃんと光るようにする。
        _post_params.bloom_threshold      = 1.0f;
        _post_params.bloom_intensity      = 0.40f;
        _post_params.vignette_intensity   = 0.25f;
        _post_params.chromatic_aberration = 0.0f;        // 邪魔なので OFF
        _post_params.grain_intensity      = 0.0f;
        _post_params.tonemap_kind         = 0;           // 0=ACES (Narkowicz)

        // exposure を Day 想定にセット
        _exposure_target = 0.7f;
        _adapted_exposure = 0.7f;
    }

    void OnUpdate(f32 dt) noexcept override {
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
        const Vec3 cam_target{0.0f, 0.4f, 0.0f};
        _cam_pos = Vec3{
            cam_target.x + cam_radius * Sin(_orbit_angle),
            cam_y_base + cam_y_bob,
            cam_target.z + cam_radius * Cos(_orbit_angle),
        };
        _camera.SetLookAt(_cam_pos, cam_target, Vec3::Up());
    }

    bool OnCustomFrame() noexcept override {
        IRhiDevice*      dev = GetRenderer().Device();
        IRhiCommandList* cl  = GetRenderer().CommandList();
        IRhiSwapchain*   sc  = GetRenderer().Swapchain();
        IRhiTexture*     depth = GetRenderer().DepthBuffer();
        IRhiTexture*     hdr   = _post.HdrRenderTarget();
        if (!dev || !cl || !sc || !hdr || !depth) return false;

        cl->Begin();

        // ============ TAA jitter (Halton 2,3) ============
        // Halton(0, b) = 0 を避けるため +1 オフセット (HelloIbl と同様)
        const f32 jx = Halton((_taa_frame_index & 31) + 1, 2) - 0.5f;
        const f32 jy = Halton((_taa_frame_index & 31) + 1, 3) - 0.5f;
        ++_taa_frame_index;
        const f32 jx_ndc = jx * 2.0f / static_cast<f32>(hdr->Width());
        const f32 jy_ndc = jy * 2.0f / static_cast<f32>(hdr->Height());
        const Mat4 view_proj_jittered = _camera.ViewProjection() *
                                        Mat4::Translation(Vec3{jx_ndc, jy_ndc, 0.0f});
        const Mat4 vp_no_jitter = _camera.ViewProjection();
        const Mat4 vp_for_render = view_proj_jittered;
        const Mat4 inv_vp        = Inverse(vp_for_render);

        // ============ IBL init (1 度だけ走る) ============
        if (!_ibl.HasBrdfLut())       (void)_ibl.EnsureBrdfLut(*dev, *cl);
        if (!_ibl.HasEnvCubemap())    (void)_ibl.EnsureEnvCubemap(*dev, *cl, _sky);
        if (!_ibl.HasIrradianceMap()) (void)_ibl.EnsureIrradiance(*dev, *cl);
        if (!_ibl.HasPrefilterMap())  (void)_ibl.EnsurePrefilter(*dev, *cl);

        // ============ Opaque HDR pass ============
        cl->BeginRenderToTexture(*hdr, ClearColor{0, 0, 0, 1}, depth, 1.0f);

        Viewport vp{}; vp.width  = static_cast<f32>(hdr->Width());
                       vp.height = static_cast<f32>(hdr->Height());
        cl->SetViewport(vp);
        ScissorRect svr{}; svr.right  = static_cast<i32>(hdr->Width());
                           svr.bottom = static_cast<i32>(hdr->Height());
        cl->SetScissor(svr);

        // Sky (background skybox)
        _sky.Render(*cl, _camera);

        // PBR scene — light + IBL + 1-frame-latency 合成 (SSR / SSAO)
        DirLight sun{};
        sun.direction = _sky.SunDirection();
        sun.color     = _sky.SunColor() * 1.2f;
        _pbr.SetLights(vp_for_render, _cam_pos, &sun, 1, Vec3{0, 0, 0});
        _pbr.SetIbl(_ibl.IrradianceMap(), _ibl.PrefilterMap(),
                    _ibl.BrdfLut(), _ibl.PrefilterMips());
        // SSR は前フレーム結果を合成 (1-frame latency)。warm flag が立つまで null。
        if (_show_ssr && _ssr_warm && _ssr.OutputTexture()) {
            _pbr.SetSsr(_ssr.OutputTexture(), /*intensity=*/1.0f);
        } else {
            _pbr.SetSsr(nullptr, 0.0f);
        }
        // SSAO も 1-frame latency。warm flag でフレーム 0 のゴミ防止。
        if (_ssao_warm) {
            _pbr.SetSsao(_ssao.OutputTexture(), /*intensity=*/0.8f,
                         hdr->Width(), hdr->Height());
        } else {
            _pbr.SetSsao(nullptr, 0.0f, hdr->Width(), hdr->Height());
        }

        // Floor
        const Mat4 floor_model = Mat4::Translation(Vec3{0, kFloorY, 0});
        _pbr.SetExtParams(0.0f, 0.5f, 0.0f, Vec3{1, 0, 0});
        _pbr.SetSheen(Vec3{0, 0, 0}, 0.0f, 0.0f);
        _pbr.SetIridescence(0.0f, 400.0f, 1.35f);
        _pbr.SetSubsurface(Vec3{0, 0, 0}, 0.0f);
        _pbr.DrawMesh(*cl, _gm_floor, floor_model,
                      Vec3{0.18f, 0.19f, 0.21f}, /*metallic=*/0.0f,
                      /*roughness=*/0.45f, /*ao=*/1.0f);

        // 4 spheres
        for (u32 i = 0; i < kSphereCount; ++i) {
            const MaterialKind k = kSphereKind[i];
            if (k == MaterialKind::ClearGlass || k == MaterialKind::FrostedGlass) {
                // glass は refraction pass で描く (この opaque pass では skip)
                continue;
            }
            const Mat4 m = Mat4::Scale(Vec3{kSphereScale, kSphereScale, kSphereScale}) *
                           Mat4::Translation(Vec3{kSphereX[i], kSphereY, kSphereZ});
            // material 再 reset (前の sphere の値が残らないように)
            _pbr.SetExtParams(0.0f, 0.5f, 0.0f, Vec3{1, 0, 0});
            _pbr.SetSheen(Vec3{0, 0, 0}, 0.0f, 0.0f);
            _pbr.SetIridescence(0.0f, 400.0f, 1.35f);
            _pbr.SetSubsurface(Vec3{0, 0, 0}, 0.0f);
            _pbr.SetEmissive(Vec3{0, 0, 0}, 0.0f);

            if (k == MaterialKind::Gold) {
                // 金: metallic 1.0、low roughness、base color は典型的な金
                _pbr.DrawMesh(*cl, _gm_sphere, m,
                              Vec3{1.00f, 0.78f, 0.34f}, 1.0f, 0.15f, 1.0f);
            } else /* Ceramic */ {
                // セラミック: 高 roughness、白ベース
                _pbr.DrawMesh(*cl, _gm_sphere, m,
                              Vec3{0.92f, 0.92f, 0.92f}, 0.0f, 0.75f, 1.0f);
            }
        }

        // Emissive orbs (公転 + 鼓動的なスケール pulse)
        Mat4 orb_curr[kOrbCount]{};
        for (u32 i = 0; i < kOrbCount; ++i) {
            const f32 ang = _orb_phase + static_cast<f32>(i) * kPi;
            const Vec3 pos{
                kOrbOrbitRadius * Sin(ang),
                kOrbY,
                kOrbOrbitRadius * Cos(ang),
            };
            // 0.85x ~ 1.15x の鼓動的 pulse (orb ごとに位相をずらす)
            const f32 pulse = kOrbScale *
                              (1.0f + 0.15f * Sin(_orb_phase * 2.7f
                                                   + static_cast<f32>(i) * (kPi * 0.5f)));
            orb_curr[i] = Mat4::Scale(Vec3{pulse, pulse, pulse}) *
                          Mat4::Translation(pos);
            _pbr.SetExtParams(0.0f, 0.5f, 0.0f, Vec3{1, 0, 0});
            _pbr.SetSheen(Vec3{0, 0, 0}, 0.0f, 0.0f);
            _pbr.SetIridescence(0.0f, 400.0f, 1.35f);
            _pbr.SetSubsurface(Vec3{0, 0, 0}, 0.0f);
            _pbr.SetEmissive(kOrbColors[i], kOrbEmissiveStrength);
            _pbr.DrawMesh(*cl, _gm_sphere, orb_curr[i],
                          kOrbColors[i], 0.0f, 0.4f, 1.0f);
        }
        _pbr.SetEmissive(Vec3{0, 0, 0}, 0.0f);

        cl->EndRenderToTexture(*hdr);

        // ============ Refraction pass (Phase 35-3b/3c/3d) ============
        if (_show_refraction) {
            _blit.Copy(*cl, *hdr, *_bg_rt);
            cl->BeginRenderToTextureLoad(*hdr, depth);
            cl->SetViewport(vp);
            cl->SetScissor(svr);
            _refr.SetFrame(vp_for_render, _cam_pos);
            IRhiTexture* env = _ibl.HasPrefilterMap() ? _ibl.PrefilterMap()
                                                       : _ibl.EnvCubemap();
            if (env) {
                for (u32 i = 0; i < kSphereCount; ++i) {
                    const MaterialKind k = kSphereKind[i];
                    if (k != MaterialKind::ClearGlass && k != MaterialKind::FrostedGlass)
                        continue;
                    const Mat4 m = Mat4::Scale(Vec3{kSphereScale, kSphereScale, kSphereScale}) *
                                   Mat4::Translation(Vec3{kSphereX[i], kSphereY, kSphereZ});
                    const f32 roughness = (k == MaterialKind::FrostedGlass) ? 0.5f : 0.0f;
                    const Vec3 tint = (k == MaterialKind::FrostedGlass)
                                          ? Vec3{0.95f, 0.97f, 1.0f}    // 弱く青
                                          : Vec3{1.0f,  1.0f,  1.0f};   // クリア
                    // clear glass は dispersion を入れて diamond/prism 風の色分離を見せる
                    const f32 dispersion = (k == MaterialKind::ClearGlass) ? 0.5f : 0.0f;
                    _refr.DrawMesh(*cl, _gm_sphere, m, *_bg_rt, *env,
                                   /*ior=*/1.5f, /*thickness=*/0.45f, tint,
                                   roughness, dispersion);
                }
            }
            cl->EndRenderToTexture(*hdr);
        }

        // ============ Geometry G-buffer pass (motion + normal) ============
        // SSR / TAA が使う。床 + opaque sphere + glass + orb を含める。
        {
            const Mat4 motion_prev_vp = _prev_vp_valid ? _prev_vp_no_jitter
                                                       : vp_no_jitter;
            _motion.Begin(*cl, vp_no_jitter, motion_prev_vp);
            _motion.DrawMesh(*cl, _gm_floor, floor_model, floor_model);
            for (u32 i = 0; i < kSphereCount; ++i) {
                const Mat4 m = Mat4::Scale(Vec3{kSphereScale, kSphereScale, kSphereScale}) *
                               Mat4::Translation(Vec3{kSphereX[i], kSphereY, kSphereZ});
                _motion.DrawMesh(*cl, _gm_sphere, m, m);
            }
            for (u32 i = 0; i < kOrbCount; ++i) {
                // orb は dynamic — prev pos も計算
                const f32 ang_prev = _prev_orb_phase + static_cast<f32>(i) * kPi;
                const Vec3 pos_prev{
                    kOrbOrbitRadius * Sin(ang_prev),
                    kOrbY,
                    kOrbOrbitRadius * Cos(ang_prev),
                };
                const Mat4 prev = Mat4::Scale(Vec3{kOrbScale, kOrbScale, kOrbScale}) *
                                  Mat4::Translation(pos_prev);
                _motion.DrawMesh(*cl, _gm_sphere, orb_curr[i],
                                 _prev_vp_valid ? prev : orb_curr[i]);
            }
            _motion.End(*cl);
        }
        IRhiTexture* motion_tex = _prev_vp_valid ? _motion.OutputTexture() : nullptr;
        _post_params.taa_motion_texture = motion_tex;

        // ============ SSR pass ('R' で toggle) ============
        if (_show_ssr) {
            // Hi-Z (Phase 36-3a): SSR の skip-ahead 用 coarse min-depth を depth から焼く
            _hiz.Build(*dev, *cl, *depth);
            const Mat4& ssr_prev_vp = _prev_vp_valid ? _prev_vp_no_jitter : vp_no_jitter;
            _ssr.Render(*dev, *cl, *hdr, *depth,
                        *_motion.OutputNormalTexture(),
                        vp_for_render, inv_vp, ssr_prev_vp,
                        _cam_pos, /*intensity=*/0.7f, motion_tex,
                        _hiz.Texture());
            _ssr_warm = true;
        }

        // ============ SSAO (GTAO) pass ============
        _ssao.Render(*dev, *cl, *depth,
                     *_motion.OutputNormalTexture(),
                     vp_for_render, inv_vp, _camera.View(),
                     _cam_pos, _sky.SunDirection(),
                     /*intensity=*/0.7f, /*radius=*/0.5f);
        _ssao_warm = true;   // 次フレームから PbrShader が読める

        // ============ PostProcess: HDR → LDR (Bloom + ACES + Auto-exposure)
        _post_params.taa_enabled = true;
        _post_params.taa_blend_factor = 0.1f;
        _post_params.taa_depth_texture = (_prev_vp_valid) ? depth : nullptr;
        _post_params.taa_view_proj_no_jitter      = vp_no_jitter;
        _post_params.taa_prev_view_proj_no_jitter = _prev_vp_no_jitter;
        _post_params.auto_exposure_enabled = true;
        _post_params.auto_exposure_key = 0.5f;
        _post_params.exposure          = 1.0f;   // auto 時は GPU 側適用

        const u32 buf_idx = sc->AcquireNextImage();
        // PostProcess::Render が内部で BeginRenderToSwapchain を発行する。
        _post.Render(*cl, *sc, buf_idx, _post_params);

        // ============ HUD overlay (post-process が bind した swapchain に描く) ============
        _batch.Begin(*cl, sc->Width(), sc->Height());
        if (_font.AtlasTexture()) {
            _batch.DrawString(_font, "ACS Showcase — PBR / IBL / Refraction / Bloom / ACES",
                              20, 20, Vec4{1, 1, 1, 1});
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "Pause=%s  SSR=%s  Refract=%s",
                          _paused ? "ON" : "OFF",
                          _show_ssr ? "ON" : "OFF",
                          _show_refraction ? "ON" : "OFF");
            _batch.DrawString(_font, buf, 20, 44, Vec4{0.85f, 0.85f, 0.85f, 1});
            _batch.DrawString(_font, "P: pause  R: SSR  X: refract  Esc: 終了",
                              20, 68, Vec4{0.7f, 0.85f, 1.0f, 1});
        }
        _batch.End();

        cl->EndRenderToSwapchain(*sc, buf_idx);
        cl->End();
        cl->Submit();
        sc->Present();

        // ===== 次フレーム用の前 VP / orb phase を保存 (TAA reprojection / motion) =====
        _prev_vp_no_jitter = vp_no_jitter;
        _prev_orb_phase    = _orb_phase;
        _prev_vp_valid     = true;

        return true;
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _bg_rt.Reset();
        _blit.Shutdown();
        _refr.Shutdown();
        _motion.Shutdown();
        _ssao.Shutdown();
        _ssr.Shutdown();
        _gm_floor  = GpuMesh{};
        _gm_sphere = GpuMesh{};
        _hiz.Shutdown();
        _pbr.Shutdown();
        _ibl.Shutdown();
        _sky.Shutdown();
        _post.Shutdown();
    }

private:
    PostProcess        _post;
    ImageBasedLighting _ibl;
    Sky                _sky;
    PbrShader          _pbr;
    Ssr                _ssr;
    Ssao               _ssao;
    HiZ                _hiz;            // Phase 36-3a: SSR の skip-ahead 用 coarse min-depth
    MotionVector       _motion;
    RefractionShader   _refr;
    Blit               _blit;
    UniquePtr<IRhiTexture> _bg_rt;
    GpuMesh            _gm_sphere;
    GpuMesh            _gm_floor;
    SpriteBatch        _batch;
    Font               _font;
    Camera             _camera;
    PostProcessParams  _post_params;
    Vec3               _cam_pos        = Vec3{0, 1.4f, -5.5f};
    f32                _orbit_angle    = 0.0f;     // カメラ orbit (rad)
    f32                _orb_phase      = 0.0f;     // emissive オーブの位相 (rad)
    f32                _prev_orb_phase = 0.0f;
    f32                _exposure_target  = 0.7f;
    f32                _adapted_exposure = 0.7f;
    Mat4               _prev_vp_no_jitter{};
    bool               _prev_vp_valid  = false;
    u32                _taa_frame_index = 0;
    bool               _show_ssr       = true;
    bool               _ssr_warm       = false;
    bool               _ssao_warm      = false;
    bool               _show_refraction = true;
    bool               _paused         = false;
};

ACS_DEFINE_MAIN(HelloShowcase)
