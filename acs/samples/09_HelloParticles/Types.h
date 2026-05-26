// SPDX-License-Identifier: Apache-2.0
// HelloParticles — 共通型 + 定数。
// パーティクルテクスチャサイズと preset 名一覧。
//
// `inline constexpr` で header に置くので、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"

namespace helloparticles {

// パーティクル粒テクスチャ (中央が明るい円型 glow)
inline constexpr acs::u32 kTexSize = 32;

// パーティクルプール上限
inline constexpr acs::u32 kParticleCapacity = 8192;

// 1 / 2 / 3 / 4 キーに対応する preset 名 (HUD 表示用)
inline constexpr const char* kPresetNames[] = {
    "[1] 火 (Fire)",
    "[2] 火花 (Sparks)",
    "[3] 噴水 (Fountain)",
    "[4] 煙 (Smoke)",
};
inline constexpr acs::u32 kPresetCount = 4;

} // namespace helloparticles
