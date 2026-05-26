// SPDX-License-Identifier: Apache-2.0
// HelloParticles — Application 実装。
#include "HelloParticlesApp.h"

#include "app/Sample.h"
#include "platform/Input.h"
#include "math/Math.h"
#include "math/Vec.h"
#include "foundation/Log.h"

using namespace acs;

namespace helloparticles {

namespace {

// 中央が明るい円型テクスチャ（パーティクルの粒）
void GenerateGlow(u8* out) noexcept {
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kTexSize * 0.5f;
            const f32 r = Sqrt(cx*cx + cy*cy) / (kTexSize * 0.5f);
            f32 a = 1.0f - r;
            if (a < 0) a = 0;
            // ガウシアンっぽいフォールオフで中央を強調
            a = a * a;
            const u8 byte_a = static_cast<u8>(a * 255);
            const usize i = static_cast<usize>(y * kTexSize + x) * 4;
            out[i+0] = 255; out[i+1] = 255; out[i+2] = 255; out[i+3] = byte_a;
        }
    }
}

} // namespace

void HelloParticlesApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    // SpriteBatch (2D 描画) と Glow テクスチャ (粒の見た目) を準備
    ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));

    // パーティクルの粒テクスチャを CPU 側で 1 枚だけ生成し GPU へ転送
    u8 px[kTexSize * kTexSize * 4];
    GenerateGlow(px);
    TextureDesc td{};
    td.width = kTexSize; td.height = kTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = px; td.initial_data_size = sizeof(px);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) { Quit(); return; }
    else _glow = Move(r.Value());

    // フォント (OS 別の標準フォント候補を Sample helper で解決。
    // 解像度 18px、atlas 1024px、CJK 対応 true。失敗しても HUD が消えるだけ)
    (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f, 1024, true);

    // 画面中央付近を初期エミッタ位置に
    if (!_scene.Init(_glow.Get(), FVec2{400, 400})) { Quit(); return; }

    ACS_LOG_INFO("HelloParticles initialized");
}

void HelloParticlesApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();
    _scene.Update(dt);
}

void HelloParticlesApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);
    // 暗めの背景（粒が映える）
    _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                    FVec4{0.05f, 0.06f, 0.10f, 1});

    _scene.Render(_batch, _font, sh, FPS());

    _batch.End();
}

void HelloParticlesApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _font.Shutdown();
    _scene.Shutdown();
    _glow.Reset();
    _batch.Shutdown();
}

} // namespace helloparticles
