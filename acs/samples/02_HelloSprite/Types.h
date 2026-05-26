// SPDX-License-Identifier: Apache-2.0
// HelloSprite — 共通型/定数/ユーティリティ。
//
// すべて `inline constexpr` で header 内に置くため、複数 TU に include
// しても 1 つのストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"

namespace hellosprite {

inline constexpr acs::u32 kTexSize        = 32;
inline constexpr acs::u32 kInitialSprites = 100;
inline constexpr acs::u32 kMaxSprites     = 4096;

// 中央が円型で透けてる、外周がカラーグラデの 32×32 テクスチャを生成。
// out は kTexSize * kTexSize * 4 (RGBA8) のバッファを指す。
void GenerateSpriteTexture(acs::u8* out) noexcept;

struct Sprite {
    acs::f32 x, y;
    acs::f32 vx, vy;
    acs::f32 size;
    acs::f32 r, g, b, a;
};

} // namespace hellosprite
