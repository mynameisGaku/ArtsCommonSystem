// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Gameplay scene。本編のゲームロジック。
//
// 役割: シーン全体のオーケストレータ。世界 / camera / Health / Score / Wave /
// Projectile / FParticles / Effects / CPerception / FTilemap を所有し、機能別
// モジュール (CPlayer / CEnemyPool / CBullets / CHitEffects / CHud) を tick / draw する。
//
// Pillar の使い分け:
//   - B ANode : player + enemy node。m_Root.UpdateTree で subtree 更新
//   - D FInputMap: WASD / Mouse / Fire
//   - E CCamera2D: smooth follow + trauma shake
//   - CCollisionWorld2D: FCircle 同士の衝突判定
//   - I/R: CHealthSystem / CScoreSystem / CWaveSpawner / CProjectileSystem
//          / CParticleEffectSystem / CEffectSystem
//   - L CPerception (sight): demo 用
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

class AGameplayScene : public acs::game::AScene {
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

    // ----- 公開 (AGameOverScene 遷移時に score / 勝敗を読むため) -----
    acs::u64 FinalScore() const noexcept { return m_Score.CurrentScore(); }
    bool     DidWin()     const noexcept { return m_Victory; }

    // ----- モジュール間連携 (CPlayer / Enemy / CBullets / CHitEffects / WaveCallbacks 用) -----
    CPlayer&                       GetPlayer()      noexcept { return m_Player; }
    CEnemyPool&                    GetEnemies()     noexcept { return m_Enemies; }
    CBullets&                      GetBullets()     noexcept { return m_BulletsMod; }
    CHitEffects&                   GetHitEffects()  noexcept { return m_HitEffects; }
    acs::game::ANode&            GetRoot()        noexcept { return m_Root; }
    acs::game::CHealthSystem&      GetHealth()      noexcept { return m_Health; }
    acs::game::CScoreSystem&       GetScore()       noexcept { return m_Score; }
    acs::game::CWaveSpawner&       GetWaves()       noexcept { return m_Waves; }
    acs::game::CProjectileSystem&  GetProjectiles() noexcept { return m_Bullets; }
    acs::game::CParticleEffectSystem& GetParticles() noexcept { return m_Particles; }
    acs::game::FRandom&            GetRng()         noexcept { return m_Rng; }

    // ----- モジュール間コールバック (CPlayer → CHitEffects、Enemy → score / wave 通知) -----
    void OnPlayerHurt() noexcept;     // 接触ダメージ時のフィードバック発火
    void OnEnemyKilled() noexcept;    // 敵死亡時の score 加算 + Wave 通知
    void RequestGameOver(bool victory) noexcept;

    // マウス座標を world に変換。CBullets と CPlayer 共有。
    acs::FVec2 MouseWorld() const noexcept;

private:
    // ----- world state -----
    acs::game::ANode                m_Root;
    acs::game::CHealthSystem          m_Health;
    acs::game::CScoreSystem           m_Score;
    acs::game::CWaveSpawner           m_Waves;
    acs::game::CProjectileSystem      m_Bullets;     // ECS 側
    acs::game::CParticleEffectSystem  m_Particles;
    acs::game::CPerception            m_Perception;
    acs::game::FTilemap               m_Floor;

    // ----- 機能別モジュール -----
    CPlayer       m_Player;
    CEnemyPool    m_Enemies;
    CBullets      m_BulletsMod;
    CHitEffects   m_HitEffects;
    CHud          m_Hud;

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
