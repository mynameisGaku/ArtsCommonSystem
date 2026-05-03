// ACS の HelloLights サンプル
//
// 動作:
//   ・暗い「部屋」（地面 + 4 つの壁）を作る
//   ・床に複数のキューブと球を配置
//   ・4 つの色違いの点光源が円周状を移動して照らす
//   ・主光源（dir）はほぼ無効（ambient 主体）にして点光源の効果を強調
//   ・WASD でカメラ移動、矢印で視点回転
//   ・Esc 終了

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"

#include "render/StandardShader.h"
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

constexpr u32 kCubeCount  = 6;
constexpr u32 kPointCount = 4;

struct ObjectInst {
    Mat4 model;
    Vec3 color;
    bool is_sphere;
};

} // namespace

class HelloLights : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        if (auto r = _shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat());
            r.IsErr()) { Quit(); return; }

        auto cube   = Primitive::MakeCube(1.0f);
        auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
        auto plane  = Primitive::MakePlane(20.0f, 20.0f);
        if (auto r = UploadMesh(*dev, *cube,   _gm_cube);   r.IsErr()) { Quit(); return; }
        if (auto r = UploadMesh(*dev, *sphere, _gm_sphere); r.IsErr()) { Quit(); return; }
        if (auto r = UploadMesh(*dev, *plane,  _gm_plane);  r.IsErr()) { Quit(); return; }

        BuildScene();

        _batch.Init(*dev, GetRenderer().ColorFormat());
        const wchar_t* fp[] = {
            L"C:\\Windows\\Fonts\\meiryo.ttc",
            L"C:\\Windows\\Fonts\\msgothic.ttc",
            L"C:\\Windows\\Fonts\\arial.ttf",
        };
        for (auto p : fp) { if (_font.LoadFromFile(*dev, p, 18.0f, 1024, true).IsOk()) break; }

        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
        _cam_pos = Vec3{0, 3.0f, -8.0f};
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        _time += dt;

        // カメラ
        const f32 mv = 5.0f * dt, tr = 1.5f * dt;
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
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl) return;

        // === ライト設定 ===
        // 弱い dir ライトで全体を最低限照らす
        DirLight dir;
        dir.direction = Vec3{0.3f, 1.0f, 0.4f};
        dir.color     = Vec3{0.05f, 0.05f, 0.07f};
        _shader.SetLights(_camera.ViewProjection(), _camera.Eye(),
                          &dir, 1, Vec3{0.04f, 0.04f, 0.06f});

        // 4 灯の点光源を円周状にゆっくり回す
        PointLight pts[kPointCount];
        const Vec3 colors[kPointCount] = {
            {1.0f, 0.3f, 0.3f},   // 赤
            {0.3f, 1.0f, 0.4f},   // 緑
            {0.3f, 0.4f, 1.0f},   // 青
            {1.0f, 0.9f, 0.3f},   // 黄
        };
        for (u32 i = 0; i < kPointCount; ++i) {
            const f32 a = (kPi * 2.0f) * static_cast<f32>(i) / kPointCount + _time * 0.4f;
            const f32 r = 4.5f;
            pts[i].position = Vec3{ Sin(a) * r, 1.5f + Sin(_time * 1.5f + i) * 0.6f, Cos(a) * r };
            pts[i].color    = colors[i] * 4.0f;        // 暗い部屋なので強めに
            pts[i].range    = 7.0f;
        }
        _shader.SetPointLights(pts, kPointCount);

        // === 描画 ===
        cl->SetPipeline(*_shader.Pipeline());
        cl->SetConstantBuffer(0, *_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_shader.PerObjectCB());
        cl->SetTexture(0, *_shader.DefaultWhiteTexture());
        cl->SetTexture(1, *_shader.ShadowTextureOrDefault());

        // 地面
        _shader.SetObject(Mat4::Translation(Vec3{0, 0, 0}),
                          Vec3{0.55f, 0.55f, 0.6f}, 0.2f, 32.0f);
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // オブジェクト
        for (const ObjectInst& obj : _objects) {
            _shader.SetObject(obj.model, obj.color, 0.5f, 32.0f);
            const GpuMesh& m = obj.is_sphere ? _gm_sphere : _gm_cube;
            cl->SetVertexBuffer(*m.vertex_buffer, m.vertex_stride);
            cl->SetIndexBuffer(*m.index_buffer);
            cl->DrawIndexed(m.index_count);
        }

        // 光源を可視化（小さな球を点光源位置に置いて自己発光風）
        // ベース色を非常に高く + 環境光に依存する形で「光ってる」風にする
        for (u32 i = 0; i < kPointCount; ++i) {
            const Vec3 col = pts[i].color * 0.3f;
            const Mat4 m = Mat4::Scale(Vec3{0.2f, 0.2f, 0.2f}) *
                            Mat4::Translation(pts[i].position);
            // 自己発光風: ambient を強く感じる色（材質側で base_color を高く）
            _shader.SetObject(m, col, 0.0f, 1.0f);
            cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
            cl->SetIndexBuffer(*_gm_sphere.index_buffer);
            cl->DrawIndexed(_gm_sphere.index_count);
        }

        // === HUD ===
        if (_font.AtlasTexture()) {
            const u32 sw = GetRenderer().Swapchain()->Width();
            const u32 sh = GetRenderer().Swapchain()->Height();
            _batch.Begin(*cl, sw, sh);
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "点光源 %u 灯 + 微弱 dir ライト   FPS: %.1f",
                          kPointCount, static_cast<double>(FPS()));
            _batch.DrawString(_font, buf, 20, 20, Vec4{1,1,1,1});
            _batch.DrawString(_font, "WASD: 移動  矢印: 視点  Esc: 終了",
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
        _shader.Shutdown();
    }

private:
    void BuildScene() noexcept {
        // 6 個の物体を散らす
        for (u32 i = 0; i < kCubeCount; ++i) {
            const f32 a = (kPi * 2.0f) * i / kCubeCount + 0.4f;
            const f32 r = 2.5f;
            ObjectInst o;
            o.is_sphere = (i & 1) != 0;
            const f32 height = (i & 1) ? 0.5f : 0.5f;
            const Vec3 pos{ Sin(a) * r, height, Cos(a) * r };
            o.model = Mat4::Translation(pos);
            o.color = Vec3{0.85f, 0.85f, 0.85f};   // ライトの色を反映するので白が映える
            _objects.PushBack(o);
        }
    }

    StandardShader _shader;
    GpuMesh _gm_cube, _gm_sphere, _gm_plane;
    Array<ObjectInst> _objects;
    SpriteBatch _batch;
    Font _font;
    Camera _camera;
    Vec3 _cam_pos{0, 3, -8};
    f32 _cam_yaw = 0.0f;
    f32 _cam_pitch = 0.0f;
    f32 _time = 0.0f;
};

ACS_DEFINE_MAIN(HelloLights)
