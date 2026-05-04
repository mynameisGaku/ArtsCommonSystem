// ACS の HelloSky サンプル
//
// 動作:
//   ・手続き生成スカイ（昼/夕焼け/夜のプリセット切替可）
//   ・地面の上で 1 つのメッシュ（メイン光源と同期した色）が回転
//   ・1 / 2 / 3 キーで昼 / 夕焼け / 夜のプリセット
//   ・Esc 終了
//
// 学習ポイント:
//   ・Sky::Render を OnRender 先頭で呼ぶ（深度書込み無し）
//   ・StandardShader::SetLights のライト色を Sky と整合させる
//   ・PresetXxx で雰囲気を一発切替

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "render/Sky.h"
#include "render/StandardShader.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

using namespace acs;

class HelloSky : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        if (auto r = _sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat());
            r.IsErr()) { ACS_LOG_ERROR("Sky init"); Quit(); return; }
        _sky.PresetDay();

        if (auto r = _shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat());
            r.IsErr()) { ACS_LOG_ERROR("Shader init"); Quit(); return; }

        auto sphere = Primitive::MakeSphere(1.0f, 48, 24);
        auto plane  = Primitive::MakePlane(50.0f, 50.0f);
        if (auto r = UploadMesh(*dev, *sphere, _gm_sphere); r.IsErr()) { Quit(); return; }
        if (auto r = UploadMesh(*dev, *plane,  _gm_plane);  r.IsErr()) { Quit(); return; }

        _batch.Init(*dev, GetRenderer().ColorFormat());
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f);

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
        _cam_pos = Vec3{0, 2.0f, -6.0f};

        ACS_LOG_INFO("HelloSky initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Num1)) { _sky.PresetDay();    _preset = 0; }
        if (Input::IsKeyPressed(Key::Num2)) { _sky.PresetSunset(); _preset = 1; }
        if (Input::IsKeyPressed(Key::Num3)) { _sky.PresetNight();  _preset = 2; }

        _angle += dt * 0.5f;

        // カメラ周回
        if (Input::IsKeyDown(Key::Left))  _cam_yaw -= dt * 1.0f;
        if (Input::IsKeyDown(Key::Right)) _cam_yaw += dt * 1.0f;
        const f32 cam_dist = 6.0f;
        _cam_pos = Vec3{ Sin(_cam_yaw) * cam_dist, 2.5f, -Cos(_cam_yaw) * cam_dist };
        _camera.SetLookAt(_cam_pos, Vec3{0, 1, 0});
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;

        // 1) 空を最初に描く
        _sky.Render(*cl, _camera);

        // 2) シーン描画 — メイン光源を Sky の太陽方向 + 色と合わせる
        DirLight lights[2];
        lights[0].direction = _sky.SunDirection();
        lights[0].color     = _sky.SunColor();
        // 環境光相当のフィル（Sky の zenith を弱めて）
        lights[1].direction = Vec3{-_sky.SunDirection().x, _sky.SunDirection().y * 0.5f, -_sky.SunDirection().z};
        lights[1].color     = Vec3{0.15f, 0.18f, 0.25f};
        Vec3 ambient;
        switch (_preset) {
            case 0: ambient = Vec3{0.20f, 0.22f, 0.30f}; break;     // day
            case 1: ambient = Vec3{0.20f, 0.10f, 0.10f}; break;     // sunset
            default:ambient = Vec3{0.04f, 0.05f, 0.10f}; break;     // night
        }

        _shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                          lights, 2, ambient);

        cl->SetPipeline(*_shader.Pipeline());
        cl->SetConstantBuffer(0, *_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_shader.PerObjectCB());
        cl->SetTexture(0, *_shader.DefaultWhiteTexture());
        cl->SetTexture(1, *_shader.ShadowTextureOrDefault());

        // 地面
        _shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                          Vec3{0.4f, 0.45f, 0.5f}, 0.1f, 8.0f);
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // 球
        Mat4 ms = Mat4::RotationY(_angle) *
                  Mat4::Translation(Vec3{0, 1.5f, 0});
        _shader.SetObject(ms, Vec3{1.0f, 0.85f, 0.4f}, 0.7f, 64.0f);
        cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
        cl->SetIndexBuffer(*_gm_sphere.index_buffer);
        cl->DrawIndexed(_gm_sphere.index_count);

        // HUD
        if (_font.AtlasTexture()) {
            const u32 sw = GetRenderer().Swapchain()->Width();
            const u32 sh = GetRenderer().Swapchain()->Height();
            _batch.Begin(*cl, sw, sh);
            const char* preset_name = (_preset == 0) ? "[1] 昼" :
                                      (_preset == 1) ? "[2] 夕焼け" : "[3] 夜";
            _batch.DrawString(_font, preset_name, 20, 20, Vec4{1,1,1,1});
            _batch.DrawString(_font, "1/2/3: プリセット切替  ←→: カメラ  Esc: 終了",
                            20, 44, Vec4{0.8f,0.85f,0.95f,1});
            _batch.End();
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _gm_plane = GpuMesh{};
        _gm_sphere = GpuMesh{};
        _shader.Shutdown();
        _sky.Shutdown();
    }

private:
    Sky            _sky;
    StandardShader _shader;
    SpriteBatch    _batch;
    Font           _font;
    GpuMesh _gm_sphere, _gm_plane;
    Camera _camera;
    Vec3   _cam_pos;
    f32    _cam_yaw = 0.5f;
    f32    _angle   = 0.0f;
    i32    _preset  = 0;
};

ACS_DEFINE_MAIN(HelloSky)
