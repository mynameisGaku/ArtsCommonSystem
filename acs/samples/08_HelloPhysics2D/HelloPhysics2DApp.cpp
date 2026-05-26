// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — Application 実装。
#include "HelloPhysics2DApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "math/Math.h"
#include "foundation/Log.h"

using namespace acs;

namespace hellophysics2d {

namespace {

// 中心が不透明白、外周にいくほどアルファが線形に落ちる円型テクスチャ。
// RGB を Ball::r/g/b で乗算するため、テクスチャ側は白固定にしておく。
// max_r を半径から 2px 引いて、エッジに 1〜2px の柔らかい透明帯を作る (aliasing 軽減)。
void GenerateBallTexture(u8* out) noexcept {
    for (u32 y = 0; y < kBallTexSize; ++y) {
        for (u32 x = 0; x < kBallTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kBallTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kBallTexSize * 0.5f;
            const f32 r2 = cx*cx + cy*cy;
            const f32 max_r = (kBallTexSize * 0.5f) - 2.0f;
            const f32 dist = Sqrt(r2) / max_r;
            f32 alpha = 1.0f - dist;
            if (alpha < 0) alpha = 0;
            if (alpha > 1) alpha = 1;
            const u8 R = 255, G = 255, B = 255;
            const u8 A = static_cast<u8>(alpha * 255);
            const usize i = static_cast<usize>(y * kBallTexSize + x) * 4;
            out[i+0] = R; out[i+1] = G; out[i+2] = B; out[i+3] = A;
        }
    }
}

} // namespace

void HelloPhysics2DApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));

    // フォントは OS 別の標準 UI フォント候補を Sample helper で解決。
    // 失敗時 (フォントが見つからない / ロード失敗) は HUD なしで続行する。
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f);

    u8 pixels[kBallTexSize * kBallTexSize * 4];
    GenerateBallTexture(pixels);
    TextureDesc td{};
    td.width = kBallTexSize; td.height = kBallTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = pixels;
    td.initial_data_size = sizeof(pixels);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) { Quit(); return; }
    else _tex = Move(r.Value());

    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();
    _scene.Init(sw, sh, 30);

    ACS_LOG_INFO("HelloPhysics2D initialized");
}

void HelloPhysics2DApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();

    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();
    _scene.Update(dt, sw, sh);
}

void HelloPhysics2DApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !_tex) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);
    _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.05f, 0.07f, 0.10f, 1});

    _scene.Render(_batch, _font, *_tex, FPS());

    _batch.End();
}

void HelloPhysics2DApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _tex.Reset();
    _batch.Shutdown();
}

} // namespace hellophysics2d
