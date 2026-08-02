// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — CWaveSpawner / CProjectileSystem 用 C 関数ブリッジ。
//
// ACS は STL を使わないので、std::function ではなく C 関数 + user pointer 形式の
// コールバックを取る。`void*` から AGameplayScene* に restore し、各モジュールに
// 振り分ける役回り。
#pragma once

#include "gameframework/GameFramework.h"
#include "math/Vec.h"

namespace hellofg {

class AGameplayScene;

// ----- CWaveSpawner -----
// 敵が湧くタイミング。enemy_id / spawn_pos は無視し、4 辺ランダムに置く。
void WaveOnSpawn(void* user, const char* enemy_id, acs::FVec2 spawn_pos) noexcept;

// wave 状態遷移ログ + 全 wave 完了で勝利遷移。
void WaveOnState(void* user, acs::u32 wave_index,
                 acs::game::EWaveState from, acs::game::EWaveState to) noexcept;

// ----- CProjectileSystem -----
// 弾の進路上に敵がいるか線形探索。最初に当たる敵を out_target / out_dmg に返す。
bool ProjectileOnHitTest(void* user, const acs::game::FProjectileInstance& p,
                         acs::u32& out_target, acs::f32& out_dmg) noexcept;

// 弾が敵に当たった瞬間に呼ばれる。CEnemyPool::ApplyHit に流す。
void ProjectileOnHit(void* user, acs::game::FProjectileId id, const char* def_id,
                     acs::u32 target_id, acs::f32 dmg) noexcept;

} // namespace hellofg
