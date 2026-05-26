// SPDX-License-Identifier: Apache-2.0
// HelloSky — 共通型 + 定数。
// 空のプリセット種別 (Day/Sunset/Night) と、対応する ambient 色を集約。
//
// 定数を `inline constexpr` で header 内に置く理由: 複数 TU から include しても
// ODR 違反にならず 1 つのストレージに resolve される (C++17 以降)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hellosky {

// 1/2/3 キーで切り替える Sky の preset 種別。
enum class SkyPreset : acs::u8 {
    Day    = 0,
    Sunset = 1,
    Night  = 2,
};

// preset 別の ambient 色。Sky の zenith をそのまま使うと明るすぎるので
// 各時間帯の雰囲気に合わせて減衰させた値を採用。
inline constexpr acs::FVec3 kAmbientDay   {0.20f, 0.22f, 0.30f};
inline constexpr acs::FVec3 kAmbientSunset{0.20f, 0.10f, 0.10f};
inline constexpr acs::FVec3 kAmbientNight {0.04f, 0.05f, 0.10f};

} // namespace hellosky
