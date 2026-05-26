// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — HelloAnimationApp 実装。
#include "HelloAnimationApp.h"

#include "app/Sample.h"
#include "asset/MeshAsset.h"
#include "asset/MeshPrimitive.h"
#include "foundation/Log.h"
#include "math/Math.h"
#include "platform/Input.h"
#include "render/IRhiSwapchain.h"

#include <cstdio>

using namespace acs;

namespace helloanim {

void HelloAnimationApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_sky.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    _sky.PresetSunset();

    ACS_SAMPLE_INIT(_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));
    ACS_SAMPLE_INIT(_std_shader.Init(*dev, GetRenderer().ColorFormat(), GetRenderer().DepthFormat()));

    // 地面用プレーン
    auto plane = Primitive::MakePlane(40.0f, 40.0f);
    ACS_SAMPLE_INIT(UploadMesh(*dev, *plane, _gm_plane));

    // スキンメッシュ生成 + GPU アップロード
    _snake = AnimationScene::BuildSnake();
    if (!_snake) { Quit(); return; }
    ACS_SAMPLE_INIT(UploadSkinnedMesh(*dev, *_snake, _gm_snake));
    _player.SetMesh(_snake.Get());
    _player.Play(0, /*loop=*/true);

    // 2D HUD
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    const f32 aspect = static_cast<f32>(GetRenderer().Swapchain()->Width()) /
                       static_cast<f32>(GetRenderer().Swapchain()->Height());
    _camera.SetPerspective(60.0f * kDeg2Rad, aspect, 0.1f, 100.0f);

    ACS_LOG_INFO("HelloAnimation initialized: %u verts, %u indices, %u bones",
                 static_cast<u32>(_snake->Vertices().Size()),
                 static_cast<u32>(_snake->Indices().Size()),
                 static_cast<u32>(_snake->Bones().Size()));
}

void HelloAnimationApp::OnUpdate(f32 dt) noexcept {
    if (FInput::IsKeyPressed(EKey::Escape)) Quit();
    if (FInput::IsKeyPressed(EKey::Space)) {
        if (_player.IsPlaying()) _player.Pause(); else _player.Resume();
    }

    if (FInput::IsKeyDown(EKey::Left))  _cam_yaw -= dt * 1.0f;
    if (FInput::IsKeyDown(EKey::Right)) _cam_yaw += dt * 1.0f;
    const f32 cam_dist = 8.0f;
    _cam_pos = FVec3{ Sin(_cam_yaw) * cam_dist, 3.0f, -Cos(_cam_yaw) * cam_dist };
    _camera.SetLookAt(_cam_pos, FVec3{0, 2, 0});

    _player.Update(dt);
}

void HelloAnimationApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;

    // ボーンパレットを書き出してから scene に委譲。
    FMat4 palette[FSkinnedShader::kMaxBones];
    const u32 nb = _player.WritePalette(palette, FSkinnedShader::kMaxBones);

    // FSky → 地面 → スキンメッシュ描画は scene に委譲。
    _scene.Render(_sky, _std_shader, _shader, *cl, _camera,
                  _gm_plane, _gm_snake, palette, nb);

    // 4. HUD
    if (_font.AtlasTexture()) {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        _batch.Begin(*cl, sw, sh);
        char buf[160];
        std::snprintf(buf, sizeof(buf),
                      "ボーン: %u  時刻: %.2fs  状態: %s",
                      nb, static_cast<double>(_player.Time()),
                      _player.IsPlaying() ? "再生中" : "停止");
        _batch.DrawString(_font, buf, 20, 20, FVec4{1,1,1,1});
        _batch.DrawString(_font, "Space: 再生/停止  ←→: カメラ  Esc: 終了",
                        20, 44, FVec4{0.8f,0.85f,0.95f,1});
        _batch.End();
    }
}

void HelloAnimationApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _batch.Shutdown();
    _gm_snake = FSkinnedGpuMesh{};
    _gm_plane = FGpuMesh{};
    _snake    = TRc<FSkinnedMeshAsset>();
    _std_shader.Shutdown();
    _shader.Shutdown();
    _sky.Shutdown();
}

} // namespace helloanim
