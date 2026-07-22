// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — PhysicsScene 実装。
#include "PhysicsScene.h"

#include "platform/Input.h"
#include "math/Collision2D.h"
#include "math/Math.h"

#include <cstdio>

using namespace acs;

namespace hellophysics2d {

void FPhysicsScene::Init(u32 screen_w, u32 screen_h, u32 initial_balls) noexcept {
    SpawnRandomBalls(initial_balls, screen_w, screen_h);
}

void FPhysicsScene::Update(f32 dt, u32 screen_w, u32 screen_h) noexcept {
    if (FInput::IsKeyPressed(EKey::Space)) m_BallCount = 0;

    if (FInput::IsMouseButtonPressed(EMouseButton::Left)) {
        FVec2 mp = FInput::MousePos();
        SpawnBallAt(mp.x, mp.y);
    }

    // dt が大きすぎるとシミュが破綻するので clamp
    if (dt > 0.05f) dt = 0.05f;

    // 1. 速度・位置の更新（重力 + 減衰）
    const f32 sw = static_cast<f32>(screen_w);
    const f32 sh = static_cast<f32>(screen_h);
    for (u32 i = 0; i < m_BallCount; ++i) {
        FBall& b = m_Balls[i];
        b.vel.y += kGravity * dt;
        b.vel.x *= kDamping;
        b.vel.y *= kDamping;
        b.pos += b.vel * dt;

        // 壁との衝突（反射）
        if (b.pos.x - b.radius < 0)  { b.pos.x = b.radius;       b.vel.x = -b.vel.x * kRestitution; }
        if (b.pos.x + b.radius > sw) { b.pos.x = sw - b.radius;  b.vel.x = -b.vel.x * kRestitution; }
        if (b.pos.y - b.radius < 0)  { b.pos.y = b.radius;       b.vel.y = -b.vel.y * kRestitution; }
        if (b.pos.y + b.radius > sh) { b.pos.y = sh - b.radius;  b.vel.y = -b.vel.y * kRestitution; }
    }

    // 2. ボール間の衝突を解消（ペネトレーション + 反射）
    for (u32 i = 0; i < m_BallCount; ++i) {
        for (u32 j = i + 1; j < m_BallCount; ++j) {
            FBall& a = m_Balls[i];
            FBall& bb = m_Balls[j];
            FCircle ca{ a.pos, a.radius };
            FCircle cb{ bb.pos, bb.radius };
            FVec2 push;
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

void FPhysicsScene::Render(FSpriteBatch& batch,
                          FFont& font,
                          IRhiTexture& ball_tex,
                          f32 fps) noexcept {
    for (u32 i = 0; i < m_BallCount; ++i) {
        const FBall& b = m_Balls[i];
        const f32 d = b.radius * 2.0f;
        batch.Draw(ball_tex, b.pos.x - b.radius, b.pos.y - b.radius, d, d,
                   FVec4{b.r, b.g, b.b, 0.95f});
    }

    // フォント未ロード時は HUD を skip (FSample::TryLoadDefaultUIFont が失敗するケース)。
    if (font.AtlasTexture()) {
        char buf[128];
        std::snprintf(buf, sizeof(buf),
                      "balls: %u / %u   FPS: %.1f", m_BallCount, kMaxBalls, fps);
        batch.DrawString(font, buf, 20, 20, FVec4{1,1,1,1});
        batch.DrawString(font,
                        "左クリック: ボール追加  Space: 全消去  Esc: 終了",
                        20, 44, FVec4{0.7f, 0.8f, 0.9f, 1});
    }
}

void FPhysicsScene::SpawnRandomBalls(u32 n, u32 screen_w, u32 screen_h) noexcept {
    const f32 sw = static_cast<f32>(screen_w);
    const f32 sh = static_cast<f32>(screen_h);
    for (u32 i = 0; i < n; ++i) {
        SpawnBallAt(RandF() * sw, RandF() * (sh * 0.5f));
    }
}

void FPhysicsScene::SpawnBallAt(f32 x, f32 y) noexcept {
    if (m_BallCount >= kMaxBalls) return;
    FBall& b = m_Balls[m_BallCount++];
    b.pos = { x, y };
    b.vel = { (RandF() - 0.5f) * 600.0f, (RandF() - 0.5f) * 400.0f };
    b.radius = 12.0f + RandF() * 24.0f;
    b.r = 0.4f + RandF() * 0.6f;
    b.g = 0.4f + RandF() * 0.6f;
    b.b = 0.4f + RandF() * 0.6f;
}

f32 FPhysicsScene::RandF() noexcept {
    m_Seed ^= m_Seed << 13; m_Seed ^= m_Seed >> 17; m_Seed ^= m_Seed << 5;
    return static_cast<f32>(m_Seed & 0xFFFFFFu) / 16777216.0f;
}

} // namespace hellophysics2d
