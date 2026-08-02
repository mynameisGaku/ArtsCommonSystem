// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — CPlayer モジュール。
// 自機 ANode + FHealthId + 円形コリジョン + 入力ハンドリング + 射撃クールダウン。
//
// 設計メモ: CPlayer は AGameplayScene に所有させ、scene が tick / draw を呼ぶ。
// 当たり判定形状は Services().Physics() に登録するため、AGameplayScene の Services()
// を介して触る。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "math/Vec.h"

namespace hellofg {

class AGameplayScene;

// CPlayer モジュール。Node + Health + Shape + 発射 CD をひとまとめにする。
class CPlayer {
public:
    // OnEnter で呼ぶ。root に子として player node を生やし、Health/Shape も登録する。
    void Init(AGameplayScene& scene, acs::game::ANode& root,
              acs::game::CHealthSystem& health) noexcept;

    // OnExit で呼ぶ。Node を root subtree から外して解体する。
    void Shutdown() noexcept;

    // 移動入力 + clamp + collision shape の追従。 perception 用に座標も返す。
    // 戻り値: 移動後の player 座標 (camera / perception 更新で使う)。
    acs::FVec2 UpdateMovement(AGameplayScene& scene, acs::f32 dt) noexcept;

    // 発射ボタンが押されていれば cooldown を見て撃つ。
    void UpdateFire(AGameplayScene& scene, acs::f32 dt) noexcept;

    // 敵との接触ダメージを 1 体ぶん適用しようとする。
    // 戻り値: true なら致死 (シーンが GameOver 遷移する)。
    bool TryTakeContactDamage(AGameplayScene& scene,
                              acs::game::CHealthSystem& health,
                              acs::FVec2 player_pos) noexcept;

    // ----- アクセサ -----
    acs::game::ANode* Node()       noexcept { return m_Node; }
    const acs::game::ANode* Node() const noexcept { return m_Node; }
    acs::FVec2          Position() const noexcept;
    acs::game::FHealthId  HealthHandle() const noexcept { return m_HealthId; }
    acs::game::FShapeId   Shape()        const noexcept { return m_Shape; }
    bool                 IsInvulnerable(const acs::game::CHealthSystem& h) const noexcept;

private:
    acs::game::ANode*    m_Node      = nullptr;
    acs::game::FHealthId   m_HealthId {};
    acs::game::FShapeId    m_Shape     {};
    acs::f32              m_FireCd   = 0.0f;
};

} // namespace hellofg
