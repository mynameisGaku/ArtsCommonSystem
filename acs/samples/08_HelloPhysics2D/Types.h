// SPDX-License-Identifier: Apache-2.0
// HelloPhysics2D — 共通型 + 定数。
// Ball POD と物理パラメータ (gravity / restitution / damping) を集約。
//
// `inline constexpr` で header に置くので、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hellophysics2d {

// ボールテクスチャ (1 枚を全 Ball で共有)
inline constexpr acs::u32 kBallTexSize = 64;

// 物理 / 描画パラメータ
inline constexpr acs::u32 kMaxBalls    = 200;
inline constexpr acs::f32 kGravity     = 600.0f;
inline constexpr acs::f32 kRestitution = 0.85f;
inline constexpr acs::f32 kDamping     = 0.999f;

// 1 個のボール状態
struct Ball {
    acs::FVec2 pos;
    acs::FVec2 vel;
    acs::f32  radius;
    acs::f32  r, g, b;
};

} // namespace hellophysics2d
