// SPDX-License-Identifier: Apache-2.0
// 2D パーティクルシステム実装
#include "render/Particles.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"
#include "memory/Allocator.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "foundation/Move.h"

namespace acs {

// ===== プリセット ===========================================================

FEmitterDesc FEmitterDesc::Fire(FVec2 pos) noexcept {
    FEmitterDesc d{};
    d.position           = pos;
    d.spawn_offset_min   = FVec2{-6, 0};
    d.spawn_offset_max   = FVec2{ 6, 0};
    d.velocity           = FVec2{0, -120};
    d.velocity_variance  = FVec2{40, 30};
    d.gravity            = FVec2{0, -50};        // 上方向の浮力 (画面 Y は下が +)
    d.size_start         = 28.0f;
    d.size_end           = 0.0f;
    d.color_start        = FVec4{1.0f, 0.85f, 0.3f, 1.0f};   // 黄色
    d.color_end          = FVec4{0.4f, 0.05f, 0.0f, 0.0f};   // 暗赤、透明へ
    d.life_seconds       = 0.9f;
    d.life_variance      = 0.2f;
    d.rate_per_sec       = 250.0f;
    return d;
}

FEmitterDesc FEmitterDesc::Sparks(FVec2 pos) noexcept {
    FEmitterDesc d{};
    d.position           = pos;
    d.spawn_offset_min   = FVec2{0, 0};
    d.spawn_offset_max   = FVec2{0, 0};
    d.velocity           = FVec2{0, 0};
    d.velocity_variance  = FVec2{350, 350};       // 全方位に飛び散る
    d.gravity            = FVec2{0, 600};
    d.size_start         = 5.0f;
    d.size_end           = 1.0f;
    d.color_start        = FVec4{1.0f, 1.0f, 0.7f, 1.0f};
    d.color_end          = FVec4{1.0f, 0.4f, 0.1f, 0.0f};
    d.life_seconds       = 0.7f;
    d.life_variance      = 0.3f;
    d.rate_per_sec       = 80.0f;
    return d;
}

FEmitterDesc FEmitterDesc::Fountain(FVec2 pos) noexcept {
    FEmitterDesc d{};
    d.position           = pos;
    d.spawn_offset_min   = FVec2{-2, 0};
    d.spawn_offset_max   = FVec2{ 2, 0};
    d.velocity           = FVec2{0, -500};
    d.velocity_variance  = FVec2{120, 80};
    d.gravity            = FVec2{0, 1000};
    d.size_start         = 8.0f;
    d.size_end           = 4.0f;
    d.color_start        = FVec4{0.6f, 0.85f, 1.0f, 1.0f};
    d.color_end          = FVec4{0.3f, 0.5f, 0.95f, 0.0f};
    d.life_seconds       = 1.6f;
    d.life_variance      = 0.2f;
    d.rate_per_sec       = 300.0f;
    return d;
}

FEmitterDesc FEmitterDesc::Smoke(FVec2 pos) noexcept {
    FEmitterDesc d{};
    d.position           = pos;
    d.spawn_offset_min   = FVec2{-8, -2};
    d.spawn_offset_max   = FVec2{ 8,  2};
    d.velocity           = FVec2{0, -40};
    d.velocity_variance  = FVec2{20, 10};
    d.gravity            = FVec2{0, -10};
    d.size_start         = 16.0f;
    d.size_end           = 64.0f;
    d.color_start        = FVec4{0.6f, 0.6f, 0.6f, 0.5f};
    d.color_end          = FVec4{0.3f, 0.3f, 0.3f, 0.0f};
    d.life_seconds       = 2.5f;
    d.life_variance      = 0.4f;
    d.rate_per_sec       = 40.0f;
    return d;
}

// ===== FParticleSystem =======================================================

FParticleSystem::~FParticleSystem() noexcept {
    Shutdown();
}

TResult<void> FParticleSystem::Init(u32 max_particles) noexcept {
    if (max_particles == 0) max_particles = 1024;
    Shutdown();
    _capacity = max_particles;
    _active = 0;
    _spawn_accum = 0;
    _pool = static_cast<FParticle*>(
        DefaultAllocator().Alloc(sizeof(FParticle) * max_particles));
    if (!_pool) return ACS_ERR(Memory, 300, "FParticleSystem: pool alloc failed");
    return Ok();
}

void FParticleSystem::Shutdown() noexcept {
    if (_pool) {
        DefaultAllocator().Free(_pool);
        _pool = nullptr;
    }
    _capacity = 0;
    _active = 0;
}

f32 FParticleSystem::RandF() noexcept {
    _seed ^= _seed << 13; _seed ^= _seed >> 17; _seed ^= _seed << 5;
    return static_cast<f32>(_seed & 0xFFFFFFu) / 16777216.0f;
}

f32 FParticleSystem::RandRange(f32 a, f32 b) noexcept {
    return a + (b - a) * RandF();
}

void FParticleSystem::SpawnOne() noexcept {
    if (_active >= _capacity) return;
    FParticle& p = _pool[_active++];
    p.age = 0;
    // 寿命: ±life_variance の範囲でランダム
    p.life = _emitter.life_seconds + (RandF() * 2.0f - 1.0f) * _emitter.life_variance;
    if (p.life < 0.05f) p.life = 0.05f;
    // 出現位置 = エミッタ + spawn_offset 範囲
    p.pos.x = _emitter.position.x +
              RandRange(_emitter.spawn_offset_min.x, _emitter.spawn_offset_max.x);
    p.pos.y = _emitter.position.y +
              RandRange(_emitter.spawn_offset_min.y, _emitter.spawn_offset_max.y);
    // 速度 = base + variance
    p.vel.x = _emitter.velocity.x + (RandF() * 2 - 1) * _emitter.velocity_variance.x;
    p.vel.y = _emitter.velocity.y + (RandF() * 2 - 1) * _emitter.velocity_variance.y;
    p.size_start  = _emitter.size_start;
    p.size_end    = _emitter.size_end;
    p.color_start = _emitter.color_start;
    p.color_end   = _emitter.color_end;
}

void FParticleSystem::EmitBurst(u32 count) noexcept {
    if (!_pool) return;
    for (u32 i = 0; i < count; ++i) SpawnOne();
}

void FParticleSystem::Update(f32 dt) noexcept {
    if (!_pool) return;

    // 1) 連続生成
    if (_emitter.active && _emitter.rate_per_sec > 0) {
        _spawn_accum += dt * _emitter.rate_per_sec;
        const u32 n = static_cast<u32>(_spawn_accum);
        _spawn_accum -= static_cast<f32>(n);
        for (u32 i = 0; i < n; ++i) SpawnOne();
    }

    // 2) 物理積分 + 寿命管理（swap-pop で死亡粒子を除去）
    for (u32 i = 0; i < _active; ) {
        FParticle& p = _pool[i];
        p.vel.x += _emitter.gravity.x * dt;
        p.vel.y += _emitter.gravity.y * dt;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.age   += dt;
        if (p.age >= p.life) {
            // 死亡: 末尾と入れ替えて active を 1 減らす
            p = _pool[--_active];
            continue;
        }
        ++i;
    }
}

void FParticleSystem::Render(FSpriteBatch& sb) noexcept {
    if (!_pool || _active == 0) return;

    // テクスチャ未設定なら FSpriteBatch の DrawRect 相当（白矩形）
    // 設定済みなら DrawSub 経由
    for (u32 i = 0; i < _active; ++i) {
        const FParticle& p = _pool[i];
        const f32 t = p.age / p.life;        // 0 (誕生) .. 1 (死亡)
        // size と color を線形補間
        const f32 s = p.size_start + (p.size_end - p.size_start) * t;
        const f32 r = p.color_start.x + (p.color_end.x - p.color_start.x) * t;
        const f32 g = p.color_start.y + (p.color_end.y - p.color_start.y) * t;
        const f32 b = p.color_start.z + (p.color_end.z - p.color_start.z) * t;
        const f32 a = p.color_start.w + (p.color_end.w - p.color_start.w) * t;
        const f32 hs = s * 0.5f;
        if (_tex) {
            sb.Draw(*_tex, p.pos.x - hs, p.pos.y - hs, s, s, FVec4{r, g, b, a});
        } else {
            sb.DrawRect(p.pos.x - hs, p.pos.y - hs, s, s, FVec4{r, g, b, a});
        }
    }
}

} // namespace acs
