// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — WaveCallbacks 実装。
#include "WaveCallbacks.h"
#include "GameplayScene.h"
#include "Enemy.h"

#include "foundation/Log.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void WaveOnSpawn(void* user, const char* /*enemy_id*/, Vec2 /*ignored_pos*/) noexcept {
    auto* scene = static_cast<GameplayScene*>(user);

    // 4 辺のうちランダムに 1 辺選び、その辺上のランダム位置に湧かせる。
    // margin は world エッジから余白を取って画面外湧きすぎないようにするため。
    const u32 edge = static_cast<u32>(scene->GetRng().RangeInt(0, 3));
    Vec2 pos{0.0f, 0.0f};
    const f32 margin = 1.0f;
    switch (edge) {
    case 0:
        pos.x = scene->GetRng().RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y =  kWorldHalfH - margin;
        break;
    case 1:
        pos.x = scene->GetRng().RangeF32(-kWorldHalfW + margin, kWorldHalfW - margin);
        pos.y = -kWorldHalfH + margin;
        break;
    case 2:
        pos.x = -kWorldHalfW + margin;
        pos.y = scene->GetRng().RangeF32(-kWorldHalfH + margin, kWorldHalfH - margin);
        break;
    default:
        pos.x =  kWorldHalfW - margin;
        pos.y = scene->GetRng().RangeF32(-kWorldHalfH + margin, kWorldHalfH - margin);
        break;
    }
    scene->GetEnemies().Spawn(*scene, scene->GetRoot(), scene->GetHealth(),
                              scene->GetWaves().CurrentWaveIndex(), pos);
}

void WaveOnState(void* user, u32 wave_index,
                 EWaveState /*from*/, EWaveState to) noexcept {
    auto* scene = static_cast<GameplayScene*>(user);
    if (to == EWaveState::AllComplete) {
        ACS_LOG_INFO("[Gameplay] all %u waves cleared! VICTORY!", wave_index + 1u);
        scene->RequestGameOver(/*victory=*/true);
    } else if (to == EWaveState::Cleared) {
        ACS_LOG_INFO("[Gameplay] wave %u cleared", wave_index);
    } else if (to == EWaveState::Spawning) {
        ACS_LOG_INFO("[Gameplay] wave %u start", wave_index);
    }
}

bool ProjectileOnHitTest(void* user, const ProjectileInstance& p,
                         u32& out_target, f32& out_dmg) noexcept {
    auto* scene = static_cast<GameplayScene*>(user);
    const EnemyInstance* enemies = scene->GetEnemies().Data();

    // 敵プールを線形に走査。最初にヒットした index を返す (= 弾は 1 体しか抜かない)。
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const EnemyInstance& e = enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const Vec2 ep = e.node->Local().position;
        const f32 dx = p.position.x - ep.x;
        const f32 dy = p.position.y - ep.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kEnemyRadius + kBulletRadius;
        if (d2 <= r * r) {
            out_target = i;
            out_dmg    = p.damage;
            return true;
        }
    }
    return false;
}

void ProjectileOnHit(void* user, ProjectileId /*pid*/, const char* /*def_id*/,
                     u32 target_id, f32 dmg) noexcept {
    auto* scene = static_cast<GameplayScene*>(user);
    scene->GetEnemies().ApplyHit(*scene, scene->GetHealth(), target_id, dmg);
}

} // namespace hellofg
