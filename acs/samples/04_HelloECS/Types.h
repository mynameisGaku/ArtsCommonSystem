// SPDX-License-Identifier: Apache-2.0
// HelloECS — 共通の POD コンポーネント / イベント / 補助関数。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hello04 {

// World に登録する POD コンポーネント。SparseSet ストレージに直接コピーされる
// ため自明な値型のままにしておく (継承や仮想関数を付けない)。
struct FPosition { acs::FVec2 v; };
struct FVelocity { acs::FVec2 v; };
struct FColor    { acs::f32 r, g, b; };

// MessageBroker で publish するイベント型。POD ならどんな型でも publish できる。
struct FSpawnEvent { acs::u32 total; };

// 中央が白く外周が透明な円型 RGBA8 テクスチャを生成 (粒子描画用)。
// out は kBallTexSize * kBallTexSize * 4 バイトを指すこと。
inline constexpr acs::u32 kBallTexSize = 32;
void GenerateBallTexture(acs::u8* out) noexcept;

// MessageBroker の購読コールバック。signature は MessageBroker.h の
// MessageCallback typedef に合わせる必要があるため引数を素の void* で受け取る。
void OnSpawnEvent(const void* payload, void* user) noexcept;

} // namespace hello04
