// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — Bullets モジュール。
// FProjectileSystem の初期化 / Tick / 描画 + 発射 API。
// hit test / on hit のコールバックは WaveCallbacks.cpp が C 関数で実装する。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"
#include "math/Vec.h"

namespace acs { class FSpriteBatch; }

namespace hellofg {

class GameplayScene;

class Bullets {
public:
    // FProjectileSystem を初期化し、弾の def を 1 つ登録する。
    // hit test / on hit コールバックは scene 側で setter を直接呼ぶ。
    void Init(acs::game::FProjectileSystem& sys) noexcept;

    // OnExit で呼ぶ。ClearAll。
    void Shutdown(acs::game::FProjectileSystem& sys) noexcept;

    // 1 発撃つ。dir_unit は正規化済みであること。SFX も鳴らす。
    void Fire(GameplayScene& scene, acs::FVec2 from, acs::FVec2 dir_unit) noexcept;

    // 描画 (world layer)。直前 dt を使って軌跡を 32 段の矩形で線化する。
    void DrawAll(const acs::game::FProjectileSystem& sys,
                 acs::FSpriteBatch& sb, acs::f32 last_dt) const noexcept;
};

} // namespace hellofg
