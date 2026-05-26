// SPDX-License-Identifier: Apache-2.0
// HelloLights — 共通型 + 定数。
//
// `inline constexpr` で header 直置きにしているのは、複数 TU から include
// しても単一ストレージに resolve させて ODR 違反を避けるため (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"

namespace hellolights {

// cube と sphere を i&1 で交互配置する都合上、偶数の方が見た目が揃う。
inline constexpr acs::u32 kCubeCount  = 6;
// 色を 4 色 (赤/緑/青/黄) 割り当てる前提なので 4 灯固定。
inline constexpr acs::u32 kPointCount = 4;

struct ObjectInst {
    acs::FMat4 model;
    acs::FVec3 color;
    bool      is_sphere;
};

} // namespace hellolights
