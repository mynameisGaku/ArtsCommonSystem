// SPDX-License-Identifier: Apache-2.0
// HelloShadows — 共通型 + 定数。
// シャドウキャスター 8 個 (柱 4 本 + 色違い球 4 個) の POD と定数を集約。
//
// `inline constexpr` で header 内に置くため、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"

namespace helloshadows {

// シャドウキャスター個数 (柱 4 + 球 4)
inline constexpr acs::u32 kCasterCount = 8;

// シャドウマップ解像度 (size x size の R32 depth texture)
inline constexpr acs::u32 kShadowMapSize = 2048;

// 1 個のシャドウキャスタ (柱 or 球、色、配置行列)
struct FCasterInst {
    acs::FMat4 model;
    acs::FVec3 base_color;
    bool      is_sphere;
};

} // namespace helloshadows
