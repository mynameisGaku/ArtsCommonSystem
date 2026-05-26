// SPDX-License-Identifier: Apache-2.0
// HelloTriangle — 共通型 / 頂点データ。
//
// 頂点フォーマット (POSITION + COLOR) と 3 頂点ぶんのカラフルな三角形を
// inline constexpr 配列で持つ。複数 TU から include されても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"

namespace hellotri {

// 1 頂点 = 位置 (FVec3) + 色 (FVec3)
struct FVertex {
    acs::f32 pos[3];
    acs::f32 col[3];
};

// 3 頂点 = 1 三角形。RGB を各頂点に割り当てて補間色を見せる。
inline constexpr FVertex kTriangleVertices[3] = {
    {{ 0.0f,  0.7f, 0.0f}, {1.0f, 0.0f, 0.0f}},  // 上   (赤)
    {{ 0.7f, -0.7f, 0.0f}, {0.0f, 1.0f, 0.0f}},  // 右下 (緑)
    {{-0.7f, -0.7f, 0.0f}, {0.0f, 0.0f, 1.0f}},  // 左下 (青)
};

} // namespace hellotri
