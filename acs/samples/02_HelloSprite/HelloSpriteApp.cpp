// SPDX-License-Identifier: Apache-2.0
// HelloSprite — HelloSpriteApp 実装。
#include "HelloSpriteApp.h"

#include "platform/Input.h"
#include "foundation/Log.h"

namespace hellosprite {

using namespace acs;

void GenerateSpriteTexture(u8* out) noexcept {
    for (u32 y = 0; y < kTexSize; ++y) {
        for (u32 x = 0; x < kTexSize; ++x) {
            const f32 cx = static_cast<f32>(x) - kTexSize * 0.5f;
            const f32 cy = static_cast<f32>(y) - kTexSize * 0.5f;
            const f32 r2 = cx*cx + cy*cy;
            const f32 max_r = (kTexSize * 0.5f) - 1.0f;
            const f32 dist = r2 / (max_r * max_r);

            u8 R, G, B, A;
            if (dist > 1.0f) {
                R = G = B = A = 0;   // 円外は透明
            } else {
                const f32 t = 1.0f - dist;
                R = static_cast<u8>(220 * t + 30);
                G = static_cast<u8>(80  * t + 50);
                B = static_cast<u8>(220 * (1.0f - t) + 50);
                A = static_cast<u8>(255 * t);
            }
            const usize i = static_cast<usize>(y * kTexSize + x) * 4;
            out[i+0] = R; out[i+1] = G; out[i+2] = B; out[i+3] = A;
        }
    }
}

void HelloSpriteApp::OnStart() noexcept {
    IRhiDevice* dev = GetRenderer().Device();
    if (!dev) { Quit(); return; }

    if (auto r = _batch.Init(*dev, GetRenderer().ColorFormat(), kMaxSprites);
        r.IsErr()) {
        ACS_LOG_ERROR("SpriteBatch::Init failed: %s", r.Error().message);
        Quit();
        return;
    }

    u8 pixels[kTexSize * kTexSize * 4];
    GenerateSpriteTexture(pixels);
    TextureDesc td{};
    td.width = kTexSize; td.height = kTexSize;
    td.format = EFormat::R8G8B8A8_UNorm;
    td.initial_data = pixels;
    td.initial_data_size = sizeof(pixels);
    if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) {
        ACS_LOG_ERROR("Texture create"); Quit(); return;
    } else _tex = Move(r.Value());

    SpawnSprites(kInitialSprites);

    ACS_LOG_INFO("HelloSprite initialized");
}

void HelloSpriteApp::OnUpdate(f32 dt) noexcept {
    if (Input::IsKeyPressed(EKey::Escape)) Quit();

    if (Input::IsKeyPressed(EKey::Space))     SpawnSprites(_sprite_count + 10);
    if (Input::IsKeyPressed(EKey::Backspace)) SpawnSprites(_sprite_count >= 10 ? _sprite_count - 10 : 0);

    // 壁にぶつかったら反射 (s.x/s.y を境界内にクランプしてから速度反転)。
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();
    for (u32 i = 0; i < _sprite_count; ++i) {
        Sprite& s = _sprites[i];
        s.x += s.vx * dt;
        s.y += s.vy * dt;
        if (s.x < 0)              { s.x = 0;              s.vx = -s.vx; }
        if (s.x + s.size > sw)    { s.x = sw - s.size;    s.vx = -s.vx; }
        if (s.y < 0)              { s.y = 0;              s.vy = -s.vy; }
        if (s.y + s.size > sh)    { s.y = sh - s.size;    s.vy = -s.vy; }
    }
}

void HelloSpriteApp::OnRender() noexcept {
    IRhiCommandList* cl = GetRenderer().CommandList();
    if (!cl || !_tex) return;
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();

    _batch.Begin(*cl, sw, sh);

    _batch.DrawRect(0, 0, static_cast<f32>(sw), 32.0f, Vec4{0, 0, 0, 0.6f});

    for (u32 i = 0; i < _sprite_count; ++i) {
        const Sprite& s = _sprites[i];
        _batch.Draw(*_tex, s.x, s.y, s.size, s.size, Vec4{s.r, s.g, s.b, s.a});
    }

    _batch.DrawRect(0, static_cast<f32>(sh - 16), static_cast<f32>(sw), 16.0f,
                    Vec4{0.2f, 0.5f, 0.8f, 0.5f});

    _batch.End();
}

void HelloSpriteApp::OnShutdown() noexcept {
    if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
    _tex.Reset();
    _batch.Shutdown();
}

void HelloSpriteApp::SpawnSprites(u32 n) noexcept {
    if (n > kMaxSprites) n = kMaxSprites;
    // 簡易擬似乱数 (xorshift)
    u32 seed = 12345 + n;
    auto rand_u = [&]() -> u32 {
        seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
        return seed;
    };
    auto rand_f = [&]() -> f32 {
        return static_cast<f32>(rand_u() & 0xFFFFFFu) / 16777216.0f;
    };
    const u32 sw = GetRenderer().Swapchain()->Width();
    const u32 sh = GetRenderer().Swapchain()->Height();
    const u32 old = _sprite_count;
    _sprite_count = n;
    for (u32 i = old; i < n; ++i) {
        Sprite& s = _sprites[i];
        s.size = 16.0f + rand_f() * 48.0f;
        s.x = rand_f() * (sw - s.size);
        s.y = 32.0f + rand_f() * (sh - 64.0f - s.size);
        const f32 angle = rand_f() * 6.28318f;
        const f32 speed = 80.0f + rand_f() * 200.0f;
        s.vx = speed * (rand_f() > 0.5f ? 1.0f : -1.0f) * (0.5f + rand_f() * 0.5f);
        s.vy = speed * (rand_f() > 0.5f ? 1.0f : -1.0f) * (0.5f + rand_f() * 0.5f);
        s.r = 0.5f + rand_f() * 0.5f;
        s.g = 0.5f + rand_f() * 0.5f;
        s.b = 0.5f + rand_f() * 0.5f;
        s.a = 0.7f + rand_f() * 0.3f;
    }
}

} // namespace hellosprite
