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

        // SpriteBatch は LDR backbuffer (tonemap 後)
        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(sw) / static_cast<f32>(sh);
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
        _cam_pos = Vec3{0, 1.0f, -5.0f};

        // Bloom 強度はデフォルトより少し弱めに (Day sky は十分明るいので)
        _post_params.bloom_threshold = 1.5f;
        _post_params.bloom_intensity = 0.4f;
        _post_params.exposure        = 1.0f;
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();

        // 1/2/3/4 で env 切替。SH9 mode が有効中は SH 9 係数も再計算が必要
        if (Input::IsKeyPressed(Key::Num1)) {
            _sky.PresetDay();    _current_preset = 0;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        }
        if (Input::IsKeyPressed(Key::Num2)) {
            _sky.PresetSunset(); _current_preset = 1;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        }
        if (Input::IsKeyPressed(Key::Num3)) {
            _sky.PresetNight();  _current_preset = 2;
            _need_recapture = true; _need_sh9_rebuild = _use_sh9;
        }
        if (Input::IsKeyPressed(Key::Num4)) {
            _current_preset = 3;
            _need_studio_hdr = true; _need_sh9_rebuild = _use_sh9;
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
        // B キーで bloom on/off (verify HDR clip 防止効果)
        if (Input::IsKeyPressed(Key::B)) {
            _post_params.bloom_enabled = !_post_params.bloom_enabled;
        }
        // E/Q で露出 ±
        if (Input::IsKeyDown(Key::E)) _post_params.exposure += dt * 0.5f;
        if (Input::IsKeyDown(Key::Q)) _post_params.exposure -= dt * 0.5f;
        if (_post_params.exposure < 0.1f) _post_params.exposure = 0.1f;
        if (_post_params.exposure > 4.0f) _post_params.exposure = 4.0f;

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

        // ===== IBL build (まだ作ってないものだけ。一度作れば cache される) =====
        // BeginRenderToTexture(hdr) の前にやる: IBL の RT 切替は _main_swapchain を
        // 触らないので、HDR pass に影響しない。
        if (!_ibl.HasBrdfLut())       _ibl.EnsureBrdfLut(*dev, *cl);
        if (!_ibl.HasEnvCubemap())    _ibl.EnsureEnvCubemap(*dev, *cl, _sky);
        if (!_ibl.HasIrradianceMap()) _ibl.EnsureIrradiance(*dev, *cl);
        if (!_ibl.HasPrefilterMap())  _ibl.EnsurePrefilter(*dev, *cl);

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
                            _camera.ViewProjection(), _camera.Eye(),
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
        _pbr.SetLights(_camera.ViewProjection(), _camera.Eye(),
                       &sun, 1, Vec3{0, 0, 0});
        _pbr.SetPointLights(nullptr, 0);

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
                const f32 py = (static_cast<f32>(y) - (kGrid - 1) * 0.5f) * kSpacing + 1.5f;
                _pbr.DrawMesh(*cl, _gm_sphere,
                              Mat4::Translation(Vec3{px, py, 3.0f}),
                              base_color, metallic, roughness, 1.0f);
            }
        }

        cl->EndRenderToTexture(*hdr);

        // ===== 2) Bloom + ACES Tonemap → LDR backbuffer =====
        _post.Render(*cl, *sc, buf_idx, _post_params);

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
        if (_font.AtlasTexture()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Phase 31/32a IBL+HDR  FPS: %.1f", static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});

            const char* preset =
                (_current_preset == 0) ? "Day" :
                (_current_preset == 1) ? "Sunset" :
                (_current_preset == 2) ? "Night" : "Studio HDR";
            std::snprintf(buf, sizeof(buf),
                          "Env preset: [%s]   (1/2/3 で sky procedural、4 で Studio HDR)", preset);
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

            std::snprintf(buf, sizeof(buf),
                          "Exposure: %.2f   Bloom: %s   Diffuse: %s   (Q/E exp, B bloom, S SH9)",
                          static_cast<double>(_post_params.exposure),
                          _post_params.bloom_enabled ? "ON" : "OFF",
                          _use_sh9 ? "SH9 (light probe)" : "Irradiance cube");
            _batch.DrawString(_font, buf, 20, 92, Vec4{0.9f, 0.9f, 0.9f, 1});

            std::snprintf(buf, sizeof(buf),
                          "Material:  Clearcoat=%s  Anisotropic=%s   (C/Z でトグル)",
                          _use_clearcoat ? "ON" : "OFF",
                          _use_anisotropy ? "ON" : "OFF");
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
        _gm_sphere = GpuMesh{};
        _pbr.Shutdown();
        _ibl.Shutdown();
        _sky.Shutdown();
        _post.Shutdown();
    }

private:
    static constexpr u32 kEquirectWidth  = 256;       // SH9 計算用、小さめで十分
    static constexpr u32 kEquirectHeight = 128;

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
    u32                _display_mode     = 0;
};

ACS_DEFINE_MAIN(HelloIbl)
