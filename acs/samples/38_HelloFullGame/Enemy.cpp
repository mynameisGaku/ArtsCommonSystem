// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Enemy 実装。
#include "Enemy.h"
#include "GameplayScene.h"
#include "HitEffects.h"

#include "render/SpriteBatch.h"
#include "foundation/Log.h"
#include "math/Math.h"

using namespace acs;
using namespace acs::game;

namespace hellofg {

void EnemyPool::Reset() noexcept {
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        _enemies[i] = EnemyInstance{};
    }
}

void EnemyPool::Shutdown() noexcept {
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        if (_enemies[i].alive && _enemies[i].node) {
            _enemies[i].node->Destroy();
        }
        _enemies[i] = EnemyInstance{};
    }
}

void EnemyPool::Spawn(GameplayScene& scene, Node2D& root, HealthSystem& health,
                     u32 current_wave, FVec2 pos) noexcept {
    u32 slot = kMaxEnemies;
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        if (!_enemies[i].alive) { slot = i; break; }
    }
    if (slot >= kMaxEnemies) {
        ACS_LOG_WARN("[Gameplay] enemy pool full, drop spawn");
        return;
    }

    auto up = MakeUnique<Node2D>();
    up->Local().position = pos;
    Node2D& nref = root.AddChild(Move(up));
    // FNodeId は 100 オフセットで衝突回避 (player=1)。
    nref._SetId(FNodeId{slot + 100u, static_cast<u8>(1)});

    EnemyInstance& e = _enemies[slot];
    e.alive = true;
    e.node  = &nref;
    e.hp    = health.Spawn(kEnemyHp);
    e.shape = scene.Services().Physics().AddCircle(Circle{pos, kEnemyRadius});
    e.wave_idx_at_spawn = current_wave;
}

bool EnemyPool::TickChaseAndContact(GameplayScene& scene, HealthSystem& health,
                                    FVec2 player_pos, f32 dt) noexcept {
    CollisionWorld2D& phy = scene.Services().Physics();
    bool any_contact_lethal = false;

    for (u32 i = 0; i < kMaxEnemies; ++i) {
        EnemyInstance& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;

        // プレイヤーへ単純追跡。AI は方向ベクトル正規化 → 等速移動のみ。
        FVec2 ep   = e.node->Local().position;
        const FVec2 dir = Normalize(player_pos - ep);
        if (LengthSq(dir) > 0.0f) {
            ep = ep + dir * (kEnemySpeed * dt);
            e.node->Local().position = ep;
            phy.UpdateCircle(e.shape, Circle{ep, kEnemyRadius});
        }

        // 円 vs 円の接触判定。
        const f32 dx = ep.x - player_pos.x;
        const f32 dy = ep.y - player_pos.y;
        const f32 d2 = dx*dx + dy*dy;
        const f32 r  = kPlayerRadius + kEnemyRadius;
        if (d2 <= r * r) {
            // player 側で無敵チェック + ダメージ + フィードバックを処理させる。
            // 接触が致死だったらシーンに伝えて GameOver させる。
            const bool lethal = scene.GetPlayer().TryTakeContactDamage(scene, health, player_pos);
            if (lethal) {
                any_contact_lethal = true;
                break;
            }
        }
    }
    return any_contact_lethal;
}

void EnemyPool::ApplyHit(GameplayScene& scene, HealthSystem& health,
                         u32 target_id, f32 dmg) noexcept {
    if (target_id >= kMaxEnemies) return;
    EnemyInstance& e = _enemies[target_id];
    if (!e.alive) return;

    const bool lethal = health.ApplyDamage(e.hp, dmg, EDamageType::Physical);
    const FVec2 ep = e.node ? e.node->Local().position : FVec2{0.0f, 0.0f};

    // 死亡 / 非死亡どちらでも被弾エフェクトは出す。死亡ならスコア + Wave 通知も。
    scene.GetHitEffects().TriggerEnemyHit(scene, ep);

    if (lethal) {
        scene.OnEnemyKilled();   // score + wave notify を scene 側で集約。
        health.Despawn(e.hp);
        if (scene.HasServices()) scene.Services().Physics().Remove(e.shape);
        if (e.node) e.node->Destroy();
        e.alive = false;
        e.node  = nullptr;
    }
}

void EnemyPool::DrawAll(SpriteBatch& sb) const noexcept {
    const f32 sz = kEnemyRadius * 2.0f;
    for (u32 i = 0; i < kMaxEnemies; ++i) {
        const EnemyInstance& e = _enemies[i];
        if (!e.alive || e.node == nullptr) continue;
        const FVec2 p = e.node->Local().position;
        sb.DrawRect(p.x - kEnemyRadius, p.y - kEnemyRadius, sz, sz, kColorEnemy);
    }
}

} // namespace hellofg
