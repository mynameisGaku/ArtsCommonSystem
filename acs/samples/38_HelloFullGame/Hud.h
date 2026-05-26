// SPDX-License-Identifier: Apache-2.0
// HelloFullGame — HUD モジュール。HP / スコア / Wave / FPS / flash オーバーレイの描画。
//
// 純粋に描画だけのモジュール。状態は呼び出し側 (GameplayScene) が持ち、
// 必要な値だけ受け取って FSpriteBatch に積む。
#pragma once

#include "gameframework/GameFramework.h"
#include "GameTypes.h"

namespace acs { class FSpriteBatch; }

namespace hellofg {

class GameplayScene;

class Hud {
public:
    // HUD レイヤを描画する。FSpriteBatch は呼び出し側で Begin/SetView 済み。
    // last_dt / fps_ema は FPS 表示用。
    void Draw(GameplayScene& scene, acs::FSpriteBatch& sb,
              acs::u32 sw, acs::u32 sh,
              acs::f32 last_dt, acs::f32 fps_ema) const noexcept;
};

} // namespace hellofg
