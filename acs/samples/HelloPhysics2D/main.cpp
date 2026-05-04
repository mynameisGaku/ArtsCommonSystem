// ACS の HelloPhysics2D サンプル
//
// 動作:
//   ・複数の円ボールが画面内をバウンドし、ボール同士でも衝突
//   ・重力 + 速度減衰
//   ・マウス左クリックで新しいボールを発射
//   ・スペースで全ボールを削除
//   ・Esc 終了
//
// 学習ポイント:
//   ・math/Collision2D.h の Circle / Resolve / Intersect の使い方
//   ・SpriteBatch 上で簡易 2D 物理シミュレーション
//   ・ペネトレーション解消 + 反射ベクトル

#include "app/Application.h"
#include "app/EntryPoint.h"
#include "app/Sample.h"
#include "platform/Input.h"

#include "render/SpriteBatch.h"
#include "render/Font.h"
#include "render/IRhiTexture.h"

#include "math/Collision2D.h"
#include "math/Math.h"
#include "math/Vec.h"

#include "memory/UniquePtr.h"
#include "foundation/Log.h"

#include <cstdio>

using namespace acs;

namespace {

constexpr u32 kBallTexSize = 64;
constexpr u32 kMaxBalls    = 200;
constexpr f32 kGravity     = 600.0f;
constexpr f32 kRestitution = 0.85f;
constexpr f32 kDamping     = 0.999f;

struct Ball {
    Vec2 pos;
    Vec2 vel;
    f32  radius;
    f32  r, g, b;
};

// 中心が白、外周が透けてる円型テクスチャを生成
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

class HelloPhysics2D : public Application {
public:
    void OnStart() noexcept override {
        IRhiDevice* dev = GetRenderer().Device();
        if (!dev) { Quit(); return; }

        // SpriteBatch
        ACS_SAMPLE_INIT(_batch.Init(*dev, GetRenderer().ColorFormat()));

        // フォント (OS 別の標準フォント候補を Sample helper で解決)
        (void)Sample::TryLoadDefaultUIFont(_font, *dev, 18.0f);

        // ボールテクスチャ生成
        u8 pixels[kBallTexSize * kBallTexSize * 4];
        GenerateBallTexture(pixels);
        TextureDesc td{};
        td.width = kBallTexSize; td.height = kBallTexSize;
        td.format = Format::R8G8B8A8_UNorm;
        td.initial_data = pixels;
        td.initial_data_size = sizeof(pixels);
        if (auto r = CreateRhiTexture(*dev, td); r.IsErr()) { Quit(); return; }
        else _tex = Move(r.Value());

        // 初期ボール 30 個
        SpawnRandomBalls(30);

        ACS_LOG_INFO("HelloPhysics2D initialized");
    }

    void OnUpdate(f32 dt) noexcept override {
        if (Input::IsKeyPressed(Key::Escape)) Quit();
        if (Input::IsKeyPressed(Key::Space)) _ball_count = 0;

        if (Input::IsMouseButtonPressed(MouseButton::Left)) {
            Vec2 mp = Input::MousePos();
            SpawnBallAt(mp.x, mp.y);
        }

        // dt が大きすぎるとシミュが破綻するので clamp
        if (dt > 0.05f) dt = 0.05f;

        // 1. 速度・位置の更新（重力 + 減衰）
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        for (u32 i = 0; i < _ball_count; ++i) {
            Ball& b = _balls[i];
            b.vel.y += kGravity * dt;
            b.vel.x *= kDamping;
            b.vel.y *= kDamping;
            b.pos += b.vel * dt;

            // 壁との衝突（反射）
            if (b.pos.x - b.radius < 0)        { b.pos.x = b.radius;       b.vel.x = -b.vel.x * kRestitution; }
            if (b.pos.x + b.radius > sw)       { b.pos.x = sw - b.radius;  b.vel.x = -b.vel.x * kRestitution; }
            if (b.pos.y - b.radius < 0)        { b.pos.y = b.radius;       b.vel.y = -b.vel.y * kRestitution; }
            if (b.pos.y + b.radius > sh)       { b.pos.y = sh - b.radius;  b.vel.y = -b.vel.y * kRestitution; }
        }

        // 2. ボール間の衝突を解消（ペネトレーション + 反射）
        for (u32 i = 0; i < _ball_count; ++i) {
            for (u32 j = i + 1; j < _ball_count; ++j) {
                Ball& a = _balls[i];
                Ball& bb = _balls[j];
                Circle ca{ a.pos, a.radius };
                Circle cb{ bb.pos, bb.radius };
                Vec2 push;
                if (Resolve(ca, cb, push)) {
                    // 等質量と仮定して半分ずつ押し出す
                    a.pos.x  += push.x * 0.5f; a.pos.y  += push.y * 0.5f;
                    bb.pos.x -= push.x * 0.5f; bb.pos.y -= push.y * 0.5f;
                    // 法線方向への速度を交換（弾性衝突簡易版）
                    const f32 plen2 = push.x * push.x + push.y * push.y;
                    if (plen2 > 1e-6f) {
                        const f32 inv_len = 1.0f / Sqrt(plen2);
                        const f32 nx = push.x * inv_len;
                        const f32 ny = push.y * inv_len;
                        const f32 va = a.vel.x  * nx + a.vel.y  * ny;
                        const f32 vb = bb.vel.x * nx + bb.vel.y * ny;
                        const f32 dv = (va - vb) * kRestitution;
                        a.vel.x  -= dv * nx; a.vel.y  -= dv * ny;
                        bb.vel.x += dv * nx; bb.vel.y += dv * ny;
                    }
                }
            }
        }
    }

