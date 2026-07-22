// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Gameplay scene。本編のゲームロジック。
//
// 役割: シーン全体のオーケストレータ。世界 / camera / Health / Score / Wave /
// Projectile / FParticles / Effects / FPerception / FTilemap を所有し、機能別
// モジュール (FPlayer / FEnemyPool / FBullets / FHitEffects / FHud) を tick / draw する。
//
// Pillar の使い分け:
//   - B ANode : player + enemy node。m_Root.UpdateTree で subtree 更新
//   - D FInputMap: WASD / Mouse / Fire
//   - E FCamera2D: smooth follow + trauma shake
//   - FCollisionWorld2D: FCircle 同士の衝突判定
//   - I/R: FHealthSystem / FScoreSystem / FWaveSpawner / FProjectileSystem
//          / FParticleEffectSystem / FEffectSystem
//   - L FPerception (sight): demo 用
//   - Q FTilemap : 床
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "Player.h"
#include "Enemy.h"
#include "Bullets.h"
#include "HitEffects.h"
#include "Hud.h"
#include "math/Vec.h"

namespace hellofg {

class FGameplayScene : public acs::game::FScene {
public:
    acs::game::ESvc WantedServices() const noexcept override {
        return acs::game::ESvc::Default2D
             | acs::game::ESvc::Camera2D
             | acs::game::ESvc::Physics2D;
    }

    void OnEnter()                  noexcept override;
    void OnExit()                   noexcept override;
    void OnUpdate(acs::f32 dt)      noexcept override;
    void OnFixedUpdate(acs::f32 dt) noexcept override;
    void OnRender(acs::game::FRenderContext& rc) noexcept override;

    // ----- 公開 (FGameOverScene 遷移時に score / 勝敗を読むため) -----
    acs::u64 FinalScore() const noexcept { return m_Score.CurrentScore(); }
    bool     DidWin()     const noexcept { return m_Victory; }

    // ----- モジュール間連携 (FPlayer / Enemy / FBullets / FHitEffects / WaveCallbacks 用) -----
    FPlayer&                       GetPlayer()      noexcept { return m_Player; }
    FEnemyPool&                    GetEnemies()     noexcept { return m_Enemies; }
    FBullets&                      GetBullets()     noexcept { return m_BulletsMod; }
    FHitEffects&                   GetHitEffects()  noexcept { return m_HitEffects; }
    acs::game::ANode&            GetRoot()        noexcept { return m_Root; }
    acs::game::FHealthSystem&      GetHealth()      noexcept { return m_Health; }
    acs::game::FScoreSystem&       GetScore()       noexcept { return m_Score; }
    acs::game::FWaveSpawner&       GetWaves()       noexcept { return m_Waves; }
    acs::game::FProjectileSystem&  GetProjectiles() noexcept { return m_Bullets; }
    acs::game::FParticleEffectSystem& GetParticles() noexcept { return m_Particles; }
    acs::game::FRandom&            GetRng()         noexcept { return m_Rng; }

    // ----- モジュール間コールバック (FPlayer → FHitEffects、Enemy → score / wave 通知) -----
    void OnPlayerHurt() noexcept;     // 接触ダメージ時のフィードバック発火
    void OnEnemyKilled() noexcept;    // 敵死亡時の score 加算 + Wave 通知
    void RequestGameOver(bool victory) noexcept;

    // マウス座標を world に変換。FBullets と FPlayer 共有。
    acs::FVec2 MouseWorld() const noexcept;

private:
    // ----- world state -----
    acs::game::ANode                m_Root;
    acs::game::FHealthSystem          m_Health;
    acs::game::FScoreSystem           m_Score;
    acs::game::FWaveSpawner           m_Waves;
    acs::game::FProjectileSystem      m_Bullets;     // ECS 側
    acs::game::FParticleEffectSystem  m_Particles;
    acs::game::FPerception            m_Perception;
    acs::game::FTilemap               m_Floor;

    // ----- 機能別モジュール -----
    FPlayer       m_Player;
    FEnemyPool    m_Enemies;
    FBullets      m_BulletsMod;
    FHitEffects   m_HitEffects;
    FHud          m_Hud;

    // wave 用の FSpawnRule 配列 (caller 所有、寿命 = scene 寿命)。
    acs::game::FSpawnRule m_WaveRules[kTotalWaves] {};
    acs::game::FWaveDef   m_WaveDefs [kTotalWaves] {};

    // 決定論 PRNG (敵スポーン位置の seed をシーンごと固定にしてリプレイ性確保)。
    acs::game::FRandom m_Rng{ 0x5A17C0DEu };

    bool       m_Victory       = false;
    bool       m_bGameOverReq = false;
    acs::f32   m_LastDt       = 1.0f / 60.0f;
    acs::f32   m_FpsEma       = 60.0f;
};

} // namespace hellofg
