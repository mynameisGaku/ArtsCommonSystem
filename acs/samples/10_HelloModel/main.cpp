// SPDX-License-Identifier: Apache-2.0
// ACS の HelloModel サンプル
//
// 動作:
//   ・中央に回転する球 (Primitive::MakeSphere)
//   ・足元に地面プレーン (Primitive::MakePlane)
//   ・側にキューブが並ぶ
//   ・全部 Lambert ライティング (StandardShader)
//   ・カメラ: WASD で水平移動、矢印キーで視点回転
//   ・Esc 終了
//
// 学習ポイント:
//   ・StandardShader を使った「最短ライティング描画」
//   ・Primitive::MakeXxx で資源不要のプロトタイピング
//   ・LoadAsync の使い方（任意の path を非同期で読み、失敗時はフォールバック）
//   ・複数オブジェクトを 1 つのパイプラインで描画する流れ

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "asset/MeshPrimitive.h"
#include "asset/MeshAsset.h"
#include "asset/AssetFuture.h"

#include "render/StandardShader.h"
#include "render/RenderAssets.h"

#include "math/Camera.h"
#include "math/Mat.h"
#include "math/Math.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

using namespace acs;

class HelloModel : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // === 標準ライティングシェーダ ===
        ACS_SAMPLE_INIT(_shader.Init(*dev,
                                     GetRenderer().ColorFormat(),
                                     GetRenderer().DepthFormat()));

        // === プリミティブをアップロード ===
        auto sphere = Primitive::MakeSphere(0.8f, 48, 24);
        auto plane  = Primitive::MakePlane(20.0f, 20.0f);
        auto cube   = Primitive::MakeCube(0.6f);

        if (!sphere || !plane || !cube) { Quit(); return; }
        ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
        ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));
        ACS_SAMPLE_INIT(UploadMesh(*dev, *cube,   _gm_cube));

        // === オプションで非同期ロードを試みる（ファイルが無くても OK）===
        // 標準ローダ群は Application が自動で登録済み。
        _async_mesh = GetAssets().LoadAsync(L"data/optional_mesh.glb");

        // === カメラ ===
        const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                           static_cast<f32>(GetRenderer().Swapchain()->Height());
        _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
        _cam_pos = Vec3{0.0f, 1.5f, -5.0f};

        ACS_LOG_INFO("HelloModel initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(EKey::Escape)) Quit();

        // 球の自転
        _angle += dt * 0.6f;

        // === カメラ操作 ===
        const f32 move_speed = 4.0f * dt;
        const f32 turn_speed = 1.6f * dt;

        if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= turn_speed;
        if (Input::IsKeyDown(EKey::Right)) _cam_yaw += turn_speed;
        if (Input::IsKeyDown(EKey::Up))    _cam_pitch -= turn_speed * 0.8f;
        if (Input::IsKeyDown(EKey::Down))  _cam_pitch += turn_speed * 0.8f;
        // ピッチ制限
        const f32 limit = 0.45f * kPi;
        if (_cam_pitch >  limit) _cam_pitch =  limit;
        if (_cam_pitch < -limit) _cam_pitch = -limit;

        Vec3 forward{ Sin(_cam_yaw) * Cos(_cam_pitch),
                     -Sin(_cam_pitch),
                      Cos(_cam_yaw) * Cos(_cam_pitch) };
        Vec3 right{ Cos(_cam_yaw), 0, -Sin(_cam_yaw) };

        if (Input::IsKeyDown(EKey::W)) _cam_pos += forward * move_speed;
        if (Input::IsKeyDown(EKey::S)) _cam_pos -= forward * move_speed;
        if (Input::IsKeyDown(EKey::D)) _cam_pos += right   * move_speed;
        if (Input::IsKeyDown(EKey::A)) _cam_pos -= right   * move_speed;

        Vec3 target = _cam_pos + forward;
        _camera.SetLookAt(_cam_pos, target);

        // === 非同期ロード結果の確認 ===
        if (_async_mesh.Valid() && !_async_loaded && _async_mesh.IsReady()) {
            auto r = _async_mesh.Get();
            if (r.IsOk()) {
                ACS_LOG_INFO("Optional mesh loaded async OK");
            } else {
                ACS_LOG_INFO("Optional mesh not present (expected): %s",
                             r.Error().message);
            }
            _async_loaded = true;
        }
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_shader.Pipeline()) return;

        // Frame 共通設定（カメラ + 1 灯方向光 + 環境光）
        const Vec3 light_dir{ -0.5f, 0.8f, 0.3f };  // light direction TO light source
        _shader.SetFrame(_camera.ViewProjection(),
                         _camera.Eye(),
                         light_dir,
                         Vec3{1.0f, 0.95f, 0.9f},   // 暖色光
                         Vec3{0.10f, 0.12f, 0.18f}); // 寒色環境光

        cl->SetPipeline(*_shader.Pipeline());
        cl->SetConstantBuffer(0, *_shader.PerFrameCB());
        cl->SetConstantBuffer(1, *_shader.PerObjectCB());
        cl->SetTexture(0, *_shader.DefaultWhiteTexture());
        cl->SetTexture(1, *_shader.ShadowTextureOrDefault());

        // ---- 中央の球 ----
        _shader.SetObject(Mat4::RotationY(_angle), Vec3{1.0f, 0.85f, 0.4f});  // 山吹色
        cl->SetVertexBuffer(*_gm_sphere.vertex_buffer, _gm_sphere.vertex_stride);
        cl->SetIndexBuffer(*_gm_sphere.index_buffer);
        cl->DrawIndexed(_gm_sphere.index_count);

        // ---- 地面プレーン ----
        _shader.SetObject(Mat4::Translation(Vec3{0, -0.8f, 0}),
                          Vec3{0.4f, 0.45f, 0.5f});  // 灰青
        cl->SetVertexBuffer(*_gm_plane.vertex_buffer, _gm_plane.vertex_stride);
        cl->SetIndexBuffer(*_gm_plane.index_buffer);
        cl->DrawIndexed(_gm_plane.index_count);

        // ---- 周囲の 4 個のキューブ ----
        const Vec3 cube_colors[4] = {
            {0.9f, 0.3f, 0.3f},  // 赤
            {0.3f, 0.7f, 0.4f},  // 緑
            {0.3f, 0.5f, 0.9f},  // 青
            {0.8f, 0.4f, 0.8f},  // 紫
        };
        cl->SetVertexBuffer(*_gm_cube.vertex_buffer, _gm_cube.vertex_stride);
        cl->SetIndexBuffer(*_gm_cube.index_buffer);
        for (u32 i = 0; i < 4; ++i) {
            const f32 a = i * (kPi * 0.5f) + _angle * 0.3f;
            const f32 r = 2.5f;
            Vec3 pos{ Sin(a) * r, -0.2f, Cos(a) * r };
            Mat4 m = Mat4::RotationY(_angle * 1.2f) * Mat4::Translation(pos);
            _shader.SetObject(m, cube_colors[i]);
            cl->DrawIndexed(_gm_cube.index_count);
        }
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        // 非同期ロード進行中なら待ってから解放
        if (_async_mesh.Valid() && !_async_loaded) (void)_async_mesh.Wait();
        _gm_cube   = GpuMesh{};
        _gm_plane  = GpuMesh{};
        _gm_sphere = GpuMesh{};
        _shader.Shutdown();
    }

private:
    StandardShader _shader;
    GpuMesh _gm_sphere, _gm_plane, _gm_cube;
    AssetFuture _async_mesh;
    bool _async_loaded = false;

    Camera _camera;
    Vec3   _cam_pos{0, 1.5f, -5.0f};
    f32    _cam_yaw   = 0.0f;
    f32    _cam_pitch = 0.0f;
    f32    _angle     = 0.0f;
};

ACS_DEFINE_MAIN(HelloModel)
