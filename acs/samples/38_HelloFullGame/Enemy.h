// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Enemy モジュール。固定容量の敵プール + AI tick + 描画。
//
// プール方式 (固定 kMaxEnemies スロット、swap-and-pop ではなく alive フラグ) を
// 採用しているのは、敵に handle (FNodeId / FHealthId / FShapeId) を持たせる必要があり
// 線形検索でも 64 体程度なら毎フレーム楽勝なため。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "math/Vec.h"

namespace acs { class FSpriteBatch; }

namespace hellofg {

class GameplayScene;

// 1 体ぶんの敵レコード。
struct EnemyInstance {
    acs::game::FNode2D*  node  = nullptr;
    acs::game::FHealthId hp    {};
    acs::game::FShapeId  shape {};
    acs::u32            wave_idx_at_spawn = 0;
    bool                alive = false;
};

class EnemyPool {
public:
    // 全スロットを空に戻す。OnEnter で呼ぶ。
    void Reset() noexcept;

    // OnExit で呼ぶ。Node の Destroy は scene 側で root subtree ごと行う。
    void Shutdown() noexcept;

    // 空きスロットを探して敵を生成。pool 満杯なら警告ログを出して何もしない。
    void Spawn(GameplayScene& scene,
               acs::game::FNode2D& root,
               acs::game::FHealthSystem& health,
               acs::u32 current_wave,
               acs::FVec2 pos) noexcept;

    // プレイヤーを追跡する単純 AI を全敵にかける。物理形状も追従させる。
    // 戻り値: 接触ヒットが致死だった場合 true (シーン側で GameOver 遷移を呼ぶ)。
    bool TickChaseAndContact(GameplayScene& scene,
                             acs::game::FHealthSystem& health,
                             acs::FVec2 player_pos,
                             acs::f32 dt) noexcept;

    // bullet が敵 i に当たった結果ダメージを与える。lethal なら敵を消す。
    // index が範囲外 / 既に死んでいる場合は何もしない。
    void ApplyHit(GameplayScene& scene,
                  acs::game::FHealthSystem& health,
                  acs::u32 target_id,
                  acs::f32 dmg) noexcept;

    // 描画 (world layer)。FSpriteBatch は GameplayScene::OnRender 側で Begin/End 済み。
    void DrawAll(acs::FSpriteBatch& sb) const noexcept;

    // hit test 用に raw 配列をそのまま渡す。
    const EnemyInstance* Data() const noexcept { return _enemies; }

private:
    EnemyInstance _enemies[kMaxEnemies] {};
};

} // namespace hellofg
