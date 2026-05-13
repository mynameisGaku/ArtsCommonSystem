// ACS の HelloIbl サンプル — Phase 31 Step 3 マイルストーン
//
// 動作:
//   ・初フレームで BRDF LUT (256x256 RG16F) を生成
//   ・同時に Sky procedural から env cubemap (256x256x6 R11G11B10_Float) をキャプチャ
//   ・以降のフレームで:
//     - env cubemap を skybox として fullscreen 背景描画
//     - BRDF LUT を画面右上に重ねて表示
//     - 1/2/3 キーで Sky preset (Day / Sunset / Night) を切替 → cubemap 再生成
//
// 後続ステップ (Step 4+) で irradiance / prefilter / PbrShader IBL を順次組み込み、
// 5x5 sphere grid を IBL のみで点灯する形に拡張する。
//
// 注: -DACS_RENDER_DILIGENT=ON 必須 (per-slice RT / cubemap / R11G11B10F が Diligent 専用)。
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

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

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

        ACS_SAMPLE_INIT(_sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
        _sky.PresetDay();
        ACS_SAMPLE_INIT(_pbr.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

        auto sphere = Primitive::MakeSphere(0.55f, 48, 24);
        ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));

        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
        _cam_pos = Vec3{0, 1.0f, -5.0f};
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();

        // 1/2/3 で sky preset 切替 → 次フレームで cubemap が再生成される
        if (Input::IsKeyPressed(Key::Num1)) {
            _sky.PresetDay();    _current_preset = 0; _need_recapture = true;
        }
        if (Input::IsKeyPressed(Key::Num2)) {
            _sky.PresetSunset(); _current_preset = 1; _need_recapture = true;
        }
        if (Input::IsKeyPressed(Key::Num3)) {
            _sky.PresetNight();  _current_preset = 2; _need_recapture = true;
        }
        if (Input::IsKeyPressed(Key::I)) {
            // 0=env / 1=irradiance / 2..6=prefilter mip 0..4
            _display_mode = (_display_mode + 1) % 7;
        }

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

    void OnRender() noexcept override {
        IRhiDevice*     dev = GetRenderer().Device();
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!dev || !cl) return;

        // Sky preset が変わったら env cubemap を再生成。
        // GPU が前フレームの draw でこの env_cube を参照しているうちに Reset
        // すると UB なので、Reset 前に WaitIdle で完了を待つ。
        // ResetEnvCubemap は env_cube + irradiance だけ消し、BRDF LUT と
        // skybox preview pipeline は残す (cheap)。
        if (_need_recapture) {
            dev->WaitIdle();
            _ibl.ResetEnvCubemap();
            _need_recapture = false;
        }

        if (!_ibl.HasBrdfLut()) {
            if (auto r = _ibl.EnsureBrdfLut(*dev, *cl); r.IsErr())
                ACS_LOG_ERROR("HelloIbl: EnsureBrdfLut failed");
        }
        if (!_ibl.HasEnvCubemap()) {
            if (auto r = _ibl.EnsureEnvCubemap(*dev, *cl, _sky); r.IsErr())
                ACS_LOG_ERROR("HelloIbl: EnsureEnvCubemap failed");
        }
        if (!_ibl.HasIrradianceMap()) {
            if (auto r = _ibl.EnsureIrradiance(*dev, *cl); r.IsErr())
                ACS_LOG_ERROR("HelloIbl: EnsureIrradiance failed");
        }
        if (!_ibl.HasPrefilterMap()) {
            if (auto r = _ibl.EnsurePrefilter(*dev, *cl); r.IsErr())
                ACS_LOG_ERROR("HelloIbl: EnsurePrefilter failed");
        }

        // 背景: 'I' でモード巡回
        //   0=env / 1=irradiance / 2..6=prefilter mip 0..4
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
                            GetRenderer().ColorFormat(), GetRenderer().DepthFormat(),
                            mip_level);
        }

        // === PBR sphere grid (5x5)、IBL のみで点灯 ===
        // ・PbrShader.SetIbl() で irradiance / prefilter / brdf を提供
        // ・SetLights で direct light count=0 + ambient=(0,0,0) を渡す。IBL ambient
        //   が ibl_enabled=1 で flat ambient を置換するので、見た目は IBL 由来の
        //   照り返し + Fresnel rim だけになる。Day preset の sky 反射が球面に出る。
        _pbr.SetIbl(_ibl.IrradianceMap(), _ibl.PrefilterMap(), _ibl.BrdfLut(),
                    _ibl.PrefilterMips());
        _pbr.SetLights(_camera.ViewProjection(), _camera.Eye(),
                       nullptr, 0, Vec3{0, 0, 0});
        // ダミー point lights (count=0) でリセット
        _pbr.SetPointLights(nullptr, 0);

        constexpr u32 kGrid = 5;
        constexpr f32 kSpacing = 1.4f;
        const Vec3 base_color{0.95f, 0.4f, 0.3f};         // 銅系 (metallic で映える)
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

        // 右上に BRDF LUT を重ね表示
        IRhiTexture* lut = _ibl.BrdfLut();
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);
        if (lut) {
            // 半透明背景 + LUT + 枠
            _batch.DrawRect(static_cast<f32>(sw) - 280, 20,
                            260, 320, Vec4{0, 0, 0, 0.55f});
            _batch.Draw(*lut,
                        static_cast<f32>(sw) - 270, 60,
                        240, 240);
        }
        if (_font.AtlasTexture()) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Phase 31 IBL  FPS: %.1f", static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});

            const char* preset =
                (_current_preset == 0) ? "Day" :
                (_current_preset == 1) ? "Sunset" : "Night";
            std::snprintf(buf, sizeof(buf), "Sky preset: [%s]   (1/2/3 で切替)", preset);
            _batch.DrawString(_font, buf, 20, 44, Vec4{0.85f, 0.95f, 1.0f, 1});

            const char* view_label = nullptr;
            switch (_display_mode) {
                case 0: view_label = "Env cubemap (Sky procedural)";          break;
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
            _batch.DrawString(_font, "WASD: 移動   矢印: 視点回転   Esc: 終了",
                              20, 92, Vec4{0.7f, 0.85f, 1.0f, 1});
            if (lut) {
                _batch.DrawString(_font, "BRDF LUT",
                                  static_cast<f32>(sw) - 260, 36, Vec4{1, 1, 1, 1});
                _batch.DrawString(_font, "X:NdotV  Y:roughness",
                                  static_cast<f32>(sw) - 265, 308, Vec4{0.85f, 0.85f, 0.85f, 1});
            }
        }
        _batch.End();
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _gm_sphere = GpuMesh{};
        _pbr.Shutdown();
        _ibl.Shutdown();
        _sky.Shutdown();
    }

private:
    ImageBasedLighting _ibl;
    Sky                _sky;
    PbrShader          _pbr;
    GpuMesh            _gm_sphere;
    SpriteBatch        _batch;
    Font               _font;
    Camera             _camera;
    Vec3               _cam_pos   = Vec3{0, 1.0f, -5.0f};
    f32                _cam_yaw   = 0.0f;
    f32                _cam_pitch = 0.0f;
    i32                _current_preset = 0;     // 0=Day, 1=Sunset, 2=Night
    bool               _need_recapture = false;
    u32                _display_mode   = 0;     // 0=env / 1=irradiance / 2..6=prefilter mip 0..4
};

ACS_DEFINE_MAIN(HelloIbl)
