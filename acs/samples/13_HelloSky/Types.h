// SPDX-License-Identifier: Apache-2.0
// HelloSky — 共通型 + 定数。
// 空のプリセット種別 (Day/Sunset/Night) と、それぞれの ambient 色を
// 1 ヘッダに集約。
//
// `inline constexpr` で header 内に置くため、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Vec.h"

namespace hellosky {

// Sky の preset 種別 (1/2/3 キーで切り替える)
enum class SkyPreset : acs::u8 {
    Day    = 0,
    Sunset = 1,
    Night  = 2,
};

// 各 preset に対応する ambient 色 (Sky の zenith を弱めた相当)
inline constexpr acs::Vec3 kAmbientDay   {0.20f, 0.22f, 0.30f};
inline constexpr acs::Vec3 kAmbientSunset{0.20f, 0.10f, 0.10f};
inline constexpr acs::Vec3 kAmbientNight {0.04f, 0.05f, 0.10f};

} // namespace hellosky
