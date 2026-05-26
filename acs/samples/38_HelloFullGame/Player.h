// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Player モジュール。
// 自機 Node2D + FHealthId + 円形コリジョン + 入力ハンドリング + 射撃クールダウン。
//
// 設計メモ: Player は GameplayScene に所有させ、scene が tick / draw を呼ぶ。
// 当たり判定形状は Services().Physics() に登録するため、GameplayScene の Services()
// を介して触る。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "math/Vec.h"

namespace hellofg {

class GameplayScene;

// Player モジュール。Node + Health + Shape + 発射 CD をひとまとめにする。
class Player {
public:
    // OnEnter で呼ぶ。root に子として player node を生やし、Health/Shape も登録する。
    void Init(GameplayScene& scene, acs::game::Node2D& root,
              acs::game::HealthSystem& health) noexcept;

    // OnExit で呼ぶ。Node を root subtree から外して解体する。
    void Shutdown() noexcept;

    // 移動入力 + clamp + collision shape の追従。 perception 用に座標も返す。
    // 戻り値: 移動後の player 座標 (camera / perception 更新で使う)。
    acs::FVec2 UpdateMovement(GameplayScene& scene, acs::f32 dt) noexcept;

    // 発射ボタンが押されていれば cooldown を見て撃つ。
    void UpdateFire(GameplayScene& scene, acs::f32 dt) noexcept;

    // 敵との接触ダメージを 1 体ぶん適用しようとする。
    // 戻り値: true なら致死 (シーンが GameOver 遷移する)。
    bool TryTakeContactDamage(GameplayScene& scene,
                              acs::game::HealthSystem& health,
                              acs::FVec2 player_pos) noexcept;

    // ----- アクセサ -----
    acs::game::Node2D* Node()       noexcept { return _node; }
    const acs::game::Node2D* Node() const noexcept { return _node; }
    acs::FVec2          Position() const noexcept;
    acs::game::FHealthId  HealthHandle() const noexcept { return _health_id; }
    acs::game::FShapeId   Shape()        const noexcept { return _shape; }
    bool                 IsInvulnerable(const acs::game::HealthSystem& h) const noexcept;

private:
    acs::game::Node2D*    _node      = nullptr;
    acs::game::FHealthId   _health_id {};
    acs::game::FShapeId    _shape     {};
    acs::f32              _fire_cd   = 0.0f;
};

} // namespace hellofg
