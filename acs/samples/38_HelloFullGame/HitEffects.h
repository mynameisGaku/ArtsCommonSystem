// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — HitEffects モジュール。
// ParticleEffectSystem + EffectSystem (shake / flash) + 単一 burst emitter。
// プレイヤー被弾 / 敵被弾の 2 経路で異なる演出を出す。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "math/Vec.h"

namespace acs { class SpriteBatch; }

namespace hellofg {

class GameplayScene;

class HitEffects {
public:
    // OnEnter で呼ぶ。emitter 1 個を非アクティブで作る (Burst でだけ吹く)。
    void Init(acs::game::ParticleEffectSystem& particles) noexcept;

    // OnExit で呼ぶ。ClearAll。
    void Shutdown(acs::game::ParticleEffectSystem& particles) noexcept;

    // tick: 自前の EffectSystem を進めて、camera にシェイクを流す。
    void Tick(GameplayScene& scene, acs::f32 dt) noexcept;

    // プレイヤーが敵に触れたときの強めシェイク + パーティクル。
    void TriggerPlayerHurt(GameplayScene& scene, acs::FVec2 pos) noexcept;

    // 敵への弾命中。柔らかいシェイク + 黄色フラッシュ + パーティクル + SFX。
    void TriggerEnemyHit(GameplayScene& scene, acs::FVec2 pos) noexcept;

    // 描画 (world layer)。パーティクル群をパティクルライフ進行に応じて減衰描画。
    void DrawParticles(const acs::game::ParticleEffectSystem& particles,
                       acs::SpriteBatch& sb) const noexcept;

    // HUD 描画で使う: 直近 flash の色と強度。
    acs::game::EffectSystem&       Fx()       noexcept { return _fx; }
    const acs::game::EffectSystem& Fx() const noexcept { return _fx; }

private:
    acs::game::FEmitterHandle  _hit_emitter {};
    acs::game::EffectSystem   _fx;
};

} // namespace hellofg
