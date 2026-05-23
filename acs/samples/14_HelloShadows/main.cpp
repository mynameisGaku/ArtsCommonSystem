// SPDX-License-Identifier: Apache-2.0
// ACS の HelloShadows サンプル
//
// 動作:
//   ・ライト視点の depth-only パスでシャドウマップを焼く
//   ・主パスでメッシュを StandardShader + シャドウサンプリングで描画
//   ・複数のキューブ + 球が地面に影を落とす
//   ・WASD で歩き回り、矢印キーで視点回転
//   ・Space で太陽の方向を回転（影の動きが見える）
//   ・Esc 終了
//
// 学習ポイント:
//   ・ShadowMap::SetDirectionalLight でライト VP を計算
//   ・cl->BeginShadowPass / EndShadowPass の使い方
//   ・StandardShader::SetShadowMap で主パスにシャドウを通知
//   ・PCSS (Fernando 2005) で物理的に正しいソフトシャドウ

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "render/Sky.h"
#include "render/StandardShader.h"
#include "render/ShadowMap.h"
#include "render/RenderAssets.h"
#include "render/SpriteBatch.h"
#include "render/Font.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

struct CasterInst {
    Mat4 model;
    Vec3 base_color;
    bool is_sphere;
};

constexpr u32 kCasterCount = 8;

} // namespace

class HelloShadows : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        ACS_SAMPLE_INIT(_sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
        _sky.PresetDay();

        ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
        ACS_SAMPLE_INIT(_shadow.Init(*dev, /*size=*/2048));

        // メッシュ
        auto cube   = Primitive::MakeCube(1.0f);
        auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
        auto plane  = Primitive::MakePlane(40.0f, 40.0f);
        ACS_SAMPLE_INIT(UploadMesh(*dev, *cube,   _gm_cube));
        ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
        ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));

        BuildScene();

        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
        _cam_pos = Vec3{0, 4, -10.0f};

        // シャドウ機能を ON
        _shader.SetShadowMap(_shadow.DepthTexture(), _shadow.LightViewProjection(), 0.001f);

        ACS_LOG_INFO("HelloShadows initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) Quit();
        _time += dt;

        if (Input::IsKeyDown(EKey::Space)) _sun_yaw += dt * 0.6f;

        // カメラ
        const f32 mv = 5.0f * dt, tr = 1.5f * dt;
        if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= tr;
        if (Input::IsKeyDown(EKey::Right)) _cam_yaw += tr;
        if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= tr * 0.8f;
        if (Input::IsKeyDown(EKey::Down))  _cam_pitch += tr * 0.8f;
        const f32 limit = 0.45f * kPi;
        if (_cam_pitch >  limit) _cam_pitch =  limit;
        if (_cam_pitch < -limit) _cam_pitch = -limit;
        Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                     -Sin(_cam_pitch),
                      Cos(_cam_yaw) * Cos(_cam_pitch) };
        Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };
        if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * mv;
        if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * mv;
        if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * mv;
        if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * mv;
        _camera.SetLookAt(_cam_pos, _cam_pos + forward);
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;

        // 太陽方向（方向 TO 光源、Y 上向き）
        Vec3 sun_dir{
            Sin(_sun_yaw) * 0.6f,
            0.85f,
            Cos(_sun_yaw) * 0.6f,
        };
        _sky.SetSunDirection(sun_dir);

        // === 1. シャドウパス ===
        _shadow.SetDirectionalLight(sun_dir, Vec3{0, 1, 0}, /*radius=*/12.0f);
        cl->BeginShadowPass(*_shadow.DepthTexture(), 1.0f);
        cl->SetPipeline(*_shadow.CasterPipeline());
        cl->SetConstantBuffer(0, *_shadow.LightCB());
        cl->SetConstantBuffer(1, *_shadow.CasterObjectCB());

        // 地面はシャドウキャスタにしないので、影を落とす物体だけ
        for (const CasterInst& obj : _casters) {
            _shadow.SetCaster(obj.model);
            const GpuMesh& m = obj.is_sphere ? _gm_sphere : _gm_cube;
            cl->SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
            cl->SetIndexBuffer(*m.index_buffer);
            cl->DrawIndexed(m.index_count);
        }
        cl->EndShadowPass(*_shadow.DepthTexture());

        // === 2. 主パス: スカイ → シーン ===
        _sky.Render(*cl, _camera);

        DirLight lights[1];
        lights[0].direction = sun_dir;
        lights[0].color     = _sky.SunColor();
        Vec3 ambient{0.20f, 0.22f, 0.28f};
        _shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                          lights, 1, ambient);
        // シャドウ VP を最新化
        _shader.SetShadowMap(_shadow.DepthTexture(), _shadow.LightViewProjection(), 0.001f);

        cl->SetPipeline(*_shader.Pipeline());
        cl->SetConstantBuffer(0, *_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_shader.PerObjectCB());
        cl->SetTexture(0, *_shader.DefaultWhiteTexture());
        cl->SetTexture(1, *_shader.ShadowTextureOrDefault());

        // 地面（受影、キャストしない）
        _shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                          Vec3{0.55f, 0.55f, 0.6f}, 0.05f, 4.0f);
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // 物体
        for (const CasterInst& obj : _casters) {
            _shader.SetObject(obj.model, obj.base_color, 0.4f, 32.0f);
            const GpuMesh& m = obj.is_sphere ? _gm_sphere : _gm_cube;
            cl->SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
            cl->SetIndexBuffer(*m.index_buffer);
            cl->DrawIndexed(m.index_count);
        }

        // === HUD ===
        if (_font.AtlasTexture()) {
            const u32 sw = GetRenderer().Swapchain()->Width();
            const u32 sh = GetRenderer().Swapchain()->Height();
            _batch.Begin(*cl, sw, sh);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "シャドウマップ %ux%u  PCSS 4x4+4x4  FPS: %.1f",
                          _shadow.Size(), _shadow.Size(), static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1, 1, 1, 1});
            _batch.DrawString(_font, "WASD: 移動  矢印: 視点  Space: 太陽回転  Esc: 終了",
                            20, 44, Vec4{0.8f, 0.85f, 0.95f, 1});
            _batch.End();
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _batch.Shutdown();
        _gm_plane = GpuMesh{};
        _gm_sphere = GpuMesh{};
        _gm_cube = GpuMesh{};
        _shadow.Shutdown();
        _shader.Shutdown();
        _sky.Shutdown();
    }

