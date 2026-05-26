// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — HitEffects 実装。
#include "HitEffects.h"
#include "GameplayScene.h"
#include "FullGameApp.h"

#include "render/SpriteBatch.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void HitEffects::Init(ParticleEffectSystem& particles) noexcept {
    ParticleEmitterDef pdef{};
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
    _hit_emitter = particles.CreateEmitter(pdef, FVec2{0.0f, 0.0f});
    // 連続発射はせず、Burst だけ使うので非アクティブで開始する。
    particles.SetEmitterActive(_hit_emitter, false);
}

void HitEffects::Shutdown(ParticleEffectSystem& particles) noexcept {
    particles.ClearAll();
}

void HitEffects::Tick(GameplayScene& scene, f32 dt) noexcept {
    _fx.Tick(dt);
    // EffectSystem 内に溜まったトラウマを camera に流して消費する。
    if (_fx.PendingShakeTrauma() > 0.0f) {
        scene.Services().Camera().AddShake(_fx.PendingShakeTrauma());
        _fx.ConsumeShake();
    }
}

void HitEffects::TriggerPlayerHurt(GameplayScene& scene, FVec2 pos) noexcept {
    _fx.TriggerShake(0.9f);
    scene.GetParticles().SetEmitterPosition(_hit_emitter, pos);
    scene.GetParticles().Burst(_hit_emitter);
    static_cast<FullGameApp&>(scene.GetGame()).Audio().PlaySfx("sfx_player_hurt", 1.0f);
}

void HitEffects::TriggerEnemyHit(GameplayScene& scene, FVec2 pos) noexcept {
    scene.GetParticles().SetEmitterPosition(_hit_emitter, pos);
    scene.GetParticles().Burst(_hit_emitter);
    _fx.TriggerShake(0.35f);
    _fx.Flash(FVec3{1.0f, 0.95f, 0.7f}, 0.25f, 0.08f);
    static_cast<FullGameApp&>(scene.GetGame()).Audio().PlaySfx("sfx_enemy_hit", 0.8f);
}

void HitEffects::DrawParticles(const ParticleEffectSystem& particles,
                                SpriteBatch& sb) const noexcept {
    u32 n = 0;
    const Particle* ps = particles.AllParticles(n);
    for (u32 i = 0; i < n; ++i) {
        const Particle& p = ps[i];
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
