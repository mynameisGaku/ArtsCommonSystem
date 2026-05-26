// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Gameplay scene。本編のゲームロジック。
//
// 役割: シーン全体のオーケストレータ。世界 / camera / Health / Score / Wave /
// Projectile / Particles / Effects / Perception / Tilemap を所有し、機能別
// モジュール (Player / EnemyPool / Bullets / HitEffects / Hud) を tick / draw する。
//
// Pillar の使い分け:
//   - B Node2D : player + enemy node。_root.UpdateTree で subtree 更新
//   - D InputMap: WASD / Mouse / Fire
//   - E Camera2D: smooth follow + trauma shake
//   - F CollisionWorld2D: Circle vs Circle
//   - I/R: HealthSystem / ScoreSystem / WaveSpawner / ProjectileSystem
//          / ParticleEffectSystem / EffectSystem
//   - L Perception (sight): demo 用
//   - Q Tilemap : 床
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
    acs::u64 FinalScore() const noexcept { return _score.CurrentScore(); }
    bool     DidWin()     const noexcept { return _victory; }

    // ----- モジュール間連携 (Player / Enemy / Bullets / HitEffects / WaveCallbacks 用) -----
    Player&                       GetPlayer()      noexcept { return _player; }
    EnemyPool&                    GetEnemies()     noexcept { return _enemies; }
    Bullets&                      GetBullets()     noexcept { return _bullets_mod; }
    HitEffects&                   GetHitEffects()  noexcept { return _hit_effects; }
    acs::game::Node2D&            GetRoot()        noexcept { return _root; }
    acs::game::HealthSystem&      GetHealth()      noexcept { return _health; }
    acs::game::ScoreSystem&       GetScore()       noexcept { return _score; }
    acs::game::WaveSpawner&       GetWaves()       noexcept { return _waves; }
    acs::game::ProjectileSystem&  GetProjectiles() noexcept { return _bullets; }
    acs::game::ParticleEffectSystem& GetParticles() noexcept { return _particles; }
    acs::game::Random&            GetRng()         noexcept { return _rng; }

    // ----- モジュール間コールバック (Player → HitEffects、Enemy → score / wave 通知) -----
    void OnPlayerHurt() noexcept;     // 接触ダメージ時のフィードバック発火
    void OnEnemyKilled() noexcept;    // 敵死亡時の score 加算 + Wave 通知
    void RequestGameOver(bool victory) noexcept;

    // マウス座標を world に変換。Bullets と Player 共有。
    acs::Vec2 MouseWorld() const noexcept;

private:
    // ----- world state -----
    acs::game::Node2D                _root;
    acs::game::HealthSystem          _health;
    acs::game::ScoreSystem           _score;
    acs::game::WaveSpawner           _waves;
    acs::game::ProjectileSystem      _bullets;     // ECS 側
    acs::game::ParticleEffectSystem  _particles;
    acs::game::Perception            _perception;
    acs::game::Tilemap               _floor;

    // ----- 機能別モジュール -----
    Player       _player;
    EnemyPool    _enemies;
    Bullets      _bullets_mod;
    HitEffects   _hit_effects;
    Hud          _hud;

    // wave 用の SpawnRule 配列 (caller 所有、寿命 = scene 寿命)。
    acs::game::SpawnRule _wave_rules[kTotalWaves] {};
    acs::game::WaveDef   _wave_defs [kTotalWaves] {};

    // 決定論 PRNG (敵スポーン位置の seed をシーンごと固定にしてリプレイ性確保)。
    acs::game::Random _rng{ 0x5A17C0DEu };

    bool       _victory       = false;
    bool       _game_over_req = false;
    acs::f32   _last_dt       = 1.0f / 60.0f;
    acs::f32   _fps_ema       = 60.0f;
};

} // namespace hellofg