private:
    void BuildScene() noexcept {
        _casters.Clear();
        // 中央に背の高い柱
        for (u32 i = 0; i < 4; ++i) {
            const f32 a = (kPi * 2.0f) * i / 4 + 0.4f;
            const f32 r = 3.0f;
            CasterInst c;
            c.is_sphere = false;
            c.base_color = Vec3{0.85f, 0.7f, 0.5f};
            c.model = Mat4::Scale(Vec3{0.6f, 2.0f, 0.6f}) *
                      Mat4::Translation(Vec3{Sin(a) * r, 1.0f, Cos(a) * r});
            _casters.PushBack(c);
        }
        // 周囲に球（複数色）
        const Vec3 colors[4] = {
            {0.95f, 0.45f, 0.45f},
            {0.45f, 0.85f, 0.55f},
            {0.45f, 0.55f, 0.95f},
            {0.95f, 0.85f, 0.45f},
        };
        for (u32 i = 0; i < 4; ++i) {
            const f32 a = (kPi * 2.0f) * i / 4 + 0.785f;
            const f32 r = 1.5f;
            CasterInst c;
            c.is_sphere = true;
            c.base_color = colors[i];
            c.model = Mat4::Scale(Vec3{1.0f, 1.0f, 1.0f}) *
                      Mat4::Translation(Vec3{Sin(a) * r, 0.5f, Cos(a) * r});
            _casters.PushBack(c);
        }
    }

    Sky               _sky;
    StandardShader    _shader;
    ShadowMap         _shadow;
    GpuMesh           _gm_cube, _gm_sphere, _gm_plane;
    Array<CasterInst> _casters;
    SpriteBatch       _batch;
    Font              _font;
    Camera            _camera;
    Vec3              _cam_pos{0, 4, -10};
    f32               _cam_yaw = 0.0f, _cam_pitch = 0.0f;
    f32               _sun_yaw = 0.5f;
    f32               _time = 0.0f;
};

ACS_DEFINE_MAIN(HelloShadows)
