// SPDX-License-Identifier: Apache-2.0
// HelloAnimation — 共通型 + 定数。
// 4 ボーンの「ヘビ風」円柱を手続き生成するためのパラメータ (ボーン数 / 縦
// 方向リング数 / 半径 / アニメ duration 等) を集約。
//
// `inline constexpr` で header 内に置くため、複数 TU に include しても 1 つの
// ストレージに resolve される (C++17)。
#pragma once

#include "foundation/Types.h"

namespace helloanim {

// ボーン数 (ヘビ風 vertical chain)
inline constexpr acs::u32 kBoneCount    = 4;
// 円周分割数 (1 リング当たりの頂点数)
inline constexpr acs::u32 kSegmentCount = 16;
// 縦方向リング数 (多いほど滑らか)
inline constexpr acs::u32 kRingCount    = 33;

// 円柱の幾何パラメータ
inline constexpr acs::f32 kHeight       = 4.0f;     // 円柱の全長
inline constexpr acs::f32 kRadiusBase   = 0.45f;
inline constexpr acs::f32 kRadiusTip    = 0.18f;

// アニメーション
inline constexpr acs::u32 kAnimKeys     = 33;       // キーフレーム数
inline constexpr acs::f32 kAnimDuration = 2.0f;     // 1 ループの秒数
inline constexpr acs::f32 kAnimAngleMax = 0.45f;    // 振り幅 (rad)

} // namespace helloanim
