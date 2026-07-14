// SPDX-License-Identifier: Apache-2.0
// 2D パーティクルシステム実装
#include "render/Particles.h"
#include "render/SpriteBatch.h"
#include "render/IRhiTexture.h"
#include "memory/Allocator.h"
#include "foundation/Result.h"
#include "foundation/Error.h"
#include "foundation/Move.h"
#include "foundation/Assert.h"

namespace acs {

/** 火 (上昇する黄〜赤の炎) のプリセットを構成する。 */
EmitterDesc EmitterDesc::Fire(FVec2 pos) noexcept {
    EmitterDesc d{};
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

/** 火花 (全方位に飛び散り重力で落下) のプリセットを構成する。 */
EmitterDesc EmitterDesc::Sparks(FVec2 pos) noexcept {
    EmitterDesc d{};
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

/** 噴水 (上方へ噴き出し重力で放物落下する青い水滴) のプリセットを構成する。 */
EmitterDesc EmitterDesc::Fountain(FVec2 pos) noexcept {
    EmitterDesc d{};
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

/** 煙 (ゆっくり上昇しながら拡大する灰色) のプリセットを構成する。 */
EmitterDesc EmitterDesc::Smoke(FVec2 pos) noexcept {
    EmitterDesc d{};
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

/** プールを解放してから破棄する。 */
ParticleSystem::~ParticleSystem() noexcept {
    Shutdown();
}

/** max_particles ぶんのプールを確保する (0 は 1024 に丸める)。 */
TResult<void> ParticleSystem::Init(u32 max_particles) noexcept {
    if (max_particles == 0) max_particles = 1024;
    Shutdown();

    if (static_cast<usize>(max_particles) > (~usize(0)) / sizeof(Particle)) {
        return ACS_ERR(Memory, 300, "ParticleSystem: pool size overflow");
    }

    FAllocator& allocator = DefaultAllocator();
    Particle* const pool = static_cast<Particle*>(
        allocator.Alloc(sizeof(Particle) * static_cast<usize>(max_particles)));
    if (!pool) return ACS_ERR(Memory, 300, "ParticleSystem: pool alloc failed");

    // 成功した確保だけをコミットし、失敗時は常に空状態を保つ。
    m_Pool = pool;
    m_Allocator = &allocator;
    m_Capacity = max_particles;
    m_Active = 0;
    m_SpawnAccum = 0;
    return Ok();
}

/** プールを解放してカウンタをリセットする。 */
void ParticleSystem::Shutdown() noexcept {
    if (m_Pool) {
        ACS_ASSERT(m_Allocator != nullptr);
        m_Allocator->Free(m_Pool);
        m_Pool = nullptr;
    }
    m_Allocator = nullptr;
    m_Capacity = 0;
    m_Active = 0;
    m_SpawnAccum = 0.0f;
}

/** xorshift で [0,1) の擬似乱数を返す。 */
f32 ParticleSystem::RandF() noexcept {
    m_Seed ^= m_Seed << 13; m_Seed ^= m_Seed >> 17; m_Seed ^= m_Seed << 5;
    return static_cast<f32>(m_Seed & 0xFFFFFFu) / 16777216.0f;
}

/** [a,b) の一様乱数を返す。 */
f32 ParticleSystem::RandRange(f32 a, f32 b) noexcept {
    return a + (b - a) * RandF();
}

/** エミッタ記述に従って粒子を 1 つ生成する (容量上限なら何もしない)。 */
void ParticleSystem::SpawnOne() noexcept {
    if (m_Active >= m_Capacity) return;
    Particle& p = m_Pool[m_Active++];
    p.age = 0;
    // 寿命: ±life_variance の範囲でランダム
    p.life = m_Emitter.life_seconds + (RandF() * 2.0f - 1.0f) * m_Emitter.life_variance;
    if (p.life < 0.05f) p.life = 0.05f;
    // 出現位置 = エミッタ + spawn_offset 範囲
    p.pos.x = m_Emitter.position.x +
              RandRange(m_Emitter.spawn_offset_min.x, m_Emitter.spawn_offset_max.x);
    p.pos.y = m_Emitter.position.y +
              RandRange(m_Emitter.spawn_offset_min.y, m_Emitter.spawn_offset_max.y);
    // 速度 = base + variance
    p.vel.x = m_Emitter.velocity.x + (RandF() * 2 - 1) * m_Emitter.velocity_variance.x;
    p.vel.y = m_Emitter.velocity.y + (RandF() * 2 - 1) * m_Emitter.velocity_variance.y;
    p.size_start  = m_Emitter.size_start;
    p.size_end    = m_Emitter.size_end;
    p.color_start = m_Emitter.color_start;
    p.color_end   = m_Emitter.color_end;
}

/** count 個の粒子を即座に生成する。 */
void ParticleSystem::EmitBurst(u32 count) noexcept {
    if (!m_Pool) return;
    for (u32 i = 0; i < count; ++i) SpawnOne();
}

/** 1 フレーム分の連続生成 + 物理積分 + 寿命管理を進める。 */
void ParticleSystem::Update(f32 dt) noexcept {
    if (!m_Pool) return;

    // 1) 連続生成
    if (m_Emitter.active && m_Emitter.rate_per_sec > 0) {
        m_SpawnAccum += dt * m_Emitter.rate_per_sec;
        const u32 n = static_cast<u32>(m_SpawnAccum);
        m_SpawnAccum -= static_cast<f32>(n);
        for (u32 i = 0; i < n; ++i) SpawnOne();
    }

    // 2) 物理積分 + 寿命管理（swap-pop で死亡粒子を除去）
    for (u32 i = 0; i < m_Active; ) {
        Particle& p = m_Pool[i];
        p.vel.x += m_Emitter.gravity.x * dt;
        p.vel.y += m_Emitter.gravity.y * dt;
        p.pos.x += p.vel.x * dt;
        p.pos.y += p.vel.y * dt;
        p.age   += dt;
        if (p.age >= p.life) {
            // 死亡: 末尾と入れ替えて active を 1 減らす
            p = m_Pool[--m_Active];
            continue;
        }
        ++i;
    }
}

/** アクティブな粒子の size/color を寿命比で補間して FSpriteBatch に積む。 */
void ParticleSystem::Render(FSpriteBatch& sb) noexcept {
    if (!m_Pool || m_Active == 0) return;

    // テクスチャ未設定なら FSpriteBatch の DrawRect 相当（白矩形）
    // 設定済みなら DrawSub 経由
    for (u32 i = 0; i < m_Active; ++i) {
        const Particle& p = m_Pool[i];
        const f32 t = p.age / p.life;        // 0 (誕生) .. 1 (死亡)
        // size と color を線形補間
        const f32 s = p.size_start + (p.size_end - p.size_start) * t;
        const f32 r = p.color_start.x + (p.color_end.x - p.color_start.x) * t;
        const f32 g = p.color_start.y + (p.color_end.y - p.color_start.y) * t;
        const f32 b = p.color_start.z + (p.color_end.z - p.color_start.z) * t;
        const f32 a = p.color_start.w + (p.color_end.w - p.color_start.w) * t;
        const f32 hs = s * 0.5f;
        if (m_Tex) {
            sb.Draw(*m_Tex, p.pos.x - hs, p.pos.y - hs, s, s, FVec4{r, g, b, a});
        } else {
            sb.DrawRect(p.pos.x - hs, p.pos.y - hs, s, s, FVec4{r, g, b, a});
        }
    }
}

} // namespace acs
