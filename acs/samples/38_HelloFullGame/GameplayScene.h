// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Gameplay scene。本編のゲームロジック。
//
// 役割: シーン全体のオーケストレータ。世界 / camera / Health / Score / Wave /
// Projectile / FParticles / Effects / FPerception / FTilemap を所有し、機能別
// モジュール (Player / EnemyPool / Bullets / HitEffects / Hud) を tick / draw する。
//
// Pillar の使い分け:
//   - B FNode2D : player + enemy node。m_Root.UpdateTree で subtree 更新
//   - D FInputMap: WASD / Mouse / Fire
//   - E FCamera2D: smooth follow + trauma shake
//   - F FCollisionWorld2D: Circle vs Circle
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

class GameplayScene : public acs::game::Scene {
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
    void OnRender(acs::game::RenderContext& rc) noexcept override;

    // ----- 公開 (GameOverScene 遷移時に score / 勝敗を読むため) -----
    acs::u64 FinalScore() const noexcept { return m_Score.CurrentScore(); }
    bool     DidWin()     const noexcept { return m_Victory; }

    // ----- モジュール間連携 (Player / Enemy / Bullets / HitEffects / WaveCallbacks 用) -----
    Player&                       GetPlayer()      noexcept { return m_Player; }
    EnemyPool&                    GetEnemies()     noexcept { return m_Enemies; }
    Bullets&                      GetBullets()     noexcept { return m_BulletsMod; }
    HitEffects&                   GetHitEffects()  noexcept { return m_HitEffects; }
    acs::game::FNode2D&            GetRoot()        noexcept { return m_Root; }
    acs::game::FHealthSystem&      GetHealth()      noexcept { return m_Health; }
    acs::game::FScoreSystem&       GetScore()       noexcept { return m_Score; }
    acs::game::FWaveSpawner&       GetWaves()       noexcept { return m_Waves; }
    acs::game::FProjectileSystem&  GetProjectiles() noexcept { return m_Bullets; }
    acs::game::FParticleEffectSystem& GetParticles() noexcept { return m_Particles; }
    acs::game::FRandom&            GetRng()         noexcept { return m_Rng; }

    // ----- モジュール間コールバック (Player → HitEffects、Enemy → score / wave 通知) -----
    void OnPlayerHurt() noexcept;     // 接触ダメージ時のフィードバック発火
    void OnEnemyKilled() noexcept;    // 敵死亡時の score 加算 + Wave 通知
    void RequestGameOver(bool victory) noexcept;

    // マウス座標を world に変換。Bullets と Player 共有。
    acs::FVec2 MouseWorld() const noexcept;

private:
    // ----- world state -----
    acs::game::FNode2D                m_Root;
    acs::game::FHealthSystem          m_Health;
    acs::game::FScoreSystem           m_Score;
    acs::game::FWaveSpawner           m_Waves;
    acs::game::FProjectileSystem      m_Bullets;     // ECS 側
    acs::game::FParticleEffectSystem  m_Particles;
    acs::game::FPerception            m_Perception;
    acs::game::FTilemap               m_Floor;

    // ----- 機能別モジュール -----
    Player       m_Player;
    EnemyPool    m_Enemies;
    Bullets      m_BulletsMod;
    HitEffects   m_HitEffects;
    Hud          m_Hud;

    // wave 用の SpawnRule 配列 (caller 所有、寿命 = scene 寿命)。
    acs::game::SpawnRule m_WaveRules[kTotalWaves] {};
    acs::game::FWaveDef   m_WaveDefs [kTotalWaves] {};

    // 決定論 PRNG (敵スポーン位置の seed をシーンごと固定にしてリプレイ性確保)。
    acs::game::FRandom m_Rng{ 0x5A17C0DEu };

    bool       m_Victory       = false;
    bool       m_bGameOverReq = false;
    acs::f32   m_LastDt       = 1.0f / 60.0f;
    acs::f32   m_FpsEma       = 60.0f;
};

} // namespace hellofg
