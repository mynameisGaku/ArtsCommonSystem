// SPDX-License-Identifier: Apache-2.0
// HelloSky — HelloSkyApp 実装。
#include "HelloSkyApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

using namespace acs;

namespace hellosky {

void HelloSkyApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    _scene.SetPreset(_sky, SkyPreset::Day);

    ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    auto sphere = Primitive::MakeSphere(1.0f, 48, 24);
    auto plane  = Primitive::MakePlane(50.0f, 50.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *sphere, _gm_sphere));
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane,  _gm_plane));

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 200.0f);
    _cam_pos = Vec3{0, 2.0f, -6.0f};

    ACS_LOG_INFO("HelloSky initialized");
}

void HelloSkyApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    if (Input::IsKeyPressed(EKey::Num1)) _scene.SetPreset(_sky, SkyPreset::Day);
    if (Input::IsKeyPressed(EKey::Num2)) _scene.SetPreset(_sky, SkyPreset::Sunset);
    if (Input::IsKeyPressed(EKey::Num3)) _scene.SetPreset(_sky, SkyPreset::Night);

    _angle += dt * 0.5f;

    // ターゲット (原点上 1m) を中心にカメラを yaw 周回させる。
    // Sin/Cos で円軌道を作るのが一番素直で、初学者が読みやすい形。
    if (Input::IsKeyDown(EKey::Left))  _cam_yaw -= dt * 1.0f;
    if (Input::IsKeyDown(EKey::Right)) _cam_yaw += dt * 1.0f;
    const f32 cam_dist = 6.0f;
    _cam_pos = Vec3{ Sin(_cam_yaw) * cam_dist, 2.5f, -Cos(_cam_yaw) * cam_dist };
    _camera.SetLookAt(_cam_pos, Vec3{0, 1, 0});
}

void HelloSkyApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    _scene.Render(_sky, _shader, *cl, _camera, _gm_plane, _gm_sphere, _angle);

    // HUD は AtlasTexture が用意できているときだけ。フォントロード失敗
    // (アセット不在) でもサンプル自体は動くようにフェイルセーフ。
    if (_font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        _batch.Begin(*cl, sw, sh);
        const SkyPreset cur = _scene.CurrentPreset();
        const char* preset_name = (cur == SkyPreset::Day)    ? "[1] 昼"     :
                                  (cur == SkyPreset::Sunset) ? "[2] 夕焼け" : "[3] 夜";
        _batch.DrawString(_font, preset_name, 20, 20, Vec4{1,1,1,1});
        _batch.DrawString(_font, "1/2/3: プリセット切替  ←→: カメラ  Esc: 終了",
                        20, 44, Vec4{0.8f,0.85f,0.95f,1});
        _batch.End();
    }
}

void HelloSkyApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _gm_plane  = GpuMesh{};
    _gm_sphere = GpuMesh{};
    _shader.Shutdown();
    _sky.Shutdown();
}

} // namespace hellosky
