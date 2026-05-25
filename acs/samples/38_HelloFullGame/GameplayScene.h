// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Gameplay scene。本編のゲームロジック。
//
// Pillar の使い分け:
//   - Pillar B Node2D: player + enemy node、_root.UpdateTree で subtree 更新
//   - Pillar D InputMap: WASD / Mouse / Fire
//   - Pillar E Camera2D: smooth follow + trauma shake
//   - Pillar F CollisionWorld2D: 当たり判定 (Circle vs Circle)
//   - Pillar I/R: HealthSystem / ScoreSystem / WaveSpawner / ProjectileSystem
//                 / ParticleEffectSystem / EffectSystem
//   - Pillar L: Perception (sight、demo 用)
//   - Pillar Q: Tilemap (床)
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
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

    // 公開: GameOverScene が遷移時に最終 score / 勝敗を読むため
    acs::u64 FinalScore() const noexcept { return _score.CurrentScore(); }
    bool     DidWin()     const noexcept { return _victory; }

private:
    void SpawnEnemy(acs::Vec2 pos) noexcept;
    void FireBullet(acs::Vec2 from, acs::Vec2 dir_unit) noexcept;
    void TriggerHitEffect(acs::Vec2 pos) noexcept;
    void RequestGameOver(bool victory) noexcept;
    acs::Vec2 MouseWorld() const noexcept;

    // WaveSpawner / ProjectileSystem 用 C 関数ポインタブリッジ
    static void OnWaveSpawn(void* user, const char* enemy_id, acs::Vec2 spawn_pos) noexcept;
    static void OnWaveState(void* user, acs::u32 wave_index,
                            acs::game::EWaveState from, acs::game::EWaveState to) noexcept;
    static bool OnHitTest  (void* user, const acs::game::ProjectileInstance& p,
                            acs::u32& out_target, acs::f32& out_dmg) noexcept;
    static void OnHit      (void* user, acs::game::ProjectileId id, const char* def_id,
                            acs::u32 target_id, acs::f32 dmg) noexcept;

    // ----- World state -----
    acs::game::Node2D                _root;
    acs::game::Node2D*               _player_node = nullptr;
    acs::game::HealthId              _player_health{};
    acs::game::HealthSystem          _health;
    acs::game::ScoreSystem           _score;
    acs::game::WaveSpawner           _waves;
    acs::game::ProjectileSystem      _bullets;
    acs::game::ParticleEffectSystem  _particles;
    acs::game::EffectSystem          _fx;
    acs::game::EmitterHandle         _hit_emitter;
    acs::game::Perception            _perception;
    acs::game::Tilemap               _floor;

    // 敵 instance テーブル (固定容量、Pool 風 swap-and-pop)
    struct Enemy {
        acs::game::Node2D*  node     = nullptr;
        acs::game::HealthId hp{};
        acs::game::ShapeId  shape{};
        acs::u32            wave_idx_at_spawn = 0;
        bool                alive = false;
    };
    Enemy _enemies[kMaxEnemies] {};

    // wave 用の SpawnRule 配列 (caller 所有、寿命 = scene 寿命)
    acs::game::SpawnRule _wave_rules[kTotalWaves] {};
    acs::game::WaveDef   _wave_defs [kTotalWaves] {};

    acs::game::Random _rng{ 0x5A17C0DEu };  // 決定論 PRNG

    acs::game::ShapeId  _player_shape;
    acs::f32            _fire_cd        = 0.0f;
    bool                _victory        = false;
    bool                _game_over_req  = false;
    acs::f32            _last_dt        = 1.0f / 60.0f; // FPS 表示用
    acs::f32            _fps_ema        = 60.0f;
};

} // namespace hellofg
