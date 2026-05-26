// SPDX-License-Identifier: Apache-2.0
// HelloECS — 共通型と定数。
//   Position / Velocity / Color  : World に登録する POD コンポーネント
//   SpawnEvent                   : MessageBroker で publish する型
//   kBallTexSize / GenerateBallTexture : 円型 particle テクスチャ生成
//   OnSpawnEvent                 : MessageBroker の購読コールバック
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hello04 {

// ---- コンポーネント定義 (POD ベース) ----------------------------------------
struct Position { acs::Vec2 v; };
struct Velocity { acs::Vec2 v; };
struct Color    { acs::f32 r, g, b; };

// ---- イベント (MessageBroker で publish) ------------------------------------
struct SpawnEvent { acs::u32 total; };

// ---- Particle テクスチャ生成 (中央白、外周透明の円) ------------------------
inline constexpr acs::u32 kBallTexSize = 32;
void GenerateBallTexture(acs::u8* out) noexcept;

// MessageBroker の購読コールバック (非捕捉関数)。SpawnEvent が来たらログに出す。
void OnSpawnEvent(const void* payload, void* user);

} // namespace hello04
