// SPDX-License-Identifier: Apache-2.0
// HelloLights — 共通型 + 定数。
// 6 個のオブジェクト (cube/sphere mix) と 4 灯の点光源を持つ「暗い部屋」シーン
// の定数 / POD を集約。
//
// `inline constexpr` で header 内に置くため、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"
#include "math/Mat.h"
#include "math/Vec.h"

namespace hellolights {

// シーン上のオブジェクト個数 (cube と sphere が i&1 で交互配置される)
inline constexpr acs::u32 kCubeCount  = 6;
// 点光源の灯数 (4 色で円周状に回転)
inline constexpr acs::u32 kPointCount = 4;

// 1 個のオブジェクトインスタンス (cube または sphere、色、配置行列)
struct ObjectInst {
    acs::Mat4 model;
    acs::Vec3 color;
    bool      is_sphere;
};

} // namespace hellolights
