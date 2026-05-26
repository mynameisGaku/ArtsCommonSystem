// SPDX-License-Identifier: Apache-2.0
// HelloLights — HelloLightsApp 実装。
#include "HelloLightsApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace hellolights {

void HelloLightsApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto cube   = Primitive::MakeCube(1.0f);
    auto sphere = Primitive::MakeSphere(0.5f, 32, 16);
    auto plane  = Primitive::MakePlane(20.0f, 20.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *cube,   _gm_cube));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));

    _scene.Build();

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);
    _cam_pos = Vec3{0, 3.0f, -8.0f};
}

void HelloLightsApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    _time += dt;

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

void HelloLightsApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    // シーン描画 (ライト計算 + 物体 + 光源可視化) は scene に委譲。
    _scene.Render(_shader, *cl, _camera, _gm_plane, _gm_cube, _gm_sphere, _time);

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

void HelloLightsApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _gm_plane  = GpuMesh{};
    _gm_sphere = GpuMesh{};
    _gm_cube   = GpuMesh{};
    _shader.Shutdown();
}

} // namespace hellolights
