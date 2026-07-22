// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — FBullets 実装。
#include "Bullets.h"
#include "GameplayScene.h"
#include "FullGameApp.h"

#include "render/SpriteBatch.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void FBullets::Init(FProjectileSystem& sys) noexcept {
    sys.Init(kMaxBullets);

    FProjectileDef bdef{};
    bdef.id              = kBulletDefId;
    bdef.kind            = EProjectileKind::Bullet;
    bdef.speed           = kBulletSpeed;
    bdef.lifetime_sec    = kBulletLifetime;
    bdef.radius          = kBulletRadius;
    bdef.gravity_y       = 0.0f;
    bdef.pierces         = false;
    bdef.max_pierces     = 0;
    bdef.homing          = false;
    bdef.homing_strength = 0.0f;
    sys.RegisterDef(bdef);
}

void FBullets::Shutdown(FProjectileSystem& sys) noexcept {
    sys.ClearAll();
}

void FBullets::Fire(FGameplayScene& scene, FVec2 from, FVec2 dir_unit) noexcept {
    const FVec2 vel = dir_unit * kBulletSpeed;
    const FProjectileId pid = scene.GetProjectiles().Spawn(
        kBulletDefId, from, vel, /*owner_id=*/1u, kBulletDamage);
    if (!pid.IsValid()) return;
    static_cast<FFullGameApp&>(scene.GetGame()).Audio().PlaySfx("sfx_fire", 0.6f);
}

void FBullets::DrawAll(const FProjectileSystem& sys, FSpriteBatch& sb,
                      f32 last_dt) const noexcept {
    u32 n = 0;
    const FProjectileInstance* bs = sys.AllAlive(n);
    const f32 sz = kBulletRadius * 2.0f;

    // trail: 直前フレーム分の経路を kSteps 段の連続矩形で線化する。
    // 完璧な motion-blur ではないが、見た目に動きが出る軽量実装。
    const f32 trail_sec = last_dt * 1.2f;
    constexpr u32 kSteps = 32;

    for (u32 i = 0; i < n; ++i) {
        const FVec2 p = bs[i].position;
        const FVec2 v = bs[i].velocity;
        for (u32 s = 1; s <= kSteps; ++s) {
            const f32 t = static_cast<f32>(s) / static_cast<f32>(kSteps);
            const FVec2 tp = FVec2{ p.x - v.x * trail_sec * t,
                                   p.y - v.y * trail_sec * t };
            const f32 a = (1.0f - t) * 0.65f;
            sb.DrawRect(tp.x - kBulletRadius, tp.y - kBulletRadius, sz, sz,
                        FVec4{ kColorBullet.x, kColorBullet.y, kColorBullet.z,
                              kColorBullet.w * a });
        }
        sb.DrawRect(p.x - kBulletRadius, p.y - kBulletRadius, sz, sz, kColorBullet);
    }
}

} // namespace hellofg