    void OnRender() noexcept override {
        IRhiCommandList* cl = GetRenderer().CommandList();
        if (!cl || !_tex) return;
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();

        _batch.Begin(*cl, sw, sh);
        // 背景
        _batch.DrawRect(0, 0, static_cast<f32>(sw), static_cast<f32>(sh),
                        Vec4{0.05f, 0.07f, 0.10f, 1});

        // ボール
        for (u32 i = 0; i < _ball_count; ++i) {
            const Ball& b = _balls[i];
            const f32 d = b.radius * 2.0f;
            _batch.Draw(*_tex, b.pos.x - b.radius, b.pos.y - b.radius, d, d,
                        Vec4{b.r, b.g, b.b, 0.95f});
        }

        // ヘルプ + ボール数
        if (_font.AtlasTexture()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                          "balls: %u / %u   FPS: %.1f", _ball_count, kMaxBalls, FPS());
            _batch.DrawString(_font, buf, 20, 20, Vec4{1,1,1,1});
            _batch.DrawString(_font,
                            "左クリック: ボール追加  Space: 全消去  Esc: 終了",
                            20, 44, Vec4{0.7f, 0.8f, 0.9f, 1});
        }
        _batch.End();
    }

    void OnShutdown() noexcept override {
        if (GetRenderer().Device()) GetRenderer().Device()->WaitIdle();
        _font.Shutdown();
        _tex.Reset();
        _batch.Shutdown();
    }

private:
    void SpawnRandomBalls(u32 n) noexcept {
        const u32 sw = GetRenderer().Swapchain()->Width();
        const u32 sh = GetRenderer().Swapchain()->Height();
        for (u32 i = 0; i < n; ++i) {
            SpawnBallAt(_rand_f() * sw, _rand_f() * (sh * 0.5f));
        }
    }

    void SpawnBallAt(f32 x, f32 y) noexcept {
        if (_ball_count >= kMaxBalls) return;
        Ball& b = _balls[_ball_count++];
        b.pos = { x, y };
        b.vel = { (_rand_f() - 0.5f) * 600.0f, (_rand_f() - 0.5f) * 400.0f };
        b.radius = 12.0f + _rand_f() * 24.0f;
        b.r = 0.4f + _rand_f() * 0.6f;
        b.g = 0.4f + _rand_f() * 0.6f;
        b.b = 0.4f + _rand_f() * 0.6f;
    }

    // xorshift32 簡易乱数
    f32 _rand_f() noexcept {
        _seed ^= _seed << 13; _seed ^= _seed >> 17; _seed ^= _seed << 5;
        return static_cast<f32>(_seed & 0xFFFFFFu) / 16777216.0f;
    }

    SpriteBatch _batch;
    Font _font;
    UniquePtr<IRhiTexture> _tex;
    Ball _balls[kMaxBalls] {};
    u32  _ball_count = 0;
    u32  _seed = 0xCAFEBABEu;
};

ACS_DEFINE_MAIN(HelloPhysics2D)
