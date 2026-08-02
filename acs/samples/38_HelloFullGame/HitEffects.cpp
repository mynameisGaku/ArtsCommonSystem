// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — CHitEffects 実装。
#include "HitEffects.h"
#include "GameplayScene.h"
#include "FullGameApp.h"

#include "render/SpriteBatch.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void CHitEffects::Init(CParticleEffectSystem& particles) noexcept {
    FParticleEmitterDef pdef{};
    pdef.color_start       = kParticleStart;
    pdef.color_end         = kParticleEnd;
    pdef.lifetime_sec      = 0.55f;
    pdef.emit_rate_per_sec = 0.0f;
    pdef.burst_count       = 16.0f;
    pdef.speed_min         = 1.5f;
    pdef.speed_max         = 4.5f;
    pdef.scale_start       = 0.3f;
    pdef.scale_end         = 0.0f;
    pdef.gravity           = FVec2{0.0f, 0.0f};
    m_HitEmitter = particles.CreateEmitter(pdef, FVec2{0.0f, 0.0f});
    // 連続発射はせず、Burst だけ使うので非アクティブで開始する。
    particles.SetEmitterActive(m_HitEmitter, false);
}

void CHitEffects::Shutdown(CParticleEffectSystem& particles) noexcept {
    particles.ClearAll();
}

void CHitEffects::Tick(AGameplayScene& scene, f32 dt) noexcept {
    m_Fx.Tick(dt);
    // CEffectSystem 内に溜まったトラウマを camera に流して消費する。
    if (m_Fx.PendingShakeTrauma() > 0.0f) {
        scene.Services().Camera().AddShake(m_Fx.PendingShakeTrauma());
        m_Fx.ConsumeShake();
    }
}

void CHitEffects::TriggerPlayerHurt(AGameplayScene& scene, FVec2 pos) noexcept {
    m_Fx.TriggerShake(0.9f);
    scene.GetParticles().SetEmitterPosition(m_HitEmitter, pos);
    scene.GetParticles().Burst(m_HitEmitter);
    static_cast<CFullGameApp&>(scene.GetGame()).Audio().PlaySfx("sfx_player_hurt", 1.0f);
}

void CHitEffects::TriggerEnemyHit(AGameplayScene& scene, FVec2 pos) noexcept {
    scene.GetParticles().SetEmitterPosition(m_HitEmitter, pos);
    scene.GetParticles().Burst(m_HitEmitter);
    m_Fx.TriggerShake(0.35f);
    m_Fx.Flash(FVec3{1.0f, 0.95f, 0.7f}, 0.25f, 0.08f);
    static_cast<CFullGameApp&>(scene.GetGame()).Audio().PlaySfx("sfx_enemy_hit", 0.8f);
}

void CHitEffects::DrawParticles(const CParticleEffectSystem& particles,
                                CSpriteBatch& sb) const noexcept {
    u32 n = 0;
    const FParticle* ps = particles.AllParticles(n);
    for (u32 i = 0; i < n; ++i) {
        const FParticle& p = ps[i];
        if (!p.IsAlive()) continue;
        // 正規化年齢で色 / スケール / α を線形補間する単純なシェーダなしレンダリング。
        const f32 t   = p.NormalizedAge();
        const f32 inv = 1.0f - t;
        const f32 scale = p.scale_start * inv + p.scale_end * t;
        const FVec3 col  = FVec3{
            p.color_start.x * inv + p.color_end.x * t,
            p.color_start.y * inv + p.color_end.y * t,
            p.color_start.z * inv + p.color_end.z * t,
        };
        const f32 alpha = inv;
        const f32 hs    = scale * 0.5f;
        sb.DrawRect(p.position.x - hs, p.position.y - hs, scale, scale,
                    FVec4{col.x, col.y, col.z, alpha});
    }
}

} // namespace hellofg
